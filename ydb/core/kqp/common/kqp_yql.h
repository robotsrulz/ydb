#pragma once

#include "kqp_gateway.h"

#include <ydb/core/kqp/expr_nodes/kqp_expr_nodes.h>

namespace NYql {

const TStringBuf KqpEffectTag = "KqpEffect";

enum class EPhysicalQueryType {
    Unspecified,
    Data,
    Scan
};

struct TKqpPhyQuerySettings {
    static constexpr std::string_view TypeSettingName = "type"sv;
    std::optional<EPhysicalQueryType> Type;

    static TKqpPhyQuerySettings Parse(const NNodes::TKqpPhysicalQuery& node);
    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx, TPositionHandle pos) const;
};

enum class EPhysicalTxType {
    Unspecified,
    Compute,
    Data,
    Scan
};

struct TKqpPhyTxSettings {
    static constexpr TStringBuf TypeSettingName = "type";
    std::optional<EPhysicalTxType> Type;

    static constexpr std::string_view WithEffectsSettingName = "with_effects"sv;
    bool WithEffects = false;

    static TKqpPhyTxSettings Parse(const NNodes::TKqpPhysicalTx& node);
    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx, TPositionHandle pos) const;
};

struct TKqpReadTableSettings {
    static constexpr TStringBuf SkipNullKeysSettingName = "SkipNullKeys";
    static constexpr TStringBuf ItemsLimitSettingName = "ItemsLimit";
    static constexpr TStringBuf ReverseSettingName = "Reverse";
    static constexpr TStringBuf SortedSettingName = "Sorted";

    TVector<TString> SkipNullKeys;
    TExprNode::TPtr ItemsLimit;
    bool Reverse = false;
    bool Sorted = false;

    void AddSkipNullKey(const TString& key);
    void SetItemsLimit(const TExprNode::TPtr& expr) { ItemsLimit = expr; }
    void SetReverse() { Reverse = true; }
    void SetSorted() { Sorted = true; }

    static TKqpReadTableSettings Parse(const NNodes::TKqlReadTableBase& node);
    static TKqpReadTableSettings Parse(const NNodes::TKqlReadTableRangesBase& node);
    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx, TPositionHandle pos) const;
};

struct TKqpUpsertRowsSettings {
    static constexpr TStringBuf InplaceSettingName = "Inplace";

    bool Inplace = false;

    void SetInplace() { Inplace = true; }

    static TKqpUpsertRowsSettings Parse(const NNodes::TKqpUpsertRows& node);
    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx, TPositionHandle pos) const;
};

struct TKqpReadTableExplainPrompt {
    static constexpr TStringBuf UsedKeyColumnsName = "UsedKeyColumns";
    static constexpr TStringBuf ExpectedMaxRangesName = "ExpectedMaxRanges";

    TVector<TString> UsedKeyColumns;
    TString ExpectedMaxRanges;

    void SetUsedKeyColumns(TVector<TString> columns) {
        UsedKeyColumns = columns;
    }

    void SetExpectedMaxRanges(size_t count) {
        ExpectedMaxRanges = ToString(count);
    }

    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx, TPositionHandle pos) const;
    static TKqpReadTableExplainPrompt Parse(const NNodes::TKqlReadTableRangesBase& node);
};

TString KqpExprToPrettyString(const TExprNode& expr, TExprContext& ctx);
TString KqpExprToPrettyString(const NNodes::TExprBase& expr, TExprContext& ctx);

TString PrintKqpStageOnly(const NNodes::TDqStageBase& stage, TExprContext& ctx);

} // namespace NYql
