#include "service_yql_scripting.h"
#include "rpc_kqp_base.h"
#include "rpc_common.h"

#include <ydb/public/api/protos/ydb_scripting.pb.h>

namespace NKikimr {
namespace NGRpcService {

using Ydb::Scripting::ExecuteYqlRequest;
using Ydb::Scripting::ExecuteYqlResponse;
using TEvExecuteYqlScriptRequest = TGrpcRequestOperationCall<ExecuteYqlRequest, ExecuteYqlResponse>;

using namespace Ydb;

class TExecuteYqlScriptRPC : public TRpcKqpRequestActor<TExecuteYqlScriptRPC, TEvExecuteYqlScriptRequest> {
    using TBase = TRpcKqpRequestActor<TExecuteYqlScriptRPC, TEvExecuteYqlScriptRequest>;

public:
    using TResult = Ydb::Scripting::ExecuteYqlResult;

    TExecuteYqlScriptRPC(IRequestOpCtx* msg)
        : TBase(msg) {}

    void Bootstrap(const TActorContext &ctx) {
        TBase::Bootstrap(ctx);

        this->Become(&TExecuteYqlScriptRPC::StateWork);
        Proceed(ctx);
    }

    void StateWork(TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NKqp::TEvKqp::TEvQueryResponse, Handle);
            default: TBase::StateWork(ev, ctx);
        }
    }

    void Proceed(const TActorContext &ctx) {
        const auto req = GetProtoRequest();
        const auto traceId = Request_->GetTraceId();

        auto ev = MakeHolder<NKqp::TEvKqp::TEvQueryRequest>();
        SetAuthToken(ev, *Request_);
        SetDatabase(ev, *Request_);

        if (traceId) {
            ev->Record.SetTraceId(traceId.GetRef());
        }

        ev->Record.MutableRequest()->SetCancelAfterMs(GetCancelAfter().MilliSeconds());
        ev->Record.MutableRequest()->SetTimeoutMs(GetOperationTimeout().MilliSeconds());

        if (req->parametersSize() != 0) {
            try {
                NKikimrMiniKQL::TParams params;
                ConvertYdbParamsToMiniKQLParams(req->parameters(), params);
                ev->Record.MutableRequest()->MutableParameters()->CopyFrom(params);
            } catch (const std::exception& ex) {
                NYql::TIssue issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, TStringBuilder()
                    << "Failed to parse script parameters.");
                issue.AddSubIssue(MakeIntrusive<NYql::TIssue>(NYql::ExceptionToIssue(ex)));

                NYql::TIssues issues;
                issues.AddIssue(issue);

                return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx);
            }
        }

        auto& script = req->script();

        NYql::TIssues issues;
        if (!CheckQuery(script, issues)) {
            return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx);
        }

        ev->Record.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
        ev->Record.MutableRequest()->SetType(NKikimrKqp::QUERY_TYPE_SQL_SCRIPT);
        ev->Record.MutableRequest()->SetQuery(script);
        ev->Record.MutableRequest()->SetKeepSession(false);
        ev->Record.MutableRequest()->SetStatsMode(GetKqpStatsMode(req->collect_stats()));
        ev->Record.MutableRequest()->SetCollectStats(req->collect_stats());

        ctx.Send(NKqp::MakeKqpProxyID(ctx.SelfID.NodeId()), ev.Release());
    }

    void Handle(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record.GetRef();
        SetCost(record.GetConsumedRu());
        AddServerHintsIfAny(record);

        if (record.GetYdbStatus() != Ydb::StatusIds::SUCCESS) {
            return OnGenericQueryResponseError(record, ctx);
        }

        const auto& kqpResponse = record.GetResponse();
        const auto& issueMessage = kqpResponse.GetQueryIssues();

        auto queryResult = TEvExecuteYqlScriptRequest::AllocateResult<TResult>(Request_);
        ConvertKqpQueryResultsToDbResult(kqpResponse, queryResult);

        if (kqpResponse.HasQueryStats()) {
            FillQueryStats(*queryResult->mutable_query_stats(), kqpResponse);
        } else if (kqpResponse.HasQueryPlan()) {
            queryResult->mutable_query_stats()->set_query_plan(kqpResponse.GetQueryPlan());
        }

        ReplyWithResult(Ydb::StatusIds::SUCCESS, issueMessage, *queryResult, ctx);
    }
};

void DoExecuteYqlScript(std::unique_ptr<IRequestOpCtx> p, const IFacilityProvider&) {
    TActivationContext::AsActorContext().Register(new TExecuteYqlScriptRPC(p.release()));
}

} // namespace NGRpcService
} // namespace NKikimr
