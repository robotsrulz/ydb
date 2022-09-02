#pragma once
#include "defs.h"

#include "rpc_deferrable.h"

#include <ydb/core/base/kikimr_issue.h>
#include <ydb/core/cms/console/configs_dispatcher.h>
#include <ydb/core/kqp/kqp.h>
#include <ydb/core/ydb_convert/ydb_convert.h>
#include <ydb/public/lib/operation_id/operation_id.h>
#include <ydb/public/sdk/cpp/client/resources/ydb_resources.h>

namespace NKikimr {
namespace NGRpcService {


inline TString DecodePreparedQueryId(const TString& in) {
    if (in.empty()) {
        throw NYql::TErrorException(NKikimrIssues::TIssuesIds::DEFAULT_ERROR)
            << "got empty preparedQueryId message";
    }
    NOperationId::TOperationId opId(in);
    const auto& ids = opId.GetValue("id");
    if (ids.size() != 1) {
        throw NYql::TErrorException(NKikimrIssues::TIssuesIds::DEFAULT_ERROR)
            << "expected exactly one preparedQueryId identifier";
    }
    return *ids[0];
}

inline TString GetTransactionModeName(const Ydb::Table::TransactionSettings& settings) {
    switch (settings.tx_mode_case()) {
        case Ydb::Table::TransactionSettings::kSerializableReadWrite:
            return "SerializableReadWrite";
        case Ydb::Table::TransactionSettings::kOnlineReadOnly:
            return "OnlineReadOnly";
        case Ydb::Table::TransactionSettings::kStaleReadOnly:
            return "StaleReadOnly";
        case Ydb::Table::TransactionSettings::kSnapshotReadOnly:
            return "SnapshotReadOnly";
        default:
            return "Unknown";
    }
}

inline NYql::NDqProto::EDqStatsMode GetKqpStatsMode(Ydb::Table::QueryStatsCollection::Mode mode) {
    switch (mode) {
        case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_BASIC:
            return NYql::NDqProto::DQ_STATS_MODE_BASIC;
        case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
            return NYql::NDqProto::DQ_STATS_MODE_PROFILE;
        default:
            return NYql::NDqProto::DQ_STATS_MODE_NONE;
    }
}

inline bool CheckSession(const TString& sessionId, NYql::TIssues& issues) {
    if (sessionId.empty()) {
        issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Empty session id"));
        return false;
    }

    return true;
}

inline bool CheckQuery(const TString& query, NYql::TIssues& issues) {
    if (query.empty()) {
        issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Empty query text"));
        return false;
    }

    return true;
}

void FillQueryStats(Ydb::TableStats::QueryStats& queryStats, const NKikimrKqp::TQueryResponse& kqpResponse);

inline void ConvertKqpQueryResultToDbResult(const NKikimrMiniKQL::TResult& from, Ydb::ResultSet* to) {
    const auto& type = from.GetType();
    TStackVec<NKikimrMiniKQL::TType> columnTypes;
    Y_ENSURE(type.GetKind() == NKikimrMiniKQL::ETypeKind::Struct);
    for (const auto& member : type.GetStruct().GetMember()) {
        if (member.GetType().GetKind() == NKikimrMiniKQL::ETypeKind::List) {
            for (const auto& column : member.GetType().GetList().GetItem().GetStruct().GetMember()) {
                auto columnMeta = to->add_columns();
                columnMeta->set_name(column.GetName());
                columnTypes.push_back(column.GetType());
                ConvertMiniKQLTypeToYdbType(column.GetType(), *columnMeta->mutable_type());
            }
        }
    }
    for (const auto& responseStruct : from.GetValue().GetStruct()) {
        for (const auto& row : responseStruct.GetList()) {
            auto newRow = to->add_rows();
            ui32 columnCount = static_cast<ui32>(row.StructSize());
            Y_ENSURE(columnCount == columnTypes.size());
            for (ui32 i = 0; i < columnCount; i++) {
                const auto& column = row.GetStruct(i);
                ConvertMiniKQLValueToYdbValue(columnTypes[i], column, *newRow->add_items());
            }
        }
        if (responseStruct.Getvalue_valueCase() == NKikimrMiniKQL::TValue::kBool) {
            to->set_truncated(responseStruct.GetBool());
        }
    }
}

template<typename TFrom, typename TTo>
inline void ConvertKqpQueryResultsToDbResult(const TFrom& from, TTo* to) {
    const auto& results = from.GetResults();
    for (const auto& result : results) {
        ConvertKqpQueryResultToDbResult(result, to->add_result_sets());
    }
}

template <typename TDerived, typename TRequest>
class TRpcKqpRequestActor : public TRpcOperationRequestActor<TDerived, TRequest> {
    using TBase = TRpcOperationRequestActor<TDerived, TRequest>;

public:
    TRpcKqpRequestActor(IRequestOpCtx* request)
        : TBase(request) {}

    void OnOperationTimeout(const TActorContext& ctx) {
        Y_UNUSED(ctx);
    }

protected:
    void StateWork(TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NKqp::TEvKqp::TEvProcessResponse, Handle);
            default: TBase::StateFuncBase(ev, ctx);
        }
    }

    template<typename TKqpResponse>
    void AddServerHintsIfAny(const TKqpResponse& kqpResponse) {
        if (kqpResponse.GetWorkerIsClosing()) {
            this->Request_->AddServerHint(TString(NYdb::YDB_SESSION_CLOSE));
        }
    }

    template<typename TKqpResponse>
    void OnGenericQueryResponseError(const TKqpResponse& kqpResponse, const TActorContext& ctx) {
        RaiseIssuesFromKqp(kqpResponse);

        this->Request_->ReplyWithYdbStatus(kqpResponse.GetYdbStatus());
        this->Die(ctx);
    }

    template<typename TKqpResponse>
    void OnQueryResponseErrorWithTxMeta(const TKqpResponse& kqpResponse, const TActorContext& ctx) {
        RaiseIssuesFromKqp(kqpResponse);

        auto queryResult = TRequest::template AllocateResult<typename TDerived::TResult>(this->Request_);
        if (kqpResponse.GetResponse().HasTxMeta()) {
            queryResult->mutable_tx_meta()->CopyFrom(kqpResponse.GetResponse().GetTxMeta());
        }

        this->Request_->SendResult(*queryResult, kqpResponse.GetYdbStatus());
        this->Die(ctx);
    }

    void OnQueryResponseError(const NKikimrKqp::TEvCreateSessionResponse& kqpResponse, const TActorContext& ctx) {
        if (kqpResponse.HasError()) {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, kqpResponse.GetError()));
            return this->Reply(kqpResponse.GetYdbStatus(), issues, ctx);
        } else {
            return this->Reply(kqpResponse.GetYdbStatus(), ctx);
        }
    }

    template<typename TKqpResponse>
    void OnKqpError(const TKqpResponse& response, const TActorContext& ctx) {
        NYql::TIssues issues;
        NYql::IssuesFromMessage(response.GetIssues(), issues);

        this->Request_->RaiseIssues(issues);
        this->Request_->ReplyWithYdbStatus(response.GetStatus());
        this->Die(ctx);
    }

    void OnProcessError(const NKikimrKqp::TEvProcessResponse& kqpResponse, const TActorContext& ctx) {
        if (kqpResponse.HasError()) {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, kqpResponse.GetError()));
            return this->Reply(kqpResponse.GetYdbStatus(), issues, ctx);
        } else {
            return this->Reply(kqpResponse.GetYdbStatus(), ctx);
        }
    }

private:
    void Handle(NKqp::TEvKqp::TEvProcessResponse::TPtr& ev, const TActorContext& ctx) {
        auto& record = ev->Get()->Record;
        NYql::TIssues issues;
        if (record.HasError()) {
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, record.GetError()));
        }
        return this->Reply(record.GetYdbStatus(), issues, ctx);
    }

private:
    template<typename TKqpResponse>
    void RaiseIssuesFromKqp(const TKqpResponse& kqpResponse) {
        NYql::TIssues issues;
        const auto& issueMessage = kqpResponse.GetResponse().GetQueryIssues();
        NYql::IssuesFromMessage(issueMessage, issues);
        this->Request_->RaiseIssues(issues);
    }
};

} // namespace NGRpcService
} // namespace NKikimr
