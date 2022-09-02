#pragma once

#include <ydb/library/yql/ast/yql_expr.h>
#include <ydb/library/yql/core/yql_data_provider.h>
#include <ydb/library/yql/core/yql_graph_transformer.h>
#include <ydb/library/yql/core/yql_expr_optimize.h>
#include <ydb/library/yql/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/core/expr_nodes/yql_expr_nodes.h>

#include <library/cpp/yson/writer.h>

#include <util/generic/hash_set.h>
#include <util/generic/string.h>
#include <util/generic/strbuf.h>

#include <utility>

namespace NYson {
    class TYsonWriter;
}

namespace NKikimr {
    namespace NMiniKQL {
        class IFunctionRegistry;
    }
}

namespace NYql {

struct TTypeAnnotationContext;
struct TOperationStatistics;

namespace NCommon {

struct TWriteTableSettings {
    NNodes::TMaybeNode<NNodes::TCoAtom> Mode;
    NNodes::TMaybeNode<NNodes::TExprList> Columns;
    NNodes::TMaybeNode<NNodes::TCoAtomList> PrimaryKey;
    NNodes::TMaybeNode<NNodes::TCoAtomList> PartitionBy;
    NNodes::TMaybeNode<NNodes::TCoNameValueTupleList> OrderBy;
    NNodes::TMaybeNode<NNodes::TCoLambda> Filter;
    NNodes::TMaybeNode<NNodes::TCoLambda> Update;
    NNodes::TMaybeNode<NNodes::TCoIndexList> Indexes;
    NNodes::TMaybeNode<NNodes::TCoChangefeedList> Changefeeds;
    NNodes::TCoNameValueTupleList Other;
    NNodes::TMaybeNode<NNodes::TExprList> ColumnFamilies;
    NNodes::TMaybeNode<NNodes::TCoNameValueTupleList> TableSettings;
    NNodes::TMaybeNode<NNodes::TCoNameValueTupleList> AlterActions;

    TWriteTableSettings(const NNodes::TCoNameValueTupleList& other)
        : Other(other) {}
};

struct TWriteRoleSettings {
    NNodes::TMaybeNode<NNodes::TCoAtom> Mode;
    NNodes::TMaybeNode<NNodes::TCoAtomList> Roles;
    NNodes::TCoNameValueTupleList Other;

    TWriteRoleSettings(const NNodes::TCoNameValueTupleList& other)
        : Other(other) {}
};

struct TCommitSettings
{
    TPositionHandle Pos;
    NNodes::TMaybeNode<NNodes::TCoAtom> Mode;
    NNodes::TMaybeNode<NNodes::TCoAtom> Epoch;
    NNodes::TCoNameValueTupleList Other;

    TCommitSettings(NNodes::TCoNameValueTupleList other)
        : Other(other) {}

    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx) const;

    bool EnsureModeEmpty(TExprContext& ctx);
    bool EnsureEpochEmpty(TExprContext& ctx);
    bool EnsureOtherEmpty(TExprContext& ctx);
};

const TStructExprType* BuildCommonTableListType(TExprContext& ctx);

TExprNode::TPtr BuildTypeExpr(TPositionHandle pos, const TTypeAnnotationNode& ann, TExprContext& ctx);

bool HasResOrPullOption(const TExprNode& node, const TStringBuf& option);

TVector<TString> GetResOrPullColumnHints(const TExprNode& node);

TWriteTableSettings ParseWriteTableSettings(NNodes::TExprList node, TExprContext& ctx);

TWriteRoleSettings ParseWriteRoleSettings(NNodes::TExprList node, TExprContext& ctx);

TCommitSettings ParseCommitSettings(NNodes::TCoCommit node, TExprContext& ctx);

TString FullTableName(const TStringBuf& cluster, const TStringBuf& table);

IDataProvider::TFillSettings GetFillSettings(const TExprNode& node);
NYson::EYsonFormat GetYsonFormat(const IDataProvider::TFillSettings& fillSettings);

TVector<TString> GetStructFields(const TTypeAnnotationNode* type);

void TransformerStatsToYson(const TString& name, const IGraphTransformer::TStatistics& stats, NYson::TYsonWriter& writer);

TString TransformerStatsToYson(const IGraphTransformer::TStatistics& stats, NYson::EYsonFormat format
    = NYson::EYsonFormat::Pretty);

void FillSecureParams(const TExprNode::TPtr& node, const TTypeAnnotationContext& types, THashMap<TString, TString>& secureParams);

bool FillUsedFiles(const TExprNode& node, TUserDataTable& files, const TTypeAnnotationContext& types, TExprContext& ctx, const TUserDataTable& crutches = {});

std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture> FreezeUsedFiles(const TExprNode& node, TUserDataTable& files, const TTypeAnnotationContext& types, TExprContext& ctx, const std::function<bool(const TString&)>& urlDownloadFilter, const TUserDataTable& crutches = {});

bool FreezeUsedFilesSync(const TExprNode& node, TUserDataTable& files, const TTypeAnnotationContext& types, TExprContext& ctx, const std::function<bool(const TString&)>& urlDownloadFilter);

void WriteColumns(NYson::TYsonWriter& writer, const NNodes::TExprBase& columns);

TString SerializeExpr(TExprContext& ctx, const TExprNode& expr, bool withTypes = false);
TString ExprToPrettyString(TExprContext& ctx, const TExprNode& expr);

void WriteStream(NYson::TYsonWriter& writer, const TExprNode* node, const TExprNode* source);
void WriteStreams(NYson::TYsonWriter& writer, TStringBuf name, const NNodes::TCoLambda& lambda);

double GetDataReplicationFactor(const TExprNode& lambda, TExprContext& ctx);

void WriteStatistics(NYson::TYsonWriter& writer, bool totalOnly, const THashMap<ui32, TOperationStatistics>& statistics);

bool ValidateCompressionForInput(std::string_view compression, TExprContext& ctx);
bool ValidateCompressionForOutput(std::string_view compression, TExprContext& ctx);

bool ValidateFormatForInput(std::string_view format, TExprContext& ctx);
bool ValidateFormatForOutput(std::string_view format, TExprContext& ctx);

bool ValidateIntervalUnit(std::string_view format, TExprContext& ctx);

} // namespace NCommon
} // namespace NYql
