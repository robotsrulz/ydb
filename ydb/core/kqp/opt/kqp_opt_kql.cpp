#include "kqp_opt_impl.h"

#include <ydb/core/kqp/provider/yql_kikimr_provider_impl.h>
#include <ydb/core/kqp/provider/kqp_opt_helpers.h>

#include <ydb/library/yql/core/yql_opt_utils.h>

namespace NKikimr::NKqp::NOpt {

using namespace NYql;
using namespace NYql::NNodes;

namespace {

bool UseReadTableRanges(const TKikimrTableDescription& tableData, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx) {
    /*
     * OLAP tables can not work with ordinary ReadTable in case there is no support in physical
     * optimizers for them.
     */
    if (tableData.Metadata->Kind == EKikimrTableKind::Olap) {
        return true;
    }

    auto predicateExtractSetting = kqpCtx->Config->GetOptPredicateExtract();

    if (predicateExtractSetting != EOptionalFlag::Auto) {
        return predicateExtractSetting == EOptionalFlag::Enabled;
    }

    if (kqpCtx->IsScanQuery() && kqpCtx->Config->FeatureFlags.GetEnablePredicateExtractForScanQueries()) {
        return true;
    }

    if (kqpCtx->IsDataQuery() && kqpCtx->Config->FeatureFlags.GetEnablePredicateExtractForDataQueries()) {
        return true;
    }

    return false;
}

bool HasIndexesToWrite(const TKikimrTableDescription& tableData) {
    bool hasIndexesToWrite = false;
    YQL_ENSURE(tableData.Metadata->Indexes.size() == tableData.Metadata->SecondaryGlobalIndexMetadata.size());
    for (const auto& index : tableData.Metadata->Indexes) {
        if (index.ItUsedForWrite()) {
            hasIndexesToWrite = true;
            break;
        }
    }

    return hasIndexesToWrite;
}

TExprBase BuildReadTable(const TCoAtomList& columns, TPositionHandle pos, const TKikimrTableDescription& tableData,
    TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    TExprNode::TPtr readTable;
    const auto& tableMeta = BuildTableMeta(tableData, pos, ctx);

    if (UseReadTableRanges(tableData, kqpCtx)) {
        readTable = Build<TKqlReadTableRanges>(ctx, pos)
            .Table(tableMeta)
            .Ranges<TCoVoid>()
                .Build()
            .Columns(columns)
            .Settings()
                .Build()
            .ExplainPrompt()
                .Build()
            .Done().Ptr();
    } else {
        readTable = Build<TKqlReadTable>(ctx, pos)
            .Table(tableMeta)
            .Range()
                .From<TKqlKeyInc>()
                    .Build()
                .To<TKqlKeyInc>()
                    .Build()
                .Build()
            .Columns(columns)
            .Settings()
                .Build()
            .Done().Ptr();
    }

    return TExprBase(readTable);

}

TExprBase BuildReadTable(const TKiReadTable& read, const TKikimrTableDescription& tableData,
    bool withSystemColumns, TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    bool unwrapValues = HasSetting(read.Settings().Ref(), "unwrap_values");

    const auto& columns = read.GetSelectColumns(ctx, tableData, withSystemColumns);

    auto readNode = BuildReadTable(columns, read.Pos(), tableData, ctx, kqpCtx);

    return unwrapValues
        ? UnwrapKiReadTableValues(readNode, tableData, columns, ctx)
        : readNode;
}

TExprBase BuildReadTableIndex(const TKiReadTable& read, const TKikimrTableDescription& tableData,
    const TString& indexName, bool withSystemColumns, TExprContext& ctx)
{
    bool unwrapValues = HasSetting(read.Settings().Ref(), "unwrap_values");

    auto kqlReadTable = Build<TKqlReadTableIndex>(ctx, read.Pos())
        .Table(BuildTableMeta(tableData, read.Pos(), ctx))
        .Range()
            .From<TKqlKeyInc>()
                .Build()
            .To<TKqlKeyInc>()
                .Build()
            .Build()
        .Columns(read.GetSelectColumns(ctx, tableData, withSystemColumns))
        .Settings()
            .Build()
        .Index().Build(indexName)
        .Done();

    return unwrapValues
        ? UnwrapKiReadTableValues(kqlReadTable, tableData, kqlReadTable.Columns(), ctx)
        : kqlReadTable;
}

TExprBase BuildUpsertTable(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    auto effect = Build<TKqlUpsertRows>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(write.Input())
        .Columns(inputColumns)
        .Done();

    return effect;
}

TExprBase BuildUpsertTableWithIndex(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    auto effect = Build<TKqlUpsertRowsIndex>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(write.Input())
        .Columns(inputColumns)
        .Done();

    return effect;
}

TExprBase BuildReplaceTable(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    const auto [data, columns] = CreateRowsToReplace(write.Input(), inputColumns, tableData, write.Pos(), ctx);

    return Build<TKqlUpsertRows>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(data)
        .Columns(columns)
        .Done();
}

TExprBase BuildReplaceTableWithIndex(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    const auto [data, columns] = CreateRowsToReplace(write.Input(), inputColumns, tableData, write.Pos(), ctx);

    auto effect = Build<TKqlUpsertRowsIndex>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(data)
        .Columns(columns)
        .Done();

    return effect;
}

TExprBase BuildInsertTable(const TKiWriteTable& write, bool abort, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    auto effect = Build<TKqlInsertRows>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(write.Input())
        .Columns(inputColumns)
        .OnConflict()
            .Value(abort ? "abort"sv : "revert"sv)
            .Build()
        .Done();

    return effect;
}

TExprBase BuildInsertTableWithIndex(const TKiWriteTable& write, bool abort, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    auto effect = Build<TKqlInsertRowsIndex>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(write.Input())
        .Columns(inputColumns)
        .OnConflict()
            .Value(abort ? "abort"sv : "revert"sv)
            .Build()
        .Done();

    return effect;
}

TExprBase BuildUpdateOnTable(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    return Build<TKqlUpdateRows>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(write.Input())
        .Columns(inputColumns)
        .Done();
}

TExprBase BuildUpdateOnTableWithIndex(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    return Build<TKqlUpdateRowsIndex>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(write.Input())
        .Columns(inputColumns)
        .Done();
}

TExprBase BuildDeleteTable(const TKiWriteTable& write, const TKikimrTableDescription& tableData, TExprContext& ctx) {
    const auto keysToDelete = ProjectColumns(write.Input(), tableData.Metadata->KeyColumnNames, ctx);

    return Build<TKqlDeleteRows>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(keysToDelete)
        .Done();
}

TExprBase BuildDeleteTableWithIndex(const TKiWriteTable& write, const TKikimrTableDescription& tableData, TExprContext& ctx) {
    const auto keysToDelete = ProjectColumns(write.Input(), tableData.Metadata->KeyColumnNames, ctx);

    return Build<TKqlDeleteRowsIndex>(ctx, write.Pos())
        .Table(BuildTableMeta(tableData, write.Pos(), ctx))
        .Input(keysToDelete)
        .Done();
}

TExprBase BuildRowsToDelete(const TKikimrTableDescription& tableData, bool withSystemColumns, const TCoLambda& filter,
    const TPositionHandle pos, TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    const auto tableMeta = BuildTableMeta(tableData, pos, ctx);
    const auto tableColumns = BuildColumnsList(tableData, pos, ctx, withSystemColumns);

    const auto allRows = BuildReadTable(tableColumns, pos, tableData, ctx, kqpCtx);

    return Build<TCoFilter>(ctx, pos)
        .Input(allRows)
        .Lambda(filter)
        .Done();
}

TExprBase BuildDeleteTable(const TKiDeleteTable& del, const TKikimrTableDescription& tableData, bool withSystemColumns,
    TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    auto rowsToDelete = BuildRowsToDelete(tableData, withSystemColumns, del.Filter(), del.Pos(), ctx, kqpCtx);
    auto keysToDelete = ProjectColumns(rowsToDelete, tableData.Metadata->KeyColumnNames, ctx);

    return Build<TKqlDeleteRows>(ctx, del.Pos())
        .Table(BuildTableMeta(tableData, del.Pos(), ctx))
        .Input(keysToDelete)
        .Done();
}

TVector<TExprBase> BuildDeleteTableWithIndex(const TKiDeleteTable& del, const TKikimrTableDescription& tableData,
    bool withSystemColumns, TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    auto rowsToDelete = BuildRowsToDelete(tableData, withSystemColumns, del.Filter(), del.Pos(), ctx, kqpCtx);

    auto indexes = BuildSecondaryIndexVector(tableData, del.Pos(), ctx, nullptr,
        [] (const TKikimrTableMetadata& meta, TPositionHandle pos, TExprContext& ctx) -> TExprBase {
            return BuildTableMeta(meta, pos, ctx);
        });
    YQL_ENSURE(indexes);

    const auto& pk = tableData.Metadata->KeyColumnNames;

    auto tableDelete = Build<TKqlDeleteRows>(ctx, del.Pos())
        .Table(BuildTableMeta(tableData, del.Pos(), ctx))
        .Input(ProjectColumns(rowsToDelete, pk, ctx))
        .Done();

    TVector<TExprBase> effects;
    effects.push_back(tableDelete);

    for (const auto& [indexMeta, indexDesc] : indexes) {
        THashSet<TStringBuf> indexTableColumns;

        THashSet<TString> keyColumns;
        for (const auto& column : indexDesc->KeyColumns) {
            YQL_ENSURE(keyColumns.emplace(column).second);
            indexTableColumns.emplace(column);
        }

        for (const auto& column : pk) {
            if (keyColumns.insert(column).second) {
                indexTableColumns.emplace(column);
            }
        }

        auto indexDelete = Build<TKqlDeleteRows>(ctx, del.Pos())
            .Table(indexMeta)
            .Input(ProjectColumns(rowsToDelete, indexTableColumns, ctx))
            .Done();

        effects.push_back(indexDelete);
    }

    return effects;
}

TExprBase BuildRowsToUpdate(const TKikimrTableDescription& tableData, bool withSystemColumns, const TCoLambda& filter,
    const TPositionHandle pos, TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    auto kqlReadTable = BuildReadTable(BuildColumnsList(tableData, pos, ctx, withSystemColumns), pos, tableData, ctx, kqpCtx);

    return Build<TCoFilter>(ctx, pos)
        .Input(kqlReadTable)
        .Lambda(filter)
        .Done();
}

TExprBase BuildUpdatedRows(const TExprBase& rows, const TCoLambda& update, const THashSet<TStringBuf>& columns,
    const TPositionHandle pos, TExprContext& ctx) {
    auto rowArg = Build<TCoArgument>(ctx, pos)
        .Name("row")
        .Done();

    auto updateStruct = Build<TExprApplier>(ctx, pos)
        .Apply(update)
        .With(0, rowArg)
        .Done();

    const auto& updateStructType = update.Ref().GetTypeAnn()->Cast<TStructExprType>();
    TVector<TExprBase> updateTuples;
    for (const auto& column : columns) {
        TCoAtom columnAtom(ctx.NewAtom(pos, column));

        TExprBase valueSource = updateStructType->FindItem(column)
            ? updateStruct
            : TExprBase(rowArg);

        auto tuple = Build<TCoNameValueTuple>(ctx, pos)
            .Name(columnAtom)
            .Value<TCoMember>()
                .Struct(valueSource)
                .Name(columnAtom)
                .Build()
            .Done();

        updateTuples.push_back(tuple);
    }

    return Build<TCoMap>(ctx, pos)
        .Input(rows)
        .Lambda()
            .Args({rowArg})
            .Body<TCoAsStruct>()
                .Add(updateTuples)
                .Build()
            .Build()
        .Done();
}

THashSet<TStringBuf> GetUpdateColumns(const TKikimrTableDescription& tableData, const TCoLambda& update) {
    THashSet<TStringBuf> updateColumns;
    for (const auto& keyColumn : tableData.Metadata->KeyColumnNames) {
        updateColumns.emplace(keyColumn);
    }

    const auto& updateStructType = update.Ref().GetTypeAnn()->Cast<TStructExprType>();
    for (const auto& item : updateStructType->GetItems()) {
        updateColumns.emplace(item->GetName());
    }

    return updateColumns;
}

TExprBase BuildUpdateTable(const TKiUpdateTable& update, const TKikimrTableDescription& tableData,
    bool withSystemColumns, TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    auto rowsToUpdate = BuildRowsToUpdate(tableData, withSystemColumns, update.Filter(), update.Pos(), ctx, kqpCtx);

    auto updateColumns = GetUpdateColumns(tableData, update.Update());
    auto updatedRows = BuildUpdatedRows(rowsToUpdate, update.Update(), updateColumns, update.Pos(), ctx);

    TVector<TCoAtom> updateColumnsList;
    for (const auto& column : updateColumns) {
        updateColumnsList.push_back(TCoAtom(ctx.NewAtom(update.Pos(), column)));
    }

    return Build<TKqlUpsertRows>(ctx, update.Pos())
        .Table(BuildTableMeta(tableData, update.Pos(), ctx))
        .Input(updatedRows)
        .Columns()
            .Add(updateColumnsList)
            .Build()
        .Done();
}

TVector<TExprBase> BuildUpdateTableWithIndex(const TKiUpdateTable& update, const TKikimrTableDescription& tableData,
    bool withSystemColumns, TExprContext& ctx, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    auto rowsToUpdate = BuildRowsToUpdate(tableData, withSystemColumns, update.Filter(), update.Pos(), ctx, kqpCtx);

    auto indexes = BuildSecondaryIndexVector(tableData, update.Pos(), ctx, nullptr,
        [] (const TKikimrTableMetadata& meta, TPositionHandle pos, TExprContext& ctx) -> TExprBase {
            return BuildTableMeta(meta, pos, ctx);
        });
    YQL_ENSURE(indexes);

    const auto& pk = tableData.Metadata->KeyColumnNames;
    auto updateColumns = GetUpdateColumns(tableData, update.Update());

    auto updatedRows = BuildUpdatedRows(rowsToUpdate, update.Update(), updateColumns, update.Pos(), ctx);

    TVector<TCoAtom> updateColumnsList;
    for (const auto& column : updateColumns) {
        updateColumnsList.push_back(TCoAtom(ctx.NewAtom(update.Pos(), column)));
    }

    auto tableUpsert = Build<TKqlUpsertRows>(ctx, update.Pos())
        .Table(BuildTableMeta(tableData, update.Pos(), ctx))
        .Input(updatedRows)
        .Columns()
            .Add(updateColumnsList)
            .Build()
        .Done();

    TVector<TExprBase> effects;
    effects.push_back(tableUpsert);

    for (const auto& [indexMeta, indexDesc] : indexes) {
        THashSet<TStringBuf> indexTableColumns;

        bool indexKeyColumnsUpdated = false;
        THashSet<TString> keyColumns;
        for (const auto& column : indexDesc->KeyColumns) {
            YQL_ENSURE(keyColumns.emplace(column).second);

            // Table PK cannot be updated, so don't consider PK columns update as index update
            if (updateColumns.contains(column) && !tableData.GetKeyColumnIndex(column)) {
                indexKeyColumnsUpdated = true;
            }

            indexTableColumns.emplace(column);
        }

        for (const auto& column : pk) {
            if (keyColumns.insert(column).second) {
                indexTableColumns.emplace(column);
            }
        }

        if (indexKeyColumnsUpdated) {
            // Have to delete old index value from index table in case when index key columns were updated
            auto indexDelete = Build<TKqlDeleteRows>(ctx, update.Pos())
                .Table(indexMeta)
                .Input(ProjectColumns(rowsToUpdate, indexTableColumns, ctx))
                .Done();

            effects.push_back(indexDelete);
        }

        bool indexDataColumnsUpdated = false;
        for (const auto& column : indexDesc->DataColumns) {
            if (updateColumns.contains(column)) {
                indexDataColumnsUpdated = true;
            }

            indexTableColumns.emplace(column);
        }

        // Index table update required in case when index key or data columns were updated
        bool needIndexTableUpdate = indexKeyColumnsUpdated || indexDataColumnsUpdated;

        if (needIndexTableUpdate) {
            auto indexRows = BuildUpdatedRows(rowsToUpdate, update.Update(), indexTableColumns, update.Pos(), ctx);

            TVector<TCoAtom> indexColumnsList;
            for (const auto& column : indexTableColumns) {
                indexColumnsList.push_back(TCoAtom(ctx.NewAtom(update.Pos(), column)));
            }

            auto indexUpsert = Build<TKqlUpsertRows>(ctx, update.Pos())
                .Table(indexMeta)
                .Input(indexRows)
                .Columns()
                    .Add(indexColumnsList)
                    .Build()
                .Done();

            effects.push_back(indexUpsert);
        }
    }

    return effects;
}

TExprNode::TPtr HandleReadTable(const TKiReadTable& read, TExprContext& ctx, const TKikimrTablesData& tablesData,
    bool withSystemColumns, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    TKikimrKey key(ctx);
    YQL_ENSURE(key.Extract(read.TableKey().Ref()));
    YQL_ENSURE(key.GetKeyType() == TKikimrKey::Type::Table);
    auto& tableData = GetTableData(tablesData, read.DataSource().Cluster(), key.GetTablePath());

    if (key.GetView()) {
        const auto& indexName = key.GetView().GetRef();
        if (!ValidateTableHasIndex(tableData.Metadata, ctx, read.Pos())) {
            return nullptr;
        }

        if (kqpCtx->IsScanQuery() && !kqpCtx->Config->FeatureFlags.GetEnableKqpScanQueryStreamLookup()) {
            const TString err = "Secondary index is not supported for ScanQuery";
            ctx.AddError(YqlIssue(ctx.GetPosition(read.Pos()), TIssuesIds::KIKIMR_BAD_REQUEST, err));
            return nullptr;
        }

        auto [metadata, state] = tableData.Metadata->GetIndexMetadata(indexName);
        YQL_ENSURE(metadata, "unable to find metadata for index: " << indexName);
        YQL_ENSURE(state == TIndexDescription::EIndexState::Ready
            || state == TIndexDescription::EIndexState::WriteOnly);

        if (state != TIndexDescription::EIndexState::Ready) {
            auto err = TStringBuilder()
                << "Requested index: " << indexName
                << " is not ready to use";
            ctx.AddError(YqlIssue(ctx.GetPosition(read.Pos()), TIssuesIds::KIKIMR_INDEX_IS_NOT_READY, err));
            return nullptr;
        }

        return BuildReadTableIndex(read, tableData, indexName, withSystemColumns, ctx).Ptr();
    }

    return BuildReadTable(read, tableData, withSystemColumns, ctx, kqpCtx).Ptr();
}

TExprBase WriteTableSimple(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    auto op = GetTableOp(write);
    switch (op) {
        case TYdbOperation::Upsert:
            return BuildUpsertTable(write, inputColumns, tableData, ctx);
        case TYdbOperation::Replace:
            return BuildReplaceTable(write, inputColumns, tableData, ctx);
        case TYdbOperation::InsertAbort:
        case TYdbOperation::InsertRevert:
            return BuildInsertTable(write, op == TYdbOperation::InsertAbort, inputColumns, tableData, ctx);
        case TYdbOperation::UpdateOn:
            return BuildUpdateOnTable(write, inputColumns, tableData, ctx);
        case TYdbOperation::Delete:
            return BuildDeleteTable(write, tableData, ctx);
        case TYdbOperation::DeleteOn:
            return BuildDeleteTable(write, tableData, ctx);
        default:
            YQL_ENSURE(false, "Unsupported table operation: " << op << ", table: " << tableData.Metadata->Name);
    }
}

TExprBase WriteTableWithIndexUpdate(const TKiWriteTable& write, const TCoAtomList& inputColumns,
    const TKikimrTableDescription& tableData, TExprContext& ctx)
{
    auto op = GetTableOp(write);
    switch (op) {
        case TYdbOperation::Upsert:
            return BuildUpsertTableWithIndex(write, inputColumns, tableData, ctx);
        case TYdbOperation::Replace:
            return BuildReplaceTableWithIndex(write, inputColumns, tableData, ctx);
        case TYdbOperation::InsertAbort:
        case TYdbOperation::InsertRevert:
            return BuildInsertTableWithIndex(write, op == TYdbOperation::InsertAbort, inputColumns, tableData, ctx);
        case TYdbOperation::UpdateOn:
            return BuildUpdateOnTableWithIndex(write, inputColumns, tableData, ctx);
        case TYdbOperation::DeleteOn:
            return BuildDeleteTableWithIndex(write, tableData, ctx);
        default:
            YQL_ENSURE(false, "Unsupported table operation: " << (ui32)op << ", table: " << tableData.Metadata->Name);
    }

    Y_UNREACHABLE();
}

TExprBase HandleWriteTable(const TKiWriteTable& write, TExprContext& ctx, const TKikimrTablesData& tablesData) {
    auto& tableData = GetTableData(tablesData, write.DataSink().Cluster(), write.Table().Value());

    auto inputColumnsSetting = GetSetting(write.Settings().Ref(), "input_columns");
    YQL_ENSURE(inputColumnsSetting);
    auto inputColumns = TCoNameValueTuple(inputColumnsSetting).Value().Cast<TCoAtomList>();

    if (HasIndexesToWrite(tableData)) {
        return WriteTableWithIndexUpdate(write, inputColumns, tableData, ctx);
    } else {
        return WriteTableSimple(write, inputColumns, tableData, ctx);
    }
}

TVector<TExprBase> HandleUpdateTable(const TKiUpdateTable& update, TExprContext& ctx,
    const TKikimrTablesData& tablesData, bool withSystemColumns, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    const auto& tableData = GetTableData(tablesData, update.DataSink().Cluster(), update.Table().Value());

    if (HasIndexesToWrite(tableData)) {
        return BuildUpdateTableWithIndex(update, tableData, withSystemColumns, ctx, kqpCtx);
    } else {
        return { BuildUpdateTable(update, tableData, withSystemColumns, ctx, kqpCtx) };
    }
}

TVector<TExprBase> HandleDeleteTable(const TKiDeleteTable& del, TExprContext& ctx, const TKikimrTablesData& tablesData,
    bool withSystemColumns, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    auto& tableData = GetTableData(tablesData, del.DataSink().Cluster(), del.Table().Value());
    if (HasIndexesToWrite(tableData)) {
        return BuildDeleteTableWithIndex(del, tableData, withSystemColumns, ctx, kqpCtx);
    } else {
        return { BuildDeleteTable(del, tableData, withSystemColumns, ctx, kqpCtx) };
    }
}

} // namespace

const TKikimrTableDescription& GetTableData(const TKikimrTablesData& tablesData,
    TStringBuf cluster, TStringBuf table)
{
    const auto& tableData = tablesData.ExistingTable(cluster, table);
    YQL_ENSURE(tableData.Metadata);

    return tableData;
}

TIntrusivePtr<TKikimrTableMetadata> GetIndexMetadata(const TKqlReadTableIndex& read,
    const TKikimrTablesData& tables, TStringBuf cluster)
{
    const auto& tableDesc = GetTableData(tables, cluster, read.Table().Path());
    const auto& [indexMeta, _ ] = tableDesc.Metadata->GetIndexMetadata(read.Index().StringValue());
    return indexMeta;
}

TMaybe<TKqlQuery> BuildKqlQuery(TKiDataQuery query, const TKikimrTablesData& tablesData, TExprContext& ctx,
    bool withSystemColumns, const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx)
{
    TVector<TExprBase> kqlEffects;
    for (const auto& effect : query.Effects()) {
        if (auto maybeWrite = effect.Maybe<TKiWriteTable>()) {
            auto result = HandleWriteTable(maybeWrite.Cast(), ctx, tablesData);
            kqlEffects.push_back(result);
        }

        if (auto maybeUpdate = effect.Maybe<TKiUpdateTable>()) {
            auto results = HandleUpdateTable(maybeUpdate.Cast(), ctx, tablesData, withSystemColumns, kqpCtx);
            kqlEffects.insert(kqlEffects.end(), results.begin(), results.end());
        }

        if (auto maybeDelete = effect.Maybe<TKiDeleteTable>()) {
            auto results = HandleDeleteTable(maybeDelete.Cast(), ctx, tablesData, withSystemColumns, kqpCtx);
            kqlEffects.insert(kqlEffects.end(), results.begin(), results.end());
        }
    }

    TVector<TKqlQueryResult> kqlResults;
    kqlResults.reserve(query.Results().Size());
    for (const auto& kiResult : query.Results()) {
        kqlResults.emplace_back(
            Build<TKqlQueryResult>(ctx, kiResult.Pos())
                .Value(kiResult.Value())
                .ColumnHints(kiResult.Columns())
                .Done());
    }

    TKqlQuery kqlQuery = Build<TKqlQuery>(ctx, query.Pos())
        .Results()
            .Add(kqlResults)
            .Build()
        .Effects()
            .Add(kqlEffects)
            .Build()
        .Done();

    TExprNode::TPtr optResult;
    TOptimizeExprSettings optSettings(nullptr);
    optSettings.VisitChanges = true;
    auto status = OptimizeExpr(kqlQuery.Ptr(), optResult,
        [&tablesData, withSystemColumns, &kqpCtx](const TExprNode::TPtr& input, TExprContext& ctx) {
            auto node = TExprBase(input);
            TExprNode::TPtr effect;

            if (auto maybeRead = node.Maybe<TCoRight>().Input().Maybe<TKiReadTable>()) {
                return HandleReadTable(maybeRead.Cast(), ctx, tablesData, withSystemColumns, kqpCtx);
            }

            return input;
        }, ctx, optSettings);

    if (status == IGraphTransformer::TStatus::Error) {
        return {};
    }

    YQL_ENSURE(status == IGraphTransformer::TStatus::Ok);

    YQL_ENSURE(TMaybeNode<TKqlQuery>(optResult));
    return TKqlQuery(optResult);
}

} // namespace NKikimr::NKqp::NOpt
