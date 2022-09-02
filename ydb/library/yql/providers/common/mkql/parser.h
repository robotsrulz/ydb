#pragma once

#include <ydb/library/yql/ast/yql_expr.h>

#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/providers/dq/expr_nodes/dqs_expr_nodes.h>
#include <ydb/library/yql/providers/common/mkql/yql_type_mkql.h>
#include <ydb/library/yql/providers/common/mkql/yql_provider_mkql.h>
#include <ydb/library/yql/minikql/mkql_node_cast.h>
#include <ydb/library/yql/minikql/mkql_node.h>

#include <algorithm>

namespace NYql {

NKikimr::NMiniKQL::TRuntimeNode BuildParseCall(
    TPosition pos,
    NKikimr::NMiniKQL::TRuntimeNode input,
    TMaybe<NKikimr::NMiniKQL::TRuntimeNode> extraColumnsByPathIndex,
    std::unordered_map<TString, ui32>&& metadataColumns,
    const std::string_view& format,
    const std::string_view& compression,
    NKikimr::NMiniKQL::TType* inputType,
    NKikimr::NMiniKQL::TType* parseItemType,
    NKikimr::NMiniKQL::TType* finalItemType,
    NCommon::TMkqlBuildContext& ctx);

TMaybe<NKikimr::NMiniKQL::TRuntimeNode> TryWrapWithParser(const NYql::NNodes::TDqSourceWideWrap& wrapper, NCommon::TMkqlBuildContext& ctx);

}
