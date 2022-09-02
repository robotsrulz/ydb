#include "kqp_opt_log_impl.h"
#include "kqp_opt_log_rules.h"

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/opt/kqp_opt_impl.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider_impl.h>

#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/dq/opt/dq_opt_log.h>
#include <ydb/library/yql/providers/common/provider/yql_table_lookup.h>

namespace NKikimr::NKqp::NOpt {

namespace {

using namespace NYql;
using namespace NYql::NCommon;
using namespace NYql::NDq;
using namespace NYql::NNodes;

TExprBase BuildEquiRangeLookup(const TKeyRange& keyRange, const TKikimrTableDescription& tableDesc,
    TPositionHandle pos, TExprContext& ctx)
{
    YQL_ENSURE(keyRange.IsEquiRange());

    TVector<TExprBase> structMembers;
    TVector<TCoAtom> skipNullColumns;
    for (ui32 i = 0; i < keyRange.GetNumDefined(); ++i) {
        const auto& columnName = tableDesc.Metadata->KeyColumnNames[i];
        TCoAtom columnNameAtom(ctx.NewAtom(pos, columnName));
        auto value = keyRange.GetFromTuple().GetValue(i).Cast();

        if (TCoNull::Match(value.Raw())) {
            value = Build<TCoNothing>(ctx, pos)
                .OptionalType(NCommon::BuildTypeExpr(pos, *tableDesc.GetColumnType(columnName), ctx))
                .Done();
        } else {
            skipNullColumns.push_back(columnNameAtom);
        }

        auto member = Build<TExprList>(ctx, pos)
            .Add(columnNameAtom)
            .Add(value)
            .Done();

        structMembers.push_back(member);
    }

    auto keysToLookup = Build<TCoAsList>(ctx, pos)
        .Add<TCoAsStruct>()
            .Add(structMembers)
            .Build()
        .Done();

    // Actually residual predicate for the key range already has a check for NULL keys,
    // but it's better to skip redundant lookup. Consider removing check from residual
    // predicate in this case.
    return Build<TCoSkipNullMembers>(ctx, pos)
        .Input(keysToLookup)
        .Members()
            .Add(skipNullColumns)
            .Build()
        .Done();
}

} // namespace

TKqlKeyRange BuildKeyRangeExpr(const TKeyRange& keyRange, const TKikimrTableDescription& tableDesc,
    TPositionHandle pos, TExprContext& ctx)
{
    bool fromInclusive = true;
    bool toInclusive = true;
    TVector<TExprBase> fromValues;
    TVector<TExprBase> toValues;

    for (size_t i = 0; i < keyRange.GetColumnRangesCount(); ++i) {
        const auto& columnName = tableDesc.Metadata->KeyColumnNames[i];
        const auto& range = keyRange.GetColumnRange(i);

        if (range.GetFrom().IsDefined()) {
            fromInclusive = range.GetFrom().IsInclusive();
            if (TCoNull::Match(range.GetFrom().GetValue().Raw())) {
                fromValues.emplace_back(
                    Build<TCoNothing>(ctx, pos)
                        .OptionalType(NCommon::BuildTypeExpr(pos, *tableDesc.GetColumnType(columnName), ctx))
                        .Done());
            } else {
                fromValues.emplace_back(range.GetFrom().GetValue());
            }
        }

        if (range.GetTo().IsDefined()) {
            toInclusive = range.GetTo().IsInclusive();
            if (TCoNull::Match(range.GetTo().GetValue().Raw())) {
                toValues.emplace_back(
                    Build<TCoNothing>(ctx, pos)
                        .OptionalType(NCommon::BuildTypeExpr(pos, *tableDesc.GetColumnType(columnName), ctx))
                        .Done());
            } else {
                toValues.emplace_back(range.GetTo().GetValue());
            }
        }
    }

    auto fromExpr = fromInclusive
        ? Build<TKqlKeyInc>(ctx, pos).Add(fromValues).Done().Cast<TKqlKeyTuple>()
        : Build<TKqlKeyExc>(ctx, pos).Add(fromValues).Done().Cast<TKqlKeyTuple>();

    auto toExpr = toInclusive
        ? Build<TKqlKeyInc>(ctx, pos).Add(toValues).Done().Cast<TKqlKeyTuple>()
        : Build<TKqlKeyExc>(ctx, pos).Add(toValues).Done().Cast<TKqlKeyTuple>();

    return Build<TKqlKeyRange>(ctx, pos)
        .From(fromExpr)
        .To(toExpr)
        .Done();
}

bool IsPointPrefix(const TKeyRange& range) {
    size_t prefixLen = 0;
    for (size_t i = 0; i < range.GetColumnRangesCount(); ++i) {
        if (range.GetColumnRange(i).IsPoint() && i == prefixLen) {
            prefixLen += 1;
        }
        if (range.GetColumnRange(i).IsDefined() && i >= prefixLen) {
            return false;
        }
    }
    return prefixLen > 0;
}

TExprBase KqpPushPredicateToReadTable(TExprBase node, TExprContext& ctx, const TKqpOptimizeContext& kqpCtx) {
    if (!node.Maybe<TCoFlatMap>()) {
        return node;
    }
    auto flatmap = node.Cast<TCoFlatMap>();

    if (!IsPredicateFlatMap(flatmap.Lambda().Body().Ref())) {
        return node;
    }

    bool onlyPointRanges = false;
    auto readMatch = MatchRead<TKqlReadTableBase>(flatmap.Input());

    //TODO: remove this branch KIKIMR-15255, KIKIMR-15321
    if (!readMatch && kqpCtx.IsDataQuery()) {
        if (auto readRangesMatch = MatchRead<TKqlReadTableRangesBase>(flatmap.Input())) {
            auto read = readRangesMatch->Read.Cast<TKqlReadTableRangesBase>();
            if (TCoVoid::Match(read.Ranges().Raw())) {
                auto key = Build<TKqlKeyInc>(ctx, read.Pos()).Done();
                readMatch = readRangesMatch;
                readMatch->Read =
                    Build<TKqlReadTable>(ctx, read.Pos())
                        .Settings(read.Settings())
                        .Table(read.Table())
                        .Columns(read.Columns())
                        .Range<TKqlKeyRange>()
                            .From(key)
                            .To(key)
                            .Build()
                        .Done();
                onlyPointRanges = true;
            } else {
                return node;
            }
        } else {
            return node;
        }
    }

    if (!readMatch) {
        return node;
    }

    if (readMatch->FlatMap) {
        return node;
    }

    auto read = readMatch->Read.Cast<TKqlReadTableBase>();

    static const std::set<TStringBuf> supportedReads {
        TKqlReadTable::CallableName(),
        TKqlReadTableIndex::CallableName(),
    };

    if (!supportedReads.contains(read.CallableName())) {
        return node;
    }

    TMaybeNode<TCoAtom> indexName;
    if (auto maybeIndexRead = read.Maybe<TKqlReadTableIndex>()) {
        indexName = maybeIndexRead.Cast().Index();
    }

    if (read.Range().From().ArgCount() > 0 || read.Range().To().ArgCount() > 0) {
        return node;
    }

    auto& mainTableDesc = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, read.Table().Path());

    auto& tableDesc = indexName ? kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, mainTableDesc.Metadata->GetIndexMetadata(TString(indexName.Cast())).first->Name) : mainTableDesc;

    if (tableDesc.Metadata->Kind == EKikimrTableKind::Olap) {
        return node;
    }

    auto row = flatmap.Lambda().Args().Arg(0);
    auto predicate = TExprBase(flatmap.Lambda().Body().Ref().ChildPtr(0));
    TTableLookup lookup = ExtractTableLookup(row, predicate, tableDesc.Metadata->KeyColumnNames,
        &KiTableLookupGetValue, &KiTableLookupCanCompare, &KiTableLookupCompare, ctx,
        kqpCtx.Config->HasAllowNullCompareInIndex());

    if (lookup.IsFullScan()) {
        return node;
    }

    auto readSettings = TKqpReadTableSettings::Parse(read);

    TVector<TExprBase> fetches;
    fetches.reserve(lookup.GetKeyRanges().size());

    for (auto& keyRange : lookup.GetKeyRanges()) {
        bool useLookup = false;
        if (onlyPointRanges && !IsPointPrefix(keyRange)) {
            return node;
        }
        if (keyRange.IsEquiRange()) {
            bool isFullKey = keyRange.GetNumDefined() == tableDesc.Metadata->KeyColumnNames.size();

            // NOTE: Use more efficient full key lookup implementation in datashard.
            // Consider using lookup for partial keys as well once better constant folding
            // is available, currently it can introduce redundant compute stage.
            useLookup = kqpCtx.IsDataQuery() && isFullKey;
        }

        TMaybeNode<TExprBase> readInput;
        if (useLookup) {
            auto lookupKeys = BuildEquiRangeLookup(keyRange, tableDesc, read.Pos(), ctx);

            if (indexName) {
                readInput = Build<TKqlLookupIndex>(ctx, read.Pos())
                    .Table(read.Table())
                    .LookupKeys(lookupKeys)
                    .Columns(read.Columns())
                    .Index(indexName.Cast())
                    .Done();
            } else {
                readInput = Build<TKqlLookupTable>(ctx, read.Pos())
                    .Table(read.Table())
                    .LookupKeys(lookupKeys)
                    .Columns(read.Columns())
                    .Done();
            }
        } else {
            auto keyRangeExpr = BuildKeyRangeExpr(keyRange, tableDesc, node.Pos(), ctx);

            TKqpReadTableSettings settings = readSettings;
            for (size_t i = 0; i < keyRange.GetColumnRangesCount(); ++i) {
                const auto& column = tableDesc.Metadata->KeyColumnNames[i];
                auto& range = keyRange.GetColumnRange(i);
                if (range.IsDefined() && !range.IsNull()) {
                    settings.AddSkipNullKey(column);
                }
            }

            if (indexName) {
                readInput = Build<TKqlReadTableIndex>(ctx, read.Pos())
                    .Table(read.Table())
                    .Range(keyRangeExpr)
                    .Columns(read.Columns())
                    .Index(indexName.Cast())
                    .Settings(settings.BuildNode(ctx, read.Pos()))
                    .Done();
            } else {
                readInput = Build<TKqlReadTable>(ctx, read.Pos())
                    .Table(read.Table())
                    .Range(keyRangeExpr)
                    .Columns(read.Columns())
                    .Settings(settings.BuildNode(ctx, read.Pos()))
                    .Done();
            }
        }

        auto input = readInput.Cast();

        auto residualPredicate = keyRange.GetResidualPredicate()
            ? keyRange.GetResidualPredicate().Cast().Ptr()
            : MakeBool<true>(node.Pos(), ctx);

        auto newBody = ctx.ChangeChild(flatmap.Lambda().Body().Ref(), 0, std::move(residualPredicate));

        input = readMatch->BuildProcessNodes(input, ctx);

        auto fetch = Build<TCoFlatMap>(ctx, node.Pos())
            .Input(input)
            .Lambda()
                .Args({"item"})
                .Body<TExprApplier>()
                    .Apply(TExprBase(newBody))
                    .With(flatmap.Lambda().Args().Arg(0), "item")
                    .Build()
                .Build()
            .Done();

        fetches.push_back(fetch);
    }

    return Build<TCoExtend>(ctx, node.Pos())
        .Add(fetches)
        .Done();
}

TExprBase KqpDropTakeOverLookupTable(const TExprBase& node, TExprContext&, const TKqpOptimizeContext& kqpCtx) {
    if (!node.Maybe<TCoTake>().Input().Maybe<TKqlLookupTableBase>()) {
        return node;
    }

    auto take = node.Cast<TCoTake>();
    auto lookupTable = take.Input().Cast<TKqlLookupTableBase>();

    if (!take.Count().Maybe<TCoUint64>()) {
        return node;
    }

    const ui64 count = FromString<ui64>(take.Count().Cast<TCoUint64>().Literal().Value());
    YQL_ENSURE(count > 0);

    auto maybeAsList = lookupTable.LookupKeys().Maybe<TCoAsList>();
    if (!maybeAsList) {
        maybeAsList = lookupTable.LookupKeys().Maybe<TCoIterator>().List().Maybe<TCoAsList>();
    }

    if (!maybeAsList) {
        return node;
    }

    if (maybeAsList.Cast().ArgCount() > count) {
        return node;
    }

    const auto tablePath = lookupTable.Table().Path().Value();
    const auto& table = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, tablePath);

    const auto& lookupKeys = GetSeqItemType(lookupTable.LookupKeys().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetItems();
    if (table.Metadata->KeyColumnNames.size() != lookupKeys.size()) {
        return node;
    }

    return lookupTable;
}

} // namespace NKikimr::NKqp::NOpt

