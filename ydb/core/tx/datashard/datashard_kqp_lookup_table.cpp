#include "datashard_kqp_compute.h"

#include <ydb/core/kqp/runtime/kqp_read_table.h>
#include <ydb/core/kqp/runtime/kqp_runtime_impl.h>
#include <ydb/core/engine/minikql/minikql_engine_host.h>

namespace NKikimr {
namespace NMiniKQL {

using namespace NTable;
using namespace NUdf;

namespace {

struct TParseLookupTableResult {
    ui32 CallableId = 0;
    TTableId TableId;
    TRuntimeNode LookupKeys;
    TVector<ui32> KeyIndices;
    TVector<NUdf::TDataTypeId> KeyTypes;

    TSmallVec<NTable::TTag> Columns;
    TSmallVec<NTable::TTag> SystemColumns;
    TSmallVec<bool> SkipNullKeys;
};

void ValidateLookupKeys(const TType* inputType, const THashMap<TString, NScheme::TTypeId>& keyColumns) {
    MKQL_ENSURE_S(inputType);
    auto rowType = AS_TYPE(TStructType, AS_TYPE(TStreamType, inputType)->GetItemType());

    for (ui32 i = 0; i < rowType->GetMembersCount(); ++i) {
        auto name = rowType->GetMemberName(i);
        auto dataType = NKqp::UnwrapDataTypeFromStruct(*rowType, i);

        auto columnType = keyColumns.FindPtr(name);
        MKQL_ENSURE_S(columnType);
        MKQL_ENSURE_S(dataType == *columnType, "Key column type mismatch, column: " << name);
    }
}

TParseLookupTableResult ParseLookupTable(TCallable& callable) {
    MKQL_ENSURE_S(callable.GetInputsCount() >= 4);

    TParseLookupTableResult result;

    result.CallableId = callable.GetUniqueId();
    MKQL_ENSURE_S(result.CallableId);

    auto tableNode = callable.GetInput(0);
    auto keysNode = callable.GetInput(1);
    auto keysIndicesNode = callable.GetInput(2);
    auto tagsNode = callable.GetInput(3);

    result.TableId = NKqp::ParseTableId(tableNode);
    result.LookupKeys = keysNode;

    auto keyIndices = AS_VALUE(TListLiteral, keysIndicesNode);
    result.KeyIndices.resize(keyIndices->GetItemsCount());
    for (ui32 i = 0; i < result.KeyIndices.size(); ++i) {
        result.KeyIndices[i] = AS_VALUE(TDataLiteral, keyIndices->GetItems()[i])->AsValue().Get<ui32>();;
    }

    auto keyTypes = AS_TYPE(TStructType, AS_TYPE(TStreamType, keysNode.GetStaticType())->GetItemType());
    result.KeyTypes.resize(keyTypes->GetMembersCount());
    for (ui32 i = 0; i < result.KeyTypes.size(); ++i) {
        if (keyTypes->GetMemberType(i)->IsOptional()) {
            auto type = AS_TYPE(TDataType, AS_TYPE(TOptionalType, keyTypes->GetMemberType(i))->GetItemType());
            result.KeyTypes[i] = type->GetSchemeType();
        } else {
            auto type = AS_TYPE(TDataType, keyTypes->GetMemberType(i));
            result.KeyTypes[i] = type->GetSchemeType();
        }
    }

    ParseReadColumns(callable.GetType()->GetReturnType(), tagsNode, result.Columns, result.SystemColumns);

    return result;
}

class TKqpLookupRowsWrapper : public TStatelessFlowComputationNode<TKqpLookupRowsWrapper> {
    using TBase = TStatelessFlowComputationNode<TKqpLookupRowsWrapper>;

public:
    TKqpLookupRowsWrapper(TComputationMutables& mutables, TKqpDatashardComputeContext& computeCtx,
        const TTypeEnvironment& typeEnv, const TParseLookupTableResult& parseResult, IComputationNode* lookupKeysNode)
        : TBase(mutables, this, EValueRepresentation::Boxed)
        , ComputeCtx(computeCtx)
        , TypeEnv(typeEnv)
        , ParseResult(parseResult)
        , LookupKeysNode(lookupKeysNode)
        , ColumnTags(ParseResult.Columns)
        , SystemColumnTags(ParseResult.SystemColumns)
        , ShardTableStats(ComputeCtx.GetDatashardCounters())
        , TaskTableStats(ComputeCtx.GetTaskCounters(ComputeCtx.GetCurrentTaskId()))
    {
        auto localTid = ComputeCtx.GetLocalTableId(ParseResult.TableId);
        auto tableInfo = ComputeCtx.Database->GetScheme().GetTableInfo(localTid);
        MKQL_ENSURE_S(tableInfo, "Unknown table " << ParseResult.TableId);
        MKQL_ENSURE_S(tableInfo->KeyColumns.size() == ParseResult.KeyIndices.size(),
            "Incomplete row key in LookupRows.");
    }

    TUnboxedValue DoCalculate(TComputationContext& ctx) const {
        auto keysValues = LookupKeysNode->GetValue(ctx);

        while (true) {
            NUdf::TUnboxedValue key;

            switch (keysValues.Fetch(key)) {
                case NUdf::EFetchStatus::Ok: {
                    TVector<TCell> keyCells(ParseResult.KeyIndices.size());
                    FillKeyTupleValue(key, ParseResult.KeyIndices, ParseResult.KeyTypes, keyCells, TypeEnv);

                    NUdf::TUnboxedValue result;
                    TKqpTableStats stats;
                    bool fetched = ComputeCtx.ReadRow(ParseResult.TableId, keyCells, ColumnTags, SystemColumnTags,
                        ctx.HolderFactory, result, stats);

                    if (stats.InvisibleRowSkips) {
                        ComputeCtx.BreakSetLocks();
                    }

                    ShardTableStats += stats;
                    TaskTableStats += stats;

                    if (fetched) {
                        return std::move(result);
                    }

                    if (ComputeCtx.IsTabletNotReady()) {
                        return NUdf::TUnboxedValue::MakeYield();
                    }

                    continue;
                }

                case NUdf::EFetchStatus::Finish:
                    return TUnboxedValue::MakeFinish();

                case NUdf::EFetchStatus::Yield:
                    MKQL_ENSURE_S(false);
                    return TUnboxedValue::MakeYield();
            }
        }

        Y_UNREACHABLE();
    }

private:
    void RegisterDependencies() const final {
        this->FlowDependsOn(LookupKeysNode);
    }

private:
    TKqpDatashardComputeContext& ComputeCtx;
    const TTypeEnvironment& TypeEnv;
    TParseLookupTableResult ParseResult;
    IComputationNode* LookupKeysNode;
    TSmallVec<TTag> ColumnTags;
    TSmallVec<TTag> SystemColumnTags;
    TKqpTableStats& ShardTableStats;
    TKqpTableStats& TaskTableStats;
};

class TKqpLookupTableWrapper : public TStatelessFlowComputationNode<TKqpLookupTableWrapper> {
    using TBase = TStatelessFlowComputationNode<TKqpLookupTableWrapper>;

public:
    TKqpLookupTableWrapper(TComputationMutables& mutables, TKqpDatashardComputeContext& computeCtx,
        const TTypeEnvironment& typeEnv, const TParseLookupTableResult& parseResult, IComputationNode* lookupKeysNode)
        : TBase(mutables, this, EValueRepresentation::Boxed)
        , ComputeCtx(computeCtx)
        , TypeEnv(typeEnv)
        , ParseResult(parseResult)
        , LookupKeysNode(lookupKeysNode)
        , ColumnTags(ParseResult.Columns)
        , SystemColumnTags(ParseResult.SystemColumns)
        , ShardTableStats(ComputeCtx.GetDatashardCounters())
        , TaskTableStats(ComputeCtx.GetTaskCounters(computeCtx.GetCurrentTaskId())) {}

    TUnboxedValue DoCalculate(TComputationContext& ctx) const {
        while (true) {
            if (!Iterator) {
                auto keysValues = LookupKeysNode->GetValue(ctx);

                NUdf::TUnboxedValue key;
                auto status = keysValues.Fetch(key);

                switch (status) {
                    case NUdf::EFetchStatus::Ok: {
                        auto localTid = ComputeCtx.GetLocalTableId(ParseResult.TableId);
                        auto tableInfo = ComputeCtx.Database->GetScheme().GetTableInfo(localTid);
                        MKQL_ENSURE_S(tableInfo);

                        TVector<TCell> fromCells(tableInfo->KeyColumns.size());
                        FillKeyTupleValue(key, ParseResult.KeyIndices, ParseResult.KeyTypes, fromCells, TypeEnv);

                        TVector<TCell> toCells(ParseResult.KeyIndices.size());
                        FillKeyTupleValue(key, ParseResult.KeyIndices, ParseResult.KeyTypes, toCells, TypeEnv);

                        auto range = TTableRange(fromCells, true, toCells, true);

                        Iterator = ComputeCtx.CreateIterator(ParseResult.TableId, range, ColumnTags);

                        ShardTableStats.NSelectRange++;
                        TaskTableStats.NSelectRange++;
                        break;
                    }

                    case NUdf::EFetchStatus::Finish:
                        return TUnboxedValue::MakeFinish();

                    case NUdf::EFetchStatus::Yield:
                        MKQL_ENSURE_S(false);
                        return TUnboxedValue::MakeYield();
                }
            }

            TUnboxedValue result;
            TKqpTableStats stats;

            bool fetched = ComputeCtx.ReadRow(ParseResult.TableId, *Iterator, SystemColumnTags,
                ParseResult.SkipNullKeys, ctx.HolderFactory, result, stats);


            if (stats.InvisibleRowSkips) {
                ComputeCtx.BreakSetLocks();
            }

            ShardTableStats += stats;
            TaskTableStats += stats;

            if (fetched) {
                return result;
            }

            if (ComputeCtx.IsTabletNotReady()) {
                return NUdf::TUnboxedValue::MakeYield();
            }

            Iterator = nullptr;
        }

        Y_UNREACHABLE();
    }

private:
    void RegisterDependencies() const final {
        this->FlowDependsOn(LookupKeysNode);
    }

private:
    TKqpDatashardComputeContext& ComputeCtx;
    const TTypeEnvironment& TypeEnv;
    TParseLookupTableResult ParseResult;
    IComputationNode* LookupKeysNode;
    TSmallVec<TTag> ColumnTags;
    TSmallVec<TTag> SystemColumnTags;
    TKqpTableStats& ShardTableStats;
    TKqpTableStats& TaskTableStats;
    mutable TAutoPtr<NTable::TTableIt> Iterator;
};

IComputationNode* WrapKqpLookupTableInternal(TCallable& callable, const TComputationNodeFactoryContext& ctx,
    TKqpDatashardComputeContext& computeCtx)
{
    auto parseResult = ParseLookupTable(callable);
    auto lookupKeysNode = LocateNode(ctx.NodeLocator, *parseResult.LookupKeys.GetNode());

    auto keyColumns = computeCtx.GetKeyColumnsMap(parseResult.TableId);
    ValidateLookupKeys(parseResult.LookupKeys.GetStaticType(), keyColumns);

    auto localTid = computeCtx.GetLocalTableId(parseResult.TableId);
    auto tableInfo = computeCtx.Database->GetScheme().GetTableInfo(localTid);
    MKQL_ENSURE_S(tableInfo);

    if (tableInfo->KeyColumns.size() == parseResult.KeyIndices.size()) {
        return new TKqpLookupRowsWrapper(ctx.Mutables, computeCtx, ctx.Env, parseResult, lookupKeysNode);
    } else {
        return new TKqpLookupTableWrapper(ctx.Mutables, computeCtx, ctx.Env, parseResult, lookupKeysNode);
    }
}

} // namespace

IComputationNode* WrapKqpLookupTable(TCallable& callable, const TComputationNodeFactoryContext& ctx,
    TKqpDatashardComputeContext& computeCtx)
{
    return WrapKqpLookupTableInternal(callable, ctx, computeCtx);
}

} // namespace NMiniKQL
} // namespace NKikimr
