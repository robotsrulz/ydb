#include "yql_kikimr_provider_impl.h"

#include <ydb/library/yql/core/type_ann/type_ann_impl.h>
#include <ydb/library/yql/core/type_ann/type_ann_list.h>
#include <ydb/library/yql/core/yql_expr_optimize.h>
#include <ydb/library/yql/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/providers/common/provider/yql_provider.h>

namespace NYql {
namespace {

using namespace NCommon;
using namespace NNodes;

const TString DocApiTableVersionAttribute = "__document_api_version";

const TTypeAnnotationNode* GetExpectedRowType(const TKikimrTableDescription& tableDesc,
    const TVector<TString>& columns, const TPosition& pos, TExprContext& ctx)
{
    TVector<const TItemExprType*> expectedRowTypeItems;
    for (auto& column : columns) {
        auto columnType = tableDesc.GetColumnType(column);

        if (!columnType) {
            ctx.AddError(TIssue(pos, TStringBuilder()
                << "No such column: " << column << ", table: "
                << FullTableName(tableDesc.Metadata->Cluster, tableDesc.Metadata->Name)));
            return nullptr;
        }

        expectedRowTypeItems.push_back(ctx.MakeType<TItemExprType>(column, columnType));
    }

    const TTypeAnnotationNode* expectedRowType = ctx.MakeType<TStructExprType>(expectedRowTypeItems);
    return expectedRowType;
}

const TTypeAnnotationNode* GetExpectedRowType(const TKikimrTableDescription& tableDesc,
    const TStructExprType& structType, const TPosition& pos, TExprContext& ctx)
{
    TVector<TString> columns;
    for (auto& item : structType.GetItems()) {
        columns.push_back(TString(item->GetName()));
    }

    return GetExpectedRowType(tableDesc, columns, pos, ctx);
}

IGraphTransformer::TStatus ConvertTableRowType(TExprNode::TPtr& input, const TKikimrTableDescription& tableDesc,
    TExprContext& ctx)
{
    YQL_ENSURE(input->GetTypeAnn());

    const TTypeAnnotationNode* actualType;
    switch (input->GetTypeAnn()->GetKind()) {
        case ETypeAnnotationKind::List:
            actualType = input->GetTypeAnn()->Cast<TListExprType>()->GetItemType();
            break;
        case ETypeAnnotationKind::Stream:
            actualType = input->GetTypeAnn()->Cast<TStreamExprType>()->GetItemType();
            break;
        default:
            actualType = input->GetTypeAnn();
            break;
    }

    YQL_ENSURE(actualType->GetKind() == ETypeAnnotationKind::Struct);
    auto rowType = actualType->Cast<TStructExprType>();

    auto pos = ctx.GetPosition(input->Pos());
    auto expectedType = GetExpectedRowType(tableDesc, *rowType, pos, ctx);
    if (!expectedType) {
        return IGraphTransformer::TStatus::Error;
    }

    switch (input->GetTypeAnn()->GetKind()) {
        case ETypeAnnotationKind::List:
            expectedType = ctx.MakeType<TListExprType>(expectedType);
            break;
        case ETypeAnnotationKind::Stream:
            expectedType = ctx.MakeType<TStreamExprType>(expectedType);
            break;
        default:
            break;
    }

    auto convertStatus = TryConvertTo(input, *expectedType, ctx);

    if (convertStatus.Level == IGraphTransformer::TStatus::Error) {
        ctx.AddError(TIssue(pos, TStringBuilder()
            << "Row type mismatch for table: "
            << FullTableName(tableDesc.Metadata->Cluster, tableDesc.Metadata->Name)));
        return IGraphTransformer::TStatus::Error;
    }

    return convertStatus;
}

class TKiSourceTypeAnnotationTransformer : public TKiSourceVisitorTransformer {
public:
    TKiSourceTypeAnnotationTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx, TTypeAnnotationContext& types)
        : SessionCtx(sessionCtx)
        , Types(types) {}

private:
    TStatus HandleKiRead(TKiReadBase node, TExprContext& ctx) override {
        auto cluster = TString(node.DataSource().Cluster());

        TKikimrKey key(ctx);
        if (!key.Extract(node.TableKey().Ref())) {
            return TStatus::Error;
        }

        switch (key.GetKeyType()) {
            case TKikimrKey::Type::Table:
            {
                auto readTable = node.Cast<TKiReadTable>();

                const TKikimrTableDescription* tableDesc;
                if ((tableDesc = SessionCtx->Tables().EnsureTableExists(cluster, key.GetTablePath(), node.Pos(), ctx)) == nullptr) {
                    return TStatus::Error;
                }

                if (const auto& view = key.GetView()) {
                    if (!ValidateTableHasIndex(tableDesc->Metadata, ctx, node.Pos())) {
                        return TStatus::Error;
                    }
                    if (tableDesc->Metadata->GetIndexMetadata(view.GetRef()).first == nullptr) {
                        ctx.AddError(YqlIssue(ctx.GetPosition(node.Pos()), TIssuesIds::KIKIMR_SCHEME_ERROR, TStringBuilder()
                            << "Required global index not found, index name: " << view.GetRef()));
                        return TStatus::Error;
                    }
                }
                bool sysColumnsEnabled = SessionCtx->Config().SystemColumnsEnabled();
                auto selectType = GetReadTableRowType(
                    ctx, SessionCtx->Tables(), TString(readTable.DataSource().Cluster()), key.GetTablePath(),
                    readTable.GetSelectColumns(ctx, SessionCtx->Tables(), sysColumnsEnabled), sysColumnsEnabled
                );

                if (!selectType) {
                    return TStatus::Error;
                }

                if (HasSetting(readTable.Settings().Ref(), "unwrap_values")) {
                    TVector<const TItemExprType*> unwrappedItems;
                    for (auto* item : selectType->Cast<TStructExprType>()->GetItems()) {
                        auto unwrappedType = item->GetItemType()->Cast<TOptionalExprType>()->GetItemType();
                        auto newItemType = ctx.MakeType<TItemExprType>(item->GetName(), unwrappedType);
                        YQL_ENSURE(newItemType->Validate(node.Pos(), ctx));
                        unwrappedItems.push_back(newItemType);
                    }
                    auto newStructType = ctx.MakeType<TStructExprType>(unwrappedItems);
                    YQL_ENSURE(newStructType->Validate(node.Pos(), ctx));
                    selectType = newStructType;
                }

                auto listSelectType = ctx.MakeType<TListExprType>(selectType);

                TTypeAnnotationNode::TListType children;
                children.push_back(node.World().Ref().GetTypeAnn());
                children.push_back(listSelectType);
                auto tupleAnn = ctx.MakeType<TTupleExprType>(children);
                node.Ptr()->SetTypeAnn(tupleAnn);

                YQL_ENSURE(tableDesc->Metadata->ColumnOrder.size() == tableDesc->Metadata->Columns.size());
                return Types.SetColumnOrder(node.Ref(), tableDesc->Metadata->ColumnOrder, ctx);
            }

            case TKikimrKey::Type::TableList:
            {
                auto tableListAnnotation = BuildCommonTableListType(ctx);
                TTypeAnnotationNode::TListType children;
                children.push_back(node.World().Ref().GetTypeAnn());
                children.push_back(tableListAnnotation);
                node.Ptr()->SetTypeAnn(ctx.MakeType<TTupleExprType>(children));
                return TStatus::Ok;
            }

            case TKikimrKey::Type::TableScheme:
            {
                auto tableDesc = SessionCtx->Tables().EnsureTableExists(cluster, key.GetTablePath(), node.Pos(), ctx);
                if (!tableDesc) {
                    return TStatus::Error;
                }

                TTypeAnnotationNode::TListType children;
                children.push_back(node.World().Ref().GetTypeAnn());
                children.push_back(ctx.MakeType<TDataExprType>(EDataSlot::Yson));
                node.Ptr()->SetTypeAnn(ctx.MakeType<TTupleExprType>(children));
                return TStatus::Ok;
            }

            case TKikimrKey::Type::Role:
            {
                return TStatus::Ok;
            }
        }

        return TStatus::Error;
    }

    TStatus HandleRead(TExprBase node, TExprContext& ctx) override {
        ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), "Failed to annotate Read!, IO rewrite should handle this"));
        return TStatus::Error;
    }

    TStatus HandleLength(TExprBase node, TExprContext& ctx) override {
        Y_UNUSED(node);
        Y_UNUSED(ctx);
        return TStatus::Error;
    }

    TStatus HandleConfigure(TExprBase node, TExprContext& ctx) override {
        if (!EnsureWorldType(*node.Ref().Child(0), ctx)) {
            return TStatus::Error;
        }

        node.Ptr()->SetTypeAnn(node.Ref().Child(0)->GetTypeAnn());
        return TStatus::Ok;
    }

private:
    TIntrusivePtr<TKikimrSessionContext> SessionCtx;
    TTypeAnnotationContext& Types;
};

namespace {
    std::function<void(TPositionHandle pos, const TString& column, const TString& message)> GetColumnTypeErrorFn(TExprContext& ctx) {
        auto columnTypeError = [&ctx](TPositionHandle pos, const TString& column, const TString& message) {
            ctx.AddError(YqlIssue(ctx.GetPosition(pos), TIssuesIds::KIKIMR_BAD_COLUMN_TYPE,
                TStringBuilder() << "Invalid type for column: " << column << ". " << message));
        };
        return columnTypeError;
    }

    bool ValidateColumnDataType(const TDataExprType* type, const TExprBase& typeNode, const TString& columnName,
            TExprContext& ctx) {
        auto columnTypeError = GetColumnTypeErrorFn(ctx);
        switch (type->GetSlot()) {
        case EDataSlot::Decimal:
            if (const auto dataExprParamsType = dynamic_cast<const TDataExprParamsType*>(type)) {
                if (dataExprParamsType->GetParamOne() != "22") {
                    columnTypeError(typeNode.Pos(), columnName, TStringBuilder() << "Bad decimal precision \""
                        << dataExprParamsType->GetParamOne() << "\". Only Decimal(22,9) is supported for table columns");
                    return false;
                }
                if (dataExprParamsType->GetParamTwo() != "9") {
                    columnTypeError(typeNode.Pos(), columnName, TStringBuilder() << "Bad decimal scale \""
                        << dataExprParamsType->GetParamTwo() << "\". Only Decimal(22,9) is supported for table columns");
                    return false;
                }
            }
            break;

        default:
            break;
        }
        return true;
    }
}

class TKiSinkTypeAnnotationTransformer : public TKiSinkVisitorTransformer
{
public:
    TKiSinkTypeAnnotationTransformer(TIntrusivePtr<IKikimrGateway> gateway,
        TIntrusivePtr<TKikimrSessionContext> sessionCtx)
        : Gateway(gateway)
        , SessionCtx(sessionCtx) {}

private:
    virtual TStatus HandleClusterConfig(TKiClusterConfig node, TExprContext& ctx) override {
        if (!EnsureTuple(node.GrpcData().Ref(), ctx)) {
            return TStatus::Error;
        }

        if (!EnsureAtom(node.TvmId().Ref(), ctx)) {
            return TStatus::Error;
        }

        node.Ptr()->SetTypeAnn(ctx.MakeType<TUnitExprType>());
        return TStatus::Ok;
    }

    virtual TStatus HandleWriteTable(TKiWriteTable node, TExprContext& ctx) override {
        if (!EnsureWorldType(node.World().Ref(), ctx)) {
            return TStatus::Error;
        }

        if (!EnsureSpecificDataSink(node.DataSink().Ref(), KikimrProviderName, ctx)) {
            return TStatus::Error;
        }

        auto table = SessionCtx->Tables().EnsureTableExists(TString(node.DataSink().Cluster()),
            TString(node.Table().Value()), node.Pos(), ctx);

        if (!table) {
            return TStatus::Error;
        }

        if (!CheckDocApiModifiation(*table->Metadata, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        auto pos = ctx.GetPosition(node.Pos());
        if (auto maybeTuple = node.Input().Maybe<TExprList>()) {
            auto tuple = maybeTuple.Cast();

            TVector<TExprBase> convertedValues;
            for (const auto& value : tuple) {
                auto valueType = value.Ref().GetTypeAnn();
                if (valueType->GetKind() != ETypeAnnotationKind::Struct) {
                    ctx.AddError(TIssue(pos, TStringBuilder()
                        << "Expected structs as input, but got: " << *valueType));
                    return TStatus::Error;
                }

                auto expectedType = GetExpectedRowType(*table, *valueType->Cast<TStructExprType>(), pos, ctx);
                if (!expectedType) {
                    return TStatus::Error;
                }

                TExprNode::TPtr node = value.Ptr();
                if (TryConvertTo(node, *expectedType, ctx) == TStatus::Error) {
                    ctx.AddError(YqlIssue(ctx.GetPosition(node->Pos()), TIssuesIds::KIKIMR_BAD_COLUMN_TYPE, TStringBuilder()
                        << "Failed to convert input columns types to scheme types"));
                    return TStatus::Error;
                }

                convertedValues.push_back(TExprBase(node));
            }

            auto list = Build<TCoAsList>(ctx, node.Pos())
                .Add(convertedValues)
                .Done();

            node.Ptr()->ChildRef(TKiWriteTable::idx_Input) = list.Ptr();
            return TStatus::Repeat;
        }

        const TStructExprType* rowType = nullptr;

        auto inputType = node.Input().Ref().GetTypeAnn();
        if (inputType->GetKind() == ETypeAnnotationKind::List) {
            auto listType = inputType->Cast<TListExprType>();
            auto itemType = listType->GetItemType();
            if (itemType->GetKind() == ETypeAnnotationKind::Struct) {
                rowType = itemType->Cast<TStructExprType>();
            }
        } else if (inputType->GetKind() == ETypeAnnotationKind::Stream) {
            auto streamType = inputType->Cast<TStreamExprType>();
            auto itemType = streamType->GetItemType();
            if (itemType->GetKind() == ETypeAnnotationKind::Struct) {
                rowType = itemType->Cast<TStructExprType>();
            }
        }

        if (!rowType) {
            ctx.AddError(TIssue(pos, TStringBuilder()
                << "Expected list or stream of structs as input, but got: " << *inputType));
            return TStatus::Error;
        }

        for (auto& keyColumnName : table->Metadata->KeyColumnNames) {
            if (!rowType->FindItem(keyColumnName)) {
                ctx.AddError(YqlIssue(pos, TIssuesIds::KIKIMR_PRECONDITION_FAILED, TStringBuilder()
                    << "Missing key column in input: " << keyColumnName
                    << " for table: " << table->Metadata->Name));
                return TStatus::Error;
            }
        }

        auto op = GetTableOp(node);
        if (op == TYdbOperation::InsertAbort || op == TYdbOperation::InsertRevert ||
            op == TYdbOperation::Upsert || op == TYdbOperation::Replace) {
            for (const auto& [name, meta] : table->Metadata->Columns) {
                if (meta.NotNull && !rowType->FindItem(name)) {
                    ctx.AddError(YqlIssue(pos, TIssuesIds::KIKIMR_NO_COLUMN_DEFAULT_VALUE, TStringBuilder()
                        << "Missing not null column in input: " << name
                        << ". All not null columns should be initialized"));
                    return TStatus::Error;
                }

                if (meta.NotNull && rowType->FindItemType(name)->HasOptionalOrNull()) {
                    ctx.AddError(YqlIssue(pos, TIssuesIds::KIKIMR_BAD_COLUMN_TYPE, TStringBuilder()
                        << "Can't set NULL or optional value to not null column: " << name
                        << ". All not null columns should be initialized"));
                    return TStatus::Error;
                }
            }
        } else if (op == TYdbOperation::UpdateOn) {
            for (const auto& item : rowType->GetItems()) {
                auto column = table->Metadata->Columns.FindPtr(TString(item->GetName()));
                YQL_ENSURE(column);
                if (column->NotNull && item->HasOptionalOrNull()) {
                    ctx.AddError(YqlIssue(pos, TIssuesIds::KIKIMR_BAD_COLUMN_TYPE, TStringBuilder()
                        << "Can't set NULL or optional value to not null column: " << column->Name));
                    return TStatus::Error;
                }
            }
        }

        auto inputColumns = GetSetting(node.Settings().Ref(), "input_columns");
        if (!inputColumns) {
            TExprNode::TListType columns;
            for (auto& item : rowType->GetItems()) {
                columns.push_back(ctx.NewAtom(node.Pos(), item->GetName()));
            }

            node.Ptr()->ChildRef(TKiWriteTable::idx_Settings) = Build<TCoNameValueTupleList>(ctx, node.Pos())
                .Add(node.Settings())
                .Add()
                    .Name().Build("input_columns")
                    .Value<TCoAtomList>()
                        .Add(columns)
                        .Build()
                    .Build()
                .Done()
                .Ptr();

            return TStatus::Repeat;
        } else {
            for (const auto& atom : TCoNameValueTuple(inputColumns).Value().Cast<TCoAtomList>()) {
                YQL_ENSURE(rowType->FindItem(atom.Value()));
            }
        }

        auto status = ConvertTableRowType(node.Ptr()->ChildRef(TKiWriteTable::idx_Input), *table, ctx);
        if (status != IGraphTransformer::TStatus::Ok) {
            return status;
        }

        if (!EnsureModifyPermissions(table->Metadata->Cluster, table->Metadata->Name, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleUpdateTable(TKiUpdateTable node, TExprContext& ctx) override {
        auto table = SessionCtx->Tables().EnsureTableExists(TString(node.DataSink().Cluster()), TString(node.Table().Value()), node.Pos(), ctx);
        if (!table) {
            return TStatus::Error;
        }

        if (!CheckDocApiModifiation(*table->Metadata, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        auto rowType = table->SchemeNode;
        auto& filterLambda = node.Ptr()->ChildRef(TKiUpdateTable::idx_Filter);
        if (!UpdateLambdaAllArgumentsTypes(filterLambda, {rowType}, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        if (!filterLambda->GetTypeAnn()) {
            return IGraphTransformer::TStatus::Repeat;
        }

        if (!EnsureSpecificDataType(*filterLambda, EDataSlot::Bool, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        auto& updateLambda = node.Ptr()->ChildRef(TKiUpdateTable::idx_Update);
        if (!UpdateLambdaAllArgumentsTypes(updateLambda, {rowType}, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        if (!updateLambda->GetTypeAnn()) {
            return IGraphTransformer::TStatus::Repeat;
        }

        if (!EnsureStructType(*updateLambda, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        auto updateResultType = updateLambda->GetTypeAnn()->Cast<TStructExprType>();
        for (auto& item : updateResultType->GetItems()) {
            const auto& name = item->GetName();

            if (table->GetKeyColumnIndex(TString(name))) {
                ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), TStringBuilder()
                    << "Cannot update primary key column: " << name));
                return IGraphTransformer::TStatus::Error;
            }
        }

        for (const auto& item : updateResultType->GetItems()) {
            auto column = table->Metadata->Columns.FindPtr(TString(item->GetName()));
            if (!column) {
                ctx.AddError(YqlIssue(ctx.GetPosition(node.Pos()), TIssuesIds::KIKIMR_BAD_REQUEST, TStringBuilder()
                    << "Column '" << column->Name << "' does not exist in table '" << node.Table().Value() << "'."));
                return TStatus::Error;
            }
            if (column->NotNull && item->HasOptionalOrNull()) {
                ctx.AddError(YqlIssue(ctx.GetPosition(node.Pos()), TIssuesIds::KIKIMR_BAD_COLUMN_TYPE, TStringBuilder()
                    << "Can't set NULL or optional value to not null column: " << column->Name));
                return TStatus::Error;
            }
        }

        auto updateBody = node.Update().Body().Ptr();
        auto status = ConvertTableRowType(updateBody, *table, ctx);
        if (status != IGraphTransformer::TStatus::Ok) {
            if (status == IGraphTransformer::TStatus::Repeat) {
                updateLambda = Build<TCoLambda>(ctx, node.Update().Pos())
                    .Args(node.Update().Args())
                    .Body(updateBody)
                    .Done()
                    .Ptr();
            }

            return status;
        }

        if (!EnsureModifyPermissions(table->Metadata->Cluster, table->Metadata->Name, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleDeleteTable(TKiDeleteTable node, TExprContext& ctx) override {
        auto table = SessionCtx->Tables().EnsureTableExists(TString(node.DataSink().Cluster()), TString(node.Table().Value()), node.Pos(), ctx);
        if (!table) {
            return TStatus::Error;
        }

        if (!CheckDocApiModifiation(*table->Metadata, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        auto rowType = table->SchemeNode;
        auto& filterLambda = node.Ptr()->ChildRef(TKiUpdateTable::idx_Filter);
        if (!UpdateLambdaAllArgumentsTypes(filterLambda, {rowType}, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        if (!filterLambda->GetTypeAnn()) {
            return IGraphTransformer::TStatus::Repeat;
        }

        if (!EnsureSpecificDataType(*filterLambda, EDataSlot::Bool, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        if (!EnsureModifyPermissions(table->Metadata->Cluster, table->Metadata->Name, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleCreateTable(TKiCreateTable create, TExprContext& ctx) override {
        TString cluster = TString(create.DataSink().Cluster());
        TString table = TString(create.Table());

        auto columnTypeError = GetColumnTypeErrorFn(ctx);

        TKikimrTableMetadataPtr meta = new TKikimrTableMetadata(cluster, table);
        meta->DoesExist = true;
        meta->ColumnOrder.reserve(create.Columns().Size());

        for (auto atom : create.PrimaryKey()) {
            meta->KeyColumnNames.emplace_back(atom.Value());
        }

        for (const auto& column : create.PartitionBy()) {
            meta->TableSettings.PartitionBy.emplace_back(column.Value());
        }

        for (auto item : create.Columns()) {
            auto columnTuple = item.Cast<TExprList>();
            auto nameNode = columnTuple.Item(0).Cast<TCoAtom>();
            auto typeNode = columnTuple.Item(1);

            auto columnName = TString(nameNode.Value());
            auto columnType = typeNode.Ref().GetTypeAnn();
            YQL_ENSURE(columnType && columnType->GetKind() == ETypeAnnotationKind::Type);

            auto type = columnType->Cast<TTypeExprType>()->GetType();
            auto notNull = type->GetKind() != ETypeAnnotationKind::Optional;
            auto actualType = notNull ? type : type->Cast<TOptionalExprType>()->GetItemType();
            if (actualType->GetKind() != ETypeAnnotationKind::Data) {
                columnTypeError(typeNode.Pos(), columnName, "Only core YQL data types are currently supported");
                return TStatus::Error;
            }

            auto dataType = actualType->Cast<TDataExprType>();

            if (!ValidateColumnDataType(dataType, typeNode, columnName, ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            TKikimrColumnMetadata columnMeta;
            columnMeta.Name = columnName;
            columnMeta.Type = dataType->GetName();
            columnMeta.NotNull = notNull;

            if (columnTuple.Size() > 2) {
                auto families = columnTuple.Item(2).Cast<TCoAtomList>();
                for (auto family : families) {
                    columnMeta.Families.push_back(TString(family.Value()));
                }
            }

            meta->ColumnOrder.push_back(columnName);
            auto insertRes = meta->Columns.insert(std::make_pair(columnName, columnMeta));
            if (!insertRes.second) {
                ctx.AddError(TIssue(ctx.GetPosition(create.Pos()), TStringBuilder()
                    << "Duplicate column: " << columnName << "."));
                return TStatus::Error;
            }
        }

        for (const auto& index : create.Indexes()) {
            const auto type = index.Type().Value();
            TIndexDescription::EType indexType;

            if (type == "syncGlobal") {
                indexType = TIndexDescription::EType::GlobalSync;
            } else if (type == "asyncGlobal") {
                indexType = TIndexDescription::EType::GlobalAsync;
            } else {
                YQL_ENSURE(false, "Unknown index type: " << type);
            }

            TVector<TString> indexColums;
            TVector<TString> dataColums;

            for (const auto& indexCol : index.Columns()) {
                if (!meta->Columns.contains(TString(indexCol.Value()))) {
                    ctx.AddError(TIssue(ctx.GetPosition(indexCol.Pos()), TStringBuilder()
                        << "Index column: " << indexCol.Value() << " was not found in the index table"));
                    return IGraphTransformer::TStatus::Error;
                }
                indexColums.emplace_back(TString(indexCol.Value()));
            }

            for (const auto& dataCol : index.DataColumns()) {
                if (!meta->Columns.contains(TString(dataCol.Value()))) {
                    ctx.AddError(TIssue(ctx.GetPosition(dataCol.Pos()), TStringBuilder()
                        << "Data column: " << dataCol.Value() << " was not found in the index table"));
                    return IGraphTransformer::TStatus::Error;
                }
                dataColums.emplace_back(TString(dataCol.Value()));
            }

            // IndexState and version, pathId are ignored for create table with index request
            TIndexDescription indexDesc(
                TString(index.Name().Value()),
                indexColums,
                dataColums,
                indexType,
                TIndexDescription::EIndexState::Ready,
                0,
                0,
                0
            );

            meta->Indexes.push_back(indexDesc);
        }

        for (const auto& changefeed : create.Changefeeds()) {
            Y_UNUSED(changefeed);
            ctx.AddError(TIssue(ctx.GetPosition(changefeed.Pos()), TStringBuilder()
                << "Cannot create table with changefeed"));
            return TStatus::Error;
        }

        for (auto columnFamily : create.ColumnFamilies()) {
            if (auto maybeTupleList = columnFamily.Maybe<TCoNameValueTupleList>()) {
                TColumnFamily family;
                for (auto familySetting : maybeTupleList.Cast()) {
                    auto name = familySetting.Name().Value();
                    if (name == "name") {
                        family.Name = TString(familySetting.Value().Cast<TCoAtom>().Value());
                    } else if (name == "data") {
                        family.Data = TString(
                            familySetting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                        );
                    } else if (name == "compression") {
                        family.Compression = TString(
                            familySetting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                        );
                    } else {
                        ctx.AddError(TIssue(ctx.GetPosition(familySetting.Name().Pos()),
                            TStringBuilder() << "Unknown column family setting name: " << name));
                        return TStatus::Error;
                    }
                }
                meta->ColumnFamilies.push_back(family);
            }
        }

        for (const auto& setting : create.TableSettings()) {
            auto name = setting.Name().Value();
            if (name == "compactionPolicy") {
                meta->TableSettings.CompactionPolicy = TString(
                    setting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                );
            } else if (name == "autoPartitioningBySize") {
                meta->TableSettings.AutoPartitioningBySize = TString(setting.Value().Cast<TCoAtom>().Value());
            }  else if (name == "partitionSizeMb") {
                ui64 value = FromString<ui64>(
                    setting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                    );
                if (value) {
                    meta->TableSettings.PartitionSizeMb = value;
                } else {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                        "Can't set preferred partition size to 0. "
                        "To disable auto partitioning by size use 'SET AUTO_PARTITIONING_BY_SIZE DISABLED'"));
                    return TStatus::Error;
                }
            } else if (name == "autoPartitioningByLoad") {
                meta->TableSettings.AutoPartitioningByLoad = TString(setting.Value().Cast<TCoAtom>().Value());
            } else if (name == "minPartitions") {
                ui64 value = FromString<ui64>(
                    setting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                    );
                if (value) {
                    meta->TableSettings.MinPartitions = value;
                } else {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                        "Can't set min partition count to 0"));
                    return TStatus::Error;
                }
            } else if (name == "maxPartitions") {
                ui64 value = FromString<ui64>(
                    setting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                    );
                if (value) {
                    meta->TableSettings.MaxPartitions = value;
                } else {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                        "Can't set max partition count to 0"));
                    return TStatus::Error;
                }
            } else if (name == "uniformPartitions") {
                meta->TableSettings.UniformPartitions = FromString<ui64>(
                    setting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                );
            } else if (name == "partitionAtKeys") {
                TVector<const TDataExprType*> keyTypes;
                keyTypes.reserve(meta->KeyColumnNames.size() + 1);

                // Getting key column types
                for (const auto& key : meta->KeyColumnNames) {
                    for (auto item : create.Columns()) {
                        auto columnTuple = item.Cast<TExprList>();
                        auto nameNode = columnTuple.Item(0).Cast<TCoAtom>();
                        auto columnName = TString(nameNode.Value());
                        if (columnName == key) {
                            auto typeNode = columnTuple.Item(1);
                            auto keyType = typeNode.Ref().GetTypeAnn()->Cast<TTypeExprType>()->GetType();
                            if (keyType->HasOptional()) {
                                keyType = keyType->Cast<TOptionalExprType>()->GetItemType();
                            }
                            keyTypes.push_back(keyType->Cast<TDataExprType>());
                        }
                    }
                }
                if (keyTypes.size() != create.PrimaryKey().Size()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Pos()), "Can't get all key column types"));
                    return IGraphTransformer::TStatus::Error;
                }
                auto listNode = setting.Value().Cast<TExprList>();
                for (size_t i = 0; i < listNode.Size(); ++i) {
                    auto partitionNode = listNode.Item(i);
                    TVector<std::pair<EDataSlot, TString>> keys;
                    auto boundaries = partitionNode.Cast<TExprList>();
                    if (boundaries.Size() > keyTypes.size()) {
                        ctx.AddError(TIssue(ctx.GetPosition(partitionNode.Pos()), TStringBuilder()
                            << "Partition at keys has " << boundaries.Size() << " key values while there are only "
                            << keyTypes.size() << " key columns"));
                        return IGraphTransformer::TStatus::Error;
                    }
                    for (size_t j = 0; j < boundaries.Size(); ++j) {
                        TExprNode::TPtr keyNode = boundaries.Item(j).Ptr();
                        TString content(keyNode->Child(0)->Content());
                        if (keyNode->GetTypeAnn()->Cast<TDataExprType>()->GetSlot() != keyTypes[j]->GetSlot()) {
                            if (TryConvertTo(keyNode, *keyTypes[j], ctx) == TStatus::Error) {
                                ctx.AddError(TIssue(ctx.GetPosition(keyNode->Pos()), TStringBuilder()
                                    << "Failed to convert value \"" << content
                                    << "\" to a corresponding key column type"));
                                return TStatus::Error;
                            }
                            auto newTypeAnn = ctx.MakeType<TDataExprType>(keyTypes[j]->GetSlot());
                            keyNode->SetTypeAnn(newTypeAnn);
                        }

                        keys.emplace_back(keyTypes[j]->GetSlot(), content);
                    }

                    meta->TableSettings.PartitionAtKeys.push_back(keys);
                }
            } else if (name == "keyBloomFilter") {
                meta->TableSettings.KeyBloomFilter = TString(setting.Value().Cast<TCoAtom>().Value());
            } else if (name == "readReplicasSettings") {
                meta->TableSettings.ReadReplicasSettings = TString(
                    setting.Value().Cast<TCoDataCtor>().Literal().Cast<TCoAtom>().Value()
                );
            } else if (name == "setTtlSettings") {
                TTtlSettings ttlSettings;
                TString error;

                YQL_ENSURE(setting.Value().Maybe<TCoNameValueTupleList>());
                if (!TTtlSettings::TryParse(setting.Value().Cast<TCoNameValueTupleList>(), ttlSettings, error)) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                        TStringBuilder() << "Invalid TTL settings: " << error));
                    return TStatus::Error;
                }

                meta->TableSettings.TtlSettings.Set(ttlSettings);
            } else if (name == "resetTtlSettings") {
                ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                    "Can't reset TTL settings"));
                return TStatus::Error;
            } else {
                ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                    TStringBuilder() << "Unknown table profile setting: " << name));
                return TStatus::Error;
            }
        }

        if (!EnsureModifyPermissions(cluster, table, create.Pos(), ctx)) {
            return TStatus::Error;
        }

        auto& tableDesc = SessionCtx->Tables().GetTable(cluster, table);
        if (tableDesc.DoesExist() && !tableDesc.Metadata->IsSameTable(*meta)) {
            ctx.AddError(TIssue(ctx.GetPosition(create.Pos()), TStringBuilder()
                << "Table name conflict: " << NCommon::FullTableName(cluster, table)
                << " is used to reference multiple tables."));
            return TStatus::Error;
        }

        tableDesc.Metadata = meta;
        bool sysColumnsEnabled = SessionCtx->Config().SystemColumnsEnabled();
        YQL_ENSURE(tableDesc.Load(ctx, sysColumnsEnabled));

        create.Ptr()->SetTypeAnn(create.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleDropTable(TKiDropTable node, TExprContext& ctx) override {
        auto table = SessionCtx->Tables().EnsureTableExists(TString(node.DataSink().Cluster()), TString(node.Table().Value()), node.Pos(), ctx);
        if (!table) {
            return TStatus::Error;
        }

        if (!EnsureModifyPermissions(table->Metadata->Cluster, table->Metadata->Name, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        if (!CheckDocApiModifiation(*table->Metadata, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleAlterTable(TKiAlterTable node, TExprContext& ctx) override {
        auto table = SessionCtx->Tables().EnsureTableExists(TString(node.DataSink().Cluster()), TString(node.Table().Value()), node.Pos(), ctx);
        if (!table) {
            return TStatus::Error;
        }

        if (!table->Metadata) {
            return TStatus::Error;
        }

        if (!EnsureModifyPermissions(table->Metadata->Cluster, table->Metadata->Name, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        if (!CheckDocApiModifiation(*table->Metadata, node.Pos(), ctx)) {
            return TStatus::Error;
        }

        YQL_ENSURE(!node.Actions().Empty());

        for (const auto& action : node.Actions()) {
            auto name = action.Name().Value();
            if (name == "renameTo") {
                YQL_ENSURE(action.Value().Cast<TCoAtom>().Value());
            } else if (name == "addColumns") {
                auto listNode = action.Value().Cast<TExprList>();
                for (size_t i = 0; i < listNode.Size(); ++i) {
                    auto item = listNode.Item(i);
                    auto columnTuple = item.Cast<TExprList>();
                    auto nameNode = columnTuple.Item(0).Cast<TCoAtom>();
                    auto name = TString(nameNode.Value());
                    if (table->Metadata->Columns.FindPtr(name)) {
                        ctx.AddError(TIssue(ctx.GetPosition(nameNode.Pos()), TStringBuilder()
                            << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                            << " Column: \"" << name << "\" already exists"));
                        return TStatus::Error;
                    }
                }
                auto columnTypeError = GetColumnTypeErrorFn(ctx);
                for (size_t i = 0; i < listNode.Size(); ++i) {
                    auto item = listNode.Item(i);
                    auto columnTuple = item.Cast<TExprList>();
                    auto nameNode = columnTuple.Item(0).Cast<TCoAtom>();
                    auto name = TString(nameNode.Value());
                    columnTuple.Item(0).Cast<TCoAtom>();
                    auto typeNode = columnTuple.Item(1);
                    auto columnType = typeNode.Ref().GetTypeAnn();
                    YQL_ENSURE(columnType && columnType->GetKind() == ETypeAnnotationKind::Type);
                    auto type = columnType->Cast<TTypeExprType>()->GetType();
                    auto actualType = (type->GetKind() == ETypeAnnotationKind::Optional) ?
                        type->Cast<TOptionalExprType>()->GetItemType() : type;

                    if (actualType->GetKind() != ETypeAnnotationKind::Data) {
                        columnTypeError(typeNode.Pos(), name, "Only core YQL data types are currently supported");
                        return TStatus::Error;
                    }

                    auto dataType = actualType->Cast<TDataExprType>();

                    if (!ValidateColumnDataType(dataType, typeNode, name, ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }

                    if (columnTuple.Size() > 2) {
                        auto families = columnTuple.Item(2);
                        if (families.Cast<TCoAtomList>().Size() > 1) {
                            ctx.AddError(TIssue(ctx.GetPosition(nameNode.Pos()), TStringBuilder()
                                << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                                << " Column: \"" << name
                                << "\". Several column families for a single column are not yet supported"));
                            return TStatus::Error;
                        }
                    }
                }
            } else if (name == "dropColumns") {
                auto listNode = action.Value().Cast<TCoAtomList>();
                THashSet<TString> keyColumns;
                for (const auto& keyColumnName : table->Metadata->KeyColumnNames) {
                    keyColumns.insert(keyColumnName);
                }
                for (auto dropColumn : listNode) {
                    TString name(dropColumn.Value());

                    if (!table->Metadata->Columns.FindPtr(name)) {
                        ctx.AddError(TIssue(ctx.GetPosition(dropColumn.Pos()), TStringBuilder()
                            << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                            << " Column \"" << name << "\" does not exist"));
                        return TStatus::Error;
                    }

                    if (keyColumns.find(name) != keyColumns.end()) {
                        ctx.AddError(TIssue(ctx.GetPosition(dropColumn.Pos()), TStringBuilder()
                            << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                            << " Column: \"" << name << "\" is a key column. Key column drop is not supported"));
                        return TStatus::Error;
                    }
                }
            } else if (name == "alterColumns") {
                auto listNode = action.Value().Cast<TExprList>();
                for (size_t i = 0; i < listNode.Size(); ++i) {
                    auto item = listNode.Item(i);
                    auto columnTuple = item.Cast<TExprList>();
                    auto nameNode = columnTuple.Item(0).Cast<TCoAtom>();;
                    auto name = TString(nameNode.Value());
                    if (!table->Metadata->Columns.FindPtr(name)) {
                        ctx.AddError(TIssue(ctx.GetPosition(nameNode.Pos()), TStringBuilder()
                            << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                            << " Column: \"" << name << "\" does not exist"));
                        return TStatus::Error;
                    }
                    auto families = columnTuple.Item(1);
                    if (families.Cast<TCoAtomList>().Size() > 1) {
                        ctx.AddError(TIssue(ctx.GetPosition(nameNode.Pos()), TStringBuilder()
                            << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                            << " Column: \"" << name
                            << "\". Several column families for a single column are not yet supported"));
                        return TStatus::Error;
                    }
                }
            } else if (name == "addIndex") {
                auto listNode = action.Value().Cast<TExprList>();
                for (size_t i = 0; i < listNode.Size(); ++i) {
                    auto item = listNode.Item(i);
                    auto columnTuple = item.Cast<TExprList>();
                    auto nameNode = columnTuple.Item(0).Cast<TCoAtom>();
                    auto name = TString(nameNode.Value());
                    if (name == "indexColumns" || name == "dataColumns") {
                        auto columnList = columnTuple.Item(1).Cast<TCoAtomList>();
                        for (auto column : columnList) {
                            TString columnName(column.Value());
                            if (!table->Metadata->Columns.FindPtr(columnName)) {
                                ctx.AddError(TIssue(ctx.GetPosition(column.Pos()), TStringBuilder()
                                    << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                                    << " Column: \"" << columnName << "\" does not exist"));
                                return TStatus::Error;
                            }
                        }
                    }
                }
            } else if (name == "dropIndex") {
                auto nameNode = action.Value().Cast<TCoAtom>();
                auto name = TString(nameNode.Value());

                const auto& indexes = table->Metadata->Indexes;

                auto cmp = [name](const TIndexDescription& desc) {
                    return name == desc.Name;
                };

                if (std::find_if(indexes.begin(), indexes.end(), cmp) == indexes.end()) {
                    ctx.AddError(TIssue(ctx.GetPosition(nameNode.Pos()), TStringBuilder()
                        << "AlterTable : " << NCommon::FullTableName(table->Metadata->Cluster, table->Metadata->Name)
                        << " Index: \"" << name << "\" does not exist"));
                    return TStatus::Error;
                }
            } else if (name != "addColumnFamilies"
                    && name != "alterColumnFamilies"
                    && name != "setTableSettings"
                    && name != "addChangefeed"
                    && name != "dropChangefeed"
                    && name != "renameIndexTo")
            {
                ctx.AddError(TIssue(ctx.GetPosition(action.Name().Pos()),
                    TStringBuilder() << "Unknown alter table action: " << name));
                return TStatus::Error;
            }
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleCreateUser(TKiCreateUser node, TExprContext& ctx) override {
        for (const auto& setting : node.Settings()) {
            auto name = setting.Name().Value();
            if (name == "password") {
                if (!EnsureAtom(setting.Value().Ref(), ctx)) {
                    return TStatus::Error;
                }
            } else if (name == "passwordEncrypted") {
                if (setting.Value()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Value().Ref().Pos()),
                        TStringBuilder() << "passwordEncrypted node shouldn't have value" << name));
                }
            } else if (name == "nullPassword") {
                if (setting.Value()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Value().Ref().Pos()),
                        TStringBuilder() << "nullPassword node shouldn't have value" << name));
                }
            } else {
                ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                    TStringBuilder() << "Unknown create user setting: " << name));
                return TStatus::Error;
            }
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleAlterUser(TKiAlterUser node, TExprContext& ctx) override {
        for (const auto& setting : node.Settings()) {
            auto name = setting.Name().Value();
            if (name == "password") {
                if (!EnsureAtom(setting.Value().Ref(), ctx)) {
                    return TStatus::Error;
                }
            } else if (name == "passwordEncrypted") {
                if (setting.Value()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Value().Ref().Pos()),
                        TStringBuilder() << "passwordEncrypted node shouldn't have value" << name));
                }
            } else if (name == "nullPassword") {
                if (setting.Value()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Value().Ref().Pos()),
                        TStringBuilder() << "nullPassword node shouldn't have value" << name));
                }
            } else {
                ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                    TStringBuilder() << "Unknown alter user setting: " << name));
                return TStatus::Error;
            }
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleDropUser(TKiDropUser node, TExprContext& ctx) override {
        for (const auto& setting : node.Settings()) {
            auto name = setting.Name().Value();
            if (name == "force") {
                if (setting.Value()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Value().Ref().Pos()),
                        TStringBuilder() << "force node shouldn't have value" << name));
                }
            } else {
                ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                    TStringBuilder() << "Unknown drop user setting: " << name));
                return TStatus::Error;
            }
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleCreateGroup(TKiCreateGroup node, TExprContext& ctx) override {
        Y_UNUSED(ctx);
        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleAlterGroup(TKiAlterGroup node, TExprContext& ctx) override {
        Y_UNUSED(ctx);
        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleDropGroup(TKiDropGroup node, TExprContext& ctx) override {
        for (const auto& setting : node.Settings()) {
            auto name = setting.Name().Value();
            if (name == "force") {
                if (setting.Value()) {
                    ctx.AddError(TIssue(ctx.GetPosition(setting.Value().Ref().Pos()),
                        TStringBuilder() << "force node shouldn't have value" << name));
                }
            } else {
                ctx.AddError(TIssue(ctx.GetPosition(setting.Name().Pos()),
                    TStringBuilder() << "Unknown drop group setting: " << name));
                return TStatus::Error;
            }
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleWrite(TExprBase node, TExprContext& ctx) override {
        ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), "Failed to annotate Write!, IO rewrite should handle this"));
        return TStatus::Error;
    }

    virtual TStatus HandleCommit(NNodes::TCoCommit node, TExprContext& ctx) override {
        auto settings = NCommon::ParseCommitSettings(node, ctx);

        bool isFlushCommit = false;
        if (settings.Mode) {
            auto mode = settings.Mode.Cast().Value();

            if (!KikimrCommitModes().contains(mode)) {
                ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), TStringBuilder()
                    << "Unsupported Kikimr commit mode: " << mode));
                return TStatus::Error;
            }

            isFlushCommit = (mode == KikimrCommitModeFlush());
        }

        if (!settings.EnsureEpochEmpty(ctx)) {
            return IGraphTransformer::TStatus::Error;
        }
        if (!settings.EnsureOtherEmpty(ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        switch (SessionCtx->Query().Type) {
            case EKikimrQueryType::YqlScript:
            case EKikimrQueryType::YqlScriptStreaming:
            case EKikimrQueryType::YqlInternal:
                break;

            default:
                if (!isFlushCommit) {
                    ctx.AddError(YqlIssue(ctx.GetPosition(node.Pos()), TIssuesIds::KIKIMR_BAD_OPERATION, TStringBuilder()
                        << "COMMIT not supported inside Kikimr query"));

                    return TStatus::Error;
                }
                break;
        }

        node.Ptr()->SetTypeAnn(node.World().Ref().GetTypeAnn());
        return TStatus::Ok;
    }

    virtual TStatus HandleEffects(NNodes::TKiEffects node, TExprContext& ctx) override {
        for (const auto& effect : node) {
            if (!EnsureWorldType(effect.Ref(), ctx)) {
                return TStatus::Error;
            }

            if (!KikimrSupportedEffects().contains(effect.CallableName())) {
                ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), TStringBuilder()
                    << "Unsupported Kikimr data query effect: " << effect.CallableName()));
                return TStatus::Error;
            }
        }

        node.Ptr()->SetTypeAnn(ctx.MakeType<TWorldExprType>());
        return TStatus::Ok;
    }

    virtual TStatus HandleDataQuery(NNodes::TKiDataQuery node, TExprContext& ctx) override {
        if (!EnsureWorldType(node.Effects().Ref(), ctx)) {
            return TStatus::Error;
        }

        TTypeAnnotationNode::TListType resultTypes;
        for (const auto& result : node.Results()) {
            auto resultType = result.Value().Ref().GetTypeAnn();
            if (!EnsureListType(node.Pos(), *resultType, ctx)) {
                return TStatus::Error;
            }
            auto itemType = resultType->Cast<TListExprType>()->GetItemType();
            if (!EnsureStructType(node.Pos(), *itemType, ctx)) {
                return TStatus::Error;
            }
            auto structType = itemType->Cast<TStructExprType>();

            for (const auto& column : result.Columns()) {
                if (!structType->FindItem(column)) {
                    ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), TStringBuilder()
                        << "Invalid column in result: " << column.Value()));
                    return TStatus::Error;
                }
            }

            resultTypes.push_back(resultType);
        }

        node.Ptr()->SetTypeAnn(ctx.MakeType<TTupleExprType>(resultTypes));
        return TStatus::Ok;
    }

    virtual TStatus HandleExecDataQuery(NNodes::TKiExecDataQuery node, TExprContext& ctx) override {
        if (!EnsureWorldType(node.World().Ref(), ctx)) {
            return TStatus::Error;
        }

        if (!EnsureDataSink(node.DataSink().Ref(), ctx)) {
            return TStatus::Error;
        }

        TTypeAnnotationNode::TListType children;
        children.push_back(node.World().Ref().GetTypeAnn());
        children.push_back(node.Query().Ref().GetTypeAnn());
        auto tupleAnn = ctx.MakeType<TTupleExprType>(children);
        node.Ptr()->SetTypeAnn(tupleAnn);

        return TStatus::Ok;
    }

    virtual TStatus HandleKql(TCallable node, TExprContext& ctx) override {
        bool sysColumnsEnabled = SessionCtx->Config().SystemColumnsEnabled();
        if (auto call = node.Maybe<TKiSelectRow>()) {
            auto selectRow = call.Cast();

            auto selectType = GetReadTableRowType(ctx, SessionCtx->Tables(), TString(selectRow.Cluster()),
                TString(selectRow.Table().Path()), selectRow.Select(), sysColumnsEnabled);
            if (!selectType) {
                return TStatus::Error;
            }

            auto optSelectType = ctx.MakeType<TOptionalExprType>(selectType);

            node.Ptr()->SetTypeAnn(optSelectType);

            return TStatus::Ok;
        }

        if (auto call = node.Maybe<TKiSelectRangeBase>()) {
            auto selectRange = call.Cast();

            auto selectType = GetReadTableRowType(ctx, SessionCtx->Tables(), TString(selectRange.Cluster()),
                TString(selectRange.Table().Path()), selectRange.Select(), sysColumnsEnabled);
            if (!selectType) {
                return TStatus::Error;
            }

            auto listSelectType = ctx.MakeType<TListExprType>(selectType);

            node.Ptr()->SetTypeAnn(listSelectType);

            return TStatus::Ok;
        }

        if (node.Maybe<TKiUpdateRow>()) {
            node.Ptr()->SetTypeAnn(ctx.MakeType<TVoidExprType>());

            return TStatus::Ok;
        }

        if (node.Maybe<TKiEraseRow>()) {
            node.Ptr()->SetTypeAnn(ctx.MakeType<TVoidExprType>());

            return TStatus::Ok;
        }

        if (node.Maybe<TKiSetResult>()) {
            node.Ptr()->SetTypeAnn(ctx.MakeType<TVoidExprType>());

            return TStatus::Ok;
        }

        if (auto maybeMap = node.Maybe<TKiMapParameter>()) {
            auto map = maybeMap.Cast();

            if (!EnsureArgsCount(map.Ref(), 2, ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            if (!EnsureListType(map.Input().Ref(), ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            auto& lambda = map.Ptr()->ChildRef(TKiMapParameter::idx_Lambda);
            auto itemType = map.Input().Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType();
            if (!UpdateLambdaAllArgumentsTypes(lambda, {itemType}, ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            if (!lambda->GetTypeAnn()) {
                return IGraphTransformer::TStatus::Repeat;
            }

            map.Ptr()->SetTypeAnn(ctx.MakeType<TListExprType>(lambda->GetTypeAnn()));

            return TStatus::Ok;
        }

        if (auto maybeMap = node.Maybe<TKiFlatMapParameter>()) {
            auto map = maybeMap.Cast();

            if (!EnsureArgsCount(map.Ref(), 2, ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            if (!EnsureListType(map.Input().Ref(), ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            auto& lambda = map.Ptr()->ChildRef(TKiFlatMapParameter::idx_Lambda);
            auto itemType = map.Input().Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType();
            if (!UpdateLambdaAllArgumentsTypes(lambda, {itemType}, ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            if (!lambda->GetTypeAnn()) {
                return IGraphTransformer::TStatus::Repeat;
            }

            auto retKind = lambda->GetTypeAnn()->GetKind();
            if (retKind != ETypeAnnotationKind::List) {
                ctx.AddError(TIssue(ctx.GetPosition(lambda->Pos()), TStringBuilder() << "Expected list as labmda return type, but got: " << *lambda->GetTypeAnn()));
                return IGraphTransformer::TStatus::Error;
            }

            map.Ptr()->SetTypeAnn(lambda->GetTypeAnn());

            return TStatus::Ok;
        }

        if (node.Maybe<TKiPartialSort>()) {
            NTypeAnnImpl::TContext typeAnnCtx(ctx);
            TExprNode::TPtr output;
            return NTypeAnnImpl::SortWrapper(node.Ptr(), output, typeAnnCtx);
        }

        if (node.Maybe<TKiPartialTake>()) {
            NTypeAnnImpl::TContext typeAnnCtx(ctx);
            TExprNode::TPtr output;
            return NTypeAnnImpl::TakeWrapper(node.Ptr(), output, typeAnnCtx);
        }

        if (auto maybeCondEffect = node.Maybe<TKiConditionalEffect>()) {
            auto condEffect = maybeCondEffect.Cast();

            if (!EnsureDataType(condEffect.Predicate().Ref(), ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            auto predicateType = condEffect.Predicate().Ref().GetTypeAnn()->Cast<TDataExprType>();
            YQL_ENSURE(predicateType);

            if (predicateType->GetSlot() != EDataSlot::Bool) {
                ctx.AddError(TIssue(ctx.GetPosition(condEffect.Pos()), "Expected bool as predicate type"));
                return IGraphTransformer::TStatus::Error;
            }

            if (!EnsureListOfVoidType(condEffect.Effect().Ref(), ctx)) {
                return IGraphTransformer::TStatus::Error;
            }

            condEffect.Ptr()->SetTypeAnn(condEffect.Effect().Ref().GetTypeAnn());

            return TStatus::Ok;
        }

        ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), TStringBuilder()
            << "Unknown Kql callable in type annotation: " << node.CallableName()));

        return TStatus::Error;
    }

    bool EnsureModifyPermissions(const TString& cluster, const TString& table, TPositionHandle pos, TExprContext& ctx) {
        bool restrictPermissions = SessionCtx->Config()._RestrictModifyPermissions.Get(cluster).GetRef();
        if (!restrictPermissions) {
            return true;
        }

        TString tmpDir = "/Root/Tmp/";
        TString homeDir = "/Root/Home/" + SessionCtx->GetUserName() + "/";

        auto tablePath = Gateway->CanonizePath(table);
        if (!tablePath.StartsWith(tmpDir) && !tablePath.StartsWith(homeDir)) {
            ctx.AddError(TIssue(ctx.GetPosition(pos), TStringBuilder()
                << "User " << SessionCtx->GetUserName() << " doesn't have permissions to modify table: " << table));
            return false;
        }

        return true;
    }

    bool CheckDocApiModifiation(const TKikimrTableMetadata& meta, TPositionHandle pos, TExprContext& ctx) {
        if (!SessionCtx->Query().DocumentApiRestricted) {
            return true;
        }

        if (!meta.Attributes.FindPtr(DocApiTableVersionAttribute)) {
            return true;
        }

        ctx.AddError(YqlIssue(ctx.GetPosition(pos), TIssuesIds::KIKIMR_BAD_OPERATION, TStringBuilder()
            << "Document API table cannot be modified from YQL query: " << meta.Name));
        return false;
    }

private:
    TIntrusivePtr<IKikimrGateway> Gateway;
    TIntrusivePtr<TKikimrSessionContext> SessionCtx;
};

} // namespace

TAutoPtr<IGraphTransformer> CreateKiSourceTypeAnnotationTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx,
    TTypeAnnotationContext& types)
{
    return new TKiSourceTypeAnnotationTransformer(sessionCtx, types);
}

TAutoPtr<IGraphTransformer> CreateKiSinkTypeAnnotationTransformer(TIntrusivePtr<IKikimrGateway> gateway,
    TIntrusivePtr<TKikimrSessionContext> sessionCtx)
{
    return new TKiSinkTypeAnnotationTransformer(gateway, sessionCtx);
}

const TTypeAnnotationNode* GetReadTableRowType(TExprContext& ctx, const TKikimrTablesData& tablesData,
    const TString& cluster, const TString& table, TCoAtomList select, bool withSystemColumns)
{
    auto tableDesc = tablesData.EnsureTableExists(cluster, table, select.Pos(), ctx);
    if (!tableDesc) {
        return nullptr;
    }

    TVector<const TItemExprType*> resultItems;
    for (auto item : select) {
        auto column = tableDesc->Metadata->Columns.FindPtr(item.Value());
        TString columnName;
        if (column) {
            columnName = column->Name;
        } else {
            if (withSystemColumns && IsKikimrSystemColumn(item.Value())) {
                columnName = TString(item.Value());
            } else {
                ctx.AddError(TIssue(ctx.GetPosition(select.Pos()), TStringBuilder()
                    << "Column not found: " << item.Value()));
                return nullptr;
            }
        }

        auto type = tableDesc->GetColumnType(columnName);
        YQL_ENSURE(type, "No such column: " << columnName);

        auto itemType = ctx.MakeType<TItemExprType>(columnName, type);
        if (!itemType->Validate(select.Pos(), ctx)) {
            return nullptr;
        }
        resultItems.push_back(itemType);
    }

    auto resultType = ctx.MakeType<TStructExprType>(resultItems);
    if (!resultType->Validate(select.Pos(), ctx)) {
        return nullptr;
    }

    return resultType;
}

} // namespace NYql
