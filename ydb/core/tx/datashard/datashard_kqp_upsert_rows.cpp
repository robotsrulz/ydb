#include "datashard_kqp_compute.h"
#include "datashard_user_table.h"

#include <ydb/core/engine/mkql_keys.h>
#include <ydb/core/engine/mkql_engine_flat_host.h>
#include <ydb/core/kqp/runtime/kqp_runtime_impl.h>

#include <ydb/library/yql/minikql/computation/mkql_computation_node_holders.h>
#include <ydb/library/yql/minikql/computation/mkql_computation_node_impl.h>
#include <ydb/library/yql/minikql/mkql_node.h>
#include <ydb/library/yql/minikql/mkql_node_cast.h>

#include <util/generic/cast.h>

namespace NKikimr {
namespace NMiniKQL {

using namespace NTable;
using namespace NUdf;

namespace {

struct TUpsertColumn {
    ui32 ColumnId;
    ui32 RowIndex;
};

class TKqpUpsertRowsWrapper : public TMutableComputationNode<TKqpUpsertRowsWrapper> {
    using TBase = TMutableComputationNode<TKqpUpsertRowsWrapper>;

public:
    class TRowResult : public TComputationValue<TRowResult> {
        using TBase = TComputationValue<TRowResult>;

    public:
        TRowResult(TMemoryUsageInfo* memInfo, const TKqpUpsertRowsWrapper& owner,
            NUdf::TUnboxedValue&& row)
            : TBase(memInfo)
            , Owner(owner)
            , Row(std::move(row)) {}

    private:
        void Apply(NUdf::IApplyContext& applyContext) const override {
            auto& dsApplyCtx = *CheckedCast<TKqpDatashardApplyContext*>(&applyContext);

            TVector<TCell> keyTuple(Owner.KeyIndices.size());
            FillKeyTupleValue(Row, Owner.KeyIndices, Owner.RowTypes, keyTuple, Owner.Env);

            if (dsApplyCtx.Host->IsPathErased(Owner.TableId)) {
                return;
            }

            if (!dsApplyCtx.Host->IsMyKey(Owner.TableId, keyTuple)) {
                return;
            }

            TVector<IEngineFlatHost::TUpdateCommand> commands;
            commands.reserve(Owner.UpsertColumns.size());

            for (auto& upsertColumn : Owner.UpsertColumns) {
                IEngineFlatHost::TUpdateCommand command;
                command.Column = upsertColumn.ColumnId;
                command.Operation = TKeyDesc::EColumnOperation::Set;
                auto rowIndex = upsertColumn.RowIndex;

                NUdf::TDataTypeId type = Owner.RowTypes[rowIndex];
                NUdf::TUnboxedValue value = Row.GetElement(rowIndex);

                if (value) {
                    auto slot = NUdf::GetDataSlot(type);
                    MKQL_ENSURE(IsValidValue(slot, value),
                        "Malformed value for type: " << NUdf::GetDataTypeInfo(slot).Name << ", " << value);
                }

                // NOTE: We have to copy values here as some values inlined in TUnboxedValue
                // cannot be inlined in TCell.
                command.Value = MakeCell(type, value, Owner.Env, true);

                commands.emplace_back(std::move(command));
            }

            ui64 nUpdateRow = Owner.ShardTableStats.NUpdateRow;
            ui64 updateRowBytes = Owner.ShardTableStats.UpdateRowBytes;

            dsApplyCtx.Host->UpdateRow(Owner.TableId, keyTuple, commands);

            if (i64 delta = Owner.ShardTableStats.NUpdateRow - nUpdateRow; delta > 0) {
                Owner.TaskTableStats.NUpdateRow += delta;
                Owner.TaskTableStats.UpdateRowBytes += Owner.ShardTableStats.UpdateRowBytes - updateRowBytes;
            }
        };

    private:
        const TKqpUpsertRowsWrapper& Owner;
        NUdf::TUnboxedValue Row;
    };

    class TRowsResult : public TComputationValue<TRowsResult> {
        using TBase = TComputationValue<TRowsResult>;

    public:
        TRowsResult(TMemoryUsageInfo* memInfo, const TKqpUpsertRowsWrapper& owner,
            NUdf::TUnboxedValue&& rows)
            : TBase(memInfo)
            , Owner(owner)
            , Rows(std::move(rows)) {}

        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& result) final {
            NUdf::TUnboxedValue row;
            auto status = Rows.Fetch(row);

            if (status == NUdf::EFetchStatus::Ok) {
                result = NUdf::TUnboxedValuePod(new TRowResult(GetMemInfo(), Owner, std::move(row)));
            }

            return status;
        }

    private:
        const TKqpUpsertRowsWrapper& Owner;
        NUdf::TUnboxedValue Rows;
    };

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<TRowsResult>(*this, RowsNode->GetValue(ctx));
    }

public:
    TKqpUpsertRowsWrapper(TComputationMutables& mutables, TKqpDatashardComputeContext& computeCtx,
        const TTableId& tableId, IComputationNode* rowsNode, TVector<NUdf::TDataTypeId>&& rowTypes,
        TVector<ui32>&& keyIndices, TVector<TUpsertColumn>&& upsertColumns, const TTypeEnvironment& env)
        : TBase(mutables)
        , TableId(tableId)
        , RowsNode(rowsNode)
        , RowTypes(std::move(rowTypes))
        , KeyIndices(std::move(keyIndices))
        , UpsertColumns(std::move(upsertColumns))
        , Env(env)
        , ShardTableStats(computeCtx.GetDatashardCounters())
        , TaskTableStats(computeCtx.GetTaskCounters(computeCtx.GetCurrentTaskId())) {}

private:
    void RegisterDependencies() const final {
        DependsOn(RowsNode);
    }

private:
    TTableId TableId;
    IComputationNode* RowsNode;
    TVector<NUdf::TDataTypeId> RowTypes;
    TVector<ui32> KeyIndices;
    TVector<TUpsertColumn> UpsertColumns;
    const TTypeEnvironment& Env;
    TKqpTableStats& ShardTableStats;
    TKqpTableStats& TaskTableStats;
};

} // namespace

IComputationNode* WrapKqpUpsertRows(TCallable& callable, const TComputationNodeFactoryContext& ctx,
    TKqpDatashardComputeContext& computeCtx)
{
    MKQL_ENSURE_S(callable.GetInputsCount() >= 3);

    auto tableNode = callable.GetInput(0);
    auto rowsNode = callable.GetInput(1);
    auto upsertColumnsNode = callable.GetInput(2);

    auto tableId = NKqp::ParseTableId(tableNode);
    auto tableInfo = computeCtx.GetTable(tableId);
    MKQL_ENSURE(tableInfo, "Table not found: " << tableId.PathId.ToString());

    auto rowType = AS_TYPE(TStructType, AS_TYPE(TStreamType, rowsNode.GetStaticType())->GetItemType());

    MKQL_ENSURE_S(tableInfo->KeyColumnIds.size() <= rowType->GetMembersCount(),
        "not enough columns in the runtime node");

    THashMap<TStringBuf, ui32> inputIndex;
    TVector<NUdf::TDataTypeId> rowTypes(rowType->GetMembersCount());
    for (ui32 i = 0; i < rowTypes.size(); ++i) {
        const auto& name = rowType->GetMemberName(i);
        MKQL_ENSURE_S(inputIndex.emplace(name, i).second);
        rowTypes[i] = NKqp::UnwrapDataTypeFromStruct(*rowType, i);
    }

    TVector<ui32> keyIndices(tableInfo->KeyColumnIds.size());
    for (ui32 i = 0; i < keyIndices.size(); i++) {
        auto& columnInfo = computeCtx.GetKeyColumnInfo(*tableInfo, i);

        auto it = inputIndex.find(columnInfo.Name);
        MKQL_ENSURE_S(it != inputIndex.end());
        auto typeId = NKqp::UnwrapDataTypeFromStruct(*rowType, it->second);
        MKQL_ENSURE_S(typeId == columnInfo.Type, "row key type missmatch with table key type");
        keyIndices[i] = it->second;
    }

    for (const auto& [_, column] : tableInfo->Columns) {
        if (column.NotNull) {
            auto it = inputIndex.find(column.Name);
            MKQL_ENSURE(it != inputIndex.end(),
                "Not null column " << column.Name << " has to be specified in upsert");

            auto columnType = rowType->GetMemberType(it->second);
            MKQL_ENSURE(columnType->GetKind() != NMiniKQL::TType::EKind::Optional,
                "Not null column " << column.Name << " can't be optional");
        }
    }

    auto upsertColumnsDict = AS_VALUE(TDictLiteral, upsertColumnsNode);
    TVector<TUpsertColumn> upsertColumns(upsertColumnsDict->GetItemsCount());
    for (ui32 i = 0; i < upsertColumns.size(); ++i) {
        auto item = upsertColumnsDict->GetItem(i);

        auto& upsertColumn = upsertColumns[i];
        upsertColumn.ColumnId = AS_VALUE(TDataLiteral, item.first)->AsValue().Get<ui32>();
        upsertColumn.RowIndex = AS_VALUE(TDataLiteral, item.second)->AsValue().Get<ui32>();

        auto tableColumn = tableInfo->Columns.FindPtr(upsertColumn.ColumnId);
        MKQL_ENSURE_S(tableColumn);

        MKQL_ENSURE_S(rowTypes[upsertColumn.RowIndex] == tableColumn->Type,
            "upsert column type missmatch, column: " << tableColumn->Name);
    }

    return new TKqpUpsertRowsWrapper(ctx.Mutables, computeCtx, tableId,
        LocateNode(ctx.NodeLocator, *rowsNode.GetNode()), std::move(rowTypes), std::move(keyIndices),
        std::move(upsertColumns), ctx.Env);
}

} // namespace NMiniKQL
} // namespace NKikimr
