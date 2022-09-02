#include "service_table.h"
#include <ydb/core/grpc_services/base/base.h>

#include "rpc_common.h"
#include "rpc_kqp_base.h"
#include "service_table.h"

#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/kikimr_issue.h>
#include <ydb/core/kqp/executer/kqp_executer.h>
#include <ydb/core/kqp/prepare/kqp_query_plan.h>

#include <ydb/core/protos/services.pb.h>
#include <ydb/core/protos/ydb_table_impl.pb.h>
#include <ydb/core/ydb_convert/ydb_convert.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>

namespace NKikimr {
namespace NGRpcService {

namespace {

using namespace NActors;
using namespace Ydb;
using namespace Ydb::Table;
using namespace NKqp;

struct TParseRequestError {
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;

    TParseRequestError()
        : Status(Ydb::StatusIds::INTERNAL_ERROR)
        , Issues({MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR,
            "Unexpected error while parsing request.")}) {}

    TParseRequestError(const Ydb::StatusIds::StatusCode& status, const NYql::TIssues& issues)
        : Status(status)
        , Issues(issues) {}
};

bool NeedReportStats(const Ydb::Experimental::ExecuteStreamQueryRequest& req) {
    switch (req.profile_mode()) {
        case Experimental::ExecuteStreamQueryRequest_ProfileMode_PROFILE_MODE_UNSPECIFIED:
        case Experimental::ExecuteStreamQueryRequest_ProfileMode_NONE:
            return false;

        case Experimental::ExecuteStreamQueryRequest_ProfileMode_BASIC:
        case Experimental::ExecuteStreamQueryRequest_ProfileMode_FULL:
        case Experimental::ExecuteStreamQueryRequest_ProfileMode_PROFILE:
            return true;

        case Experimental::ExecuteStreamQueryRequest_ProfileMode_ExecuteStreamQueryRequest_ProfileMode_INT_MIN_SENTINEL_DO_NOT_USE_:
        case Experimental::ExecuteStreamQueryRequest_ProfileMode_ExecuteStreamQueryRequest_ProfileMode_INT_MAX_SENTINEL_DO_NOT_USE_:
            YQL_ENSURE(false);
    }
}

bool NeedReportStats(const Ydb::Table::ExecuteScanQueryRequest& req) {
    switch (req.mode()) {
        case ExecuteScanQueryRequest_Mode_MODE_UNSPECIFIED:
            return false;

        case ExecuteScanQueryRequest_Mode_MODE_EXPLAIN:
            return true;

        case ExecuteScanQueryRequest_Mode_MODE_EXEC:
            switch (req.collect_stats()) {
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_BASIC:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE:
                    return true;

                default:
                    break;
            }

            return false;

        case ExecuteScanQueryRequest_Mode_ExecuteScanQueryRequest_Mode_INT_MIN_SENTINEL_DO_NOT_USE_:
        case ExecuteScanQueryRequest_Mode_ExecuteScanQueryRequest_Mode_INT_MAX_SENTINEL_DO_NOT_USE_:
            YQL_ENSURE(false);
    }
}

bool NeedReportPlan(const Ydb::Table::ExecuteScanQueryRequest& req) {
    switch (req.mode()) {
        case ExecuteScanQueryRequest_Mode_MODE_UNSPECIFIED:
            return false;

        case ExecuteScanQueryRequest_Mode_MODE_EXPLAIN:
            return true;

        case ExecuteScanQueryRequest_Mode_MODE_EXEC:
            switch (req.collect_stats()) {
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE:
                    return true;

                default:
                    break;
            }

            return false;

        case ExecuteScanQueryRequest_Mode_ExecuteScanQueryRequest_Mode_INT_MIN_SENTINEL_DO_NOT_USE_:
        case ExecuteScanQueryRequest_Mode_ExecuteScanQueryRequest_Mode_INT_MAX_SENTINEL_DO_NOT_USE_:
            YQL_ENSURE(false);
    }
}

bool FillKqpParameters(const ::google::protobuf::Map<TString, Ydb::TypedValue>& input,
    NKikimrMiniKQL::TParams& output, TParseRequestError& error)
{
    if (input.size() != 0) {
        try {
            ConvertYdbParamsToMiniKQLParams(input, output);
        } catch (const std::exception& ex) {
            auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Failed to parse query parameters.");
            issue.AddSubIssue(MakeIntrusive<NYql::TIssue>(NYql::ExceptionToIssue(ex)));

            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, {issue});
            return false;
        }
    }

    return true;
}

bool FillKqpRequest(const Ydb::Experimental::ExecuteStreamQueryRequest& req, NKikimrKqp::TEvQueryRequest& kqpRequest,
    TParseRequestError& error)
{
    if (!FillKqpParameters(req.parameters(), *kqpRequest.MutableRequest()->MutableParameters(), error)) {
        return false;
    }

    auto& query = req.yql_text();

    NYql::TIssues issues;
    if (!CheckQuery(query, issues)) {
        error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
        return false;
    }

    if (req.explain()) {
        kqpRequest.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_EXPLAIN);
    } else {
        kqpRequest.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
    }

    kqpRequest.MutableRequest()->SetType(NKikimrKqp::QUERY_TYPE_SQL_SCAN);
    kqpRequest.MutableRequest()->SetQuery(query);
    kqpRequest.MutableRequest()->SetKeepSession(false);
    switch (req.profile_mode()) {
        case Ydb::Experimental::ExecuteStreamQueryRequest_ProfileMode_PROFILE_MODE_UNSPECIFIED:
        case Ydb::Experimental::ExecuteStreamQueryRequest_ProfileMode_NONE:
            kqpRequest.MutableRequest()->SetCollectStats(Ydb::Table::QueryStatsCollection::STATS_COLLECTION_NONE);
            break;
        case Ydb::Experimental::ExecuteStreamQueryRequest_ProfileMode_BASIC:
            kqpRequest.MutableRequest()->SetCollectStats(Ydb::Table::QueryStatsCollection::STATS_COLLECTION_BASIC);
            break;
        case Ydb::Experimental::ExecuteStreamQueryRequest_ProfileMode_FULL:
            kqpRequest.MutableRequest()->SetCollectStats(Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL);
            break;
        case Ydb::Experimental::ExecuteStreamQueryRequest_ProfileMode_PROFILE:
            kqpRequest.MutableRequest()->SetCollectStats(Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE);
            break;
        default:
            YQL_ENSURE(false, "Unknown profile_mode "
                << Ydb::Experimental::ExecuteStreamQueryRequest_ProfileMode_Name(req.profile_mode()));
    }

    return true;
}

bool FillKqpRequest(const Ydb::Table::ExecuteScanQueryRequest& req, NKikimrKqp::TEvQueryRequest& kqpRequest,
    TParseRequestError& error)
{
    if (!FillKqpParameters(req.parameters(), *kqpRequest.MutableRequest()->MutableParameters(), error)) {
        return false;
    }

    switch (req.mode()) {
        case Ydb::Table::ExecuteScanQueryRequest::MODE_EXEC:
            kqpRequest.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
            kqpRequest.MutableRequest()->SetStatsMode(GetKqpStatsMode(req.collect_stats()));
            kqpRequest.MutableRequest()->SetCollectStats(req.collect_stats());
            break;
        case Ydb::Table::ExecuteScanQueryRequest::MODE_EXPLAIN:
            kqpRequest.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_EXPLAIN);
            break;
        default: {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Unexpected query mode"));
            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
            return false;
        }
    }
    kqpRequest.MutableRequest()->SetType(NKikimrKqp::QUERY_TYPE_SQL_SCAN);
    kqpRequest.MutableRequest()->SetKeepSession(false);

    auto& query = req.query();
    switch (query.query_case()) {
        case Query::kYqlText: {
            NYql::TIssues issues;
            if (!CheckQuery(query.yql_text(), issues)) {
                error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
                return false;
            }

            kqpRequest.MutableRequest()->SetQuery(query.yql_text());
            break;
        }

        case Query::kId: {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR,
                "Specifying query by ID not supported in scan execution."));
            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
            return false;
        }

        default: {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Unexpected query option"));
            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
            return false;
        }
    }

    return true;
}

bool FillProfile(Ydb::Experimental::ExecuteStreamQueryResponse& response,
    const NYql::NDqProto::TDqExecutionStats& profile)
{
    response.set_status(Ydb::StatusIds::SUCCESS);
    response.mutable_result()->set_profile(profile.Utf8DebugString());
    return true;
}

bool FillProfile(Ydb::Table::ExecuteScanQueryPartialResponse& response,
    const NYql::NDqProto::TDqExecutionStats& profile)
{
    Y_UNUSED(response);
    Y_UNUSED(profile);
    return false;
}

using TEvStreamExecuteScanQueryRequest = TGrpcRequestNoOperationCall<Ydb::Table::ExecuteScanQueryRequest,
    Ydb::Table::ExecuteScanQueryPartialResponse>;

template<typename TRequestEv, typename TResponse>
class TStreamExecuteScanQueryRPC : public TActorBootstrapped<TStreamExecuteScanQueryRPC<TRequestEv, TResponse>> {
private:
    enum EWakeupTag : ui64 {
        ClientLostTag = 1,
        TimeoutTag = 2
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::GRPC_STREAM_REQ;
    }

    TStreamExecuteScanQueryRPC(TRequestEv* request, ui64 rpcBufferSize)
        : Request_(request)
        , RpcBufferSize_(rpcBufferSize) {}

    void Bootstrap(const TActorContext &ctx) {
        this->Become(&TStreamExecuteScanQueryRPC::StateWork);

        const auto& cfg = AppData(ctx)->StreamingConfig.GetOutputStreamConfig();

        InactiveClientTimeout_ = TDuration::FromValue(cfg.GetInactiveClientTimeout());
        if (InactiveClientTimeout_) {
            SetTimeoutTimer(InactiveClientTimeout_, ctx);
        }

        LastDataStreamTimestamp_ = TAppData::TimeProvider->Now();

        auto selfId = this->SelfId();
        auto as = TActivationContext::ActorSystem();

        Request_->SetClientLostAction([selfId, as]() {
            as->Send(selfId, new TEvents::TEvWakeup(EWakeupTag::ClientLostTag));
        });

        Request_->SetStreamingNotify([selfId, as](size_t left) {
            as->Send(selfId, new TRpcServices::TEvGrpcNextReply(left));
        });

        Proceed(ctx);
    }

private:
    void StateWork(TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvWakeup, Handle);
            HFunc(TRpcServices::TEvGrpcNextReply, Handle);
            HFunc(NKqp::TEvKqp::TEvQueryResponse, Handle);
            HFunc(NKqp::TEvKqp::TEvProcessResponse, Handle);
            HFunc(NKqp::TEvKqp::TEvAbortExecution, Handle);
            HFunc(NKqp::TEvKqpExecuter::TEvStreamData, Handle);
            HFunc(NKqp::TEvKqpExecuter::TEvStreamProfile, Handle);
            HFunc(NKqp::TEvKqpExecuter::TEvExecuterProgress, Handle);
            default: {
                auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, TStringBuilder()
                    << "Unexpected event received in TStreamExecuteScanQueryRPC::StateWork: " << ev->GetTypeRewrite());
                return ReplyFinishStream(Ydb::StatusIds::INTERNAL_ERROR, issue, ctx);
            }
        }
    }

    void Proceed(const TActorContext &ctx) {
        const auto req = Request_->GetProtoRequest();
        const auto traceId = Request_->GetTraceId();

        auto ev = MakeHolder<NKqp::TEvKqp::TEvQueryRequest>();
        SetAuthToken(ev, *Request_);
        SetDatabase(ev, *Request_);
        SetRlPath(ev, *Request_);

        if (traceId) {
            ev->Record.SetTraceId(traceId.GetRef());
        }

        ActorIdToProto(this->SelfId(), ev->Record.MutableRequestActorId());

        TParseRequestError parseError;
        if (!FillKqpRequest(*req, ev->Record, parseError)) {
            return ReplyFinishStream(parseError.Status, parseError.Issues, ctx);
        }

        if (!ctx.Send(NKqp::MakeKqpProxyID(ctx.SelfID.NodeId()), ev.Release())) {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Internal error"));
            ReplyFinishStream(Ydb::StatusIds::INTERNAL_ERROR, issues, ctx);
        }
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev, const TActorContext& ctx) {
        switch ((EWakeupTag) ev->Get()->Tag) {
            case EWakeupTag::ClientLostTag:
                return HandleClientLost(ctx);
            case EWakeupTag::TimeoutTag:
                return HandleTimeout(ctx);
        }
    }

    void Handle(TRpcServices::TEvGrpcNextReply::TPtr& ev, const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " NextReply"
            << ", left: " << ev->Get()->LeftInQueue
            << ", queue: " << GRpcResponsesSizeQueue_.size()
            << ", used memory: " << GRpcResponsesSize_
            << ", buffer size: " << RpcBufferSize_);

        while (GRpcResponsesSizeQueue_.size() > ev->Get()->LeftInQueue) {
            GRpcResponsesSize_ -= GRpcResponsesSizeQueue_.front();
            GRpcResponsesSizeQueue_.pop();
        }
        Y_VERIFY_DEBUG(GRpcResponsesSizeQueue_.empty() == (GRpcResponsesSize_ == 0));
        LastDataStreamTimestamp_ = TAppData::TimeProvider->Now();

        if (WaitOnSeqNo_ && RpcBufferSize_ > GRpcResponsesSize_) {
            ui64 freeSpace = RpcBufferSize_ - GRpcResponsesSize_;

            LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Send stream data ack"
                << ", seqNo: " << *WaitOnSeqNo_
                << ", freeSpace: " << freeSpace
                << ", to: " << ExecuterActorId_);

            auto resp = MakeHolder<NKqp::TEvKqpExecuter::TEvStreamDataAck>();
            resp->Record.SetSeqNo(*WaitOnSeqNo_);
            resp->Record.SetFreeSpace(freeSpace);

            ctx.Send(ExecuterActorId_, resp.Release());

            WaitOnSeqNo_.Clear();
        }
    }

    void Handle(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) {
        auto& record = ev->Get()->Record.GetRef();

        NYql::TIssues issues;
        const auto& issueMessage = record.GetResponse().GetQueryIssues();
        NYql::IssuesFromMessage(issueMessage, issues);

        if (record.GetYdbStatus() == Ydb::StatusIds::SUCCESS) {
            TResponse response;
            TString out;
            auto& kqpResponse = record.GetResponse();
            response.set_status(Ydb::StatusIds::SUCCESS);

            if constexpr (std::is_same_v<TResponse, Ydb::Table::ExecuteScanQueryPartialResponse>) {
                bool reportStats = NeedReportStats(*Request_->GetProtoRequest());
                bool reportPlan = reportStats && NeedReportPlan(*Request_->GetProtoRequest());

                if (reportStats) {
                    if (kqpResponse.HasQueryStats()) {
                        for (const auto& execStats: ExecutionProfiles_) {
                            record.MutableResponse()->MutableQueryStats()->AddExecutions()->Swap(execStats.get());
                        }

                        record.MutableResponse()->SetQueryPlan(reportPlan
                            ? SerializeAnalyzePlan(kqpResponse.GetQueryStats())
                            : "");

                        FillQueryStats(*response.mutable_result()->mutable_query_stats(), kqpResponse);
                        ExecutionProfiles_.clear();
                    } else if (reportPlan) {
                        response.mutable_result()->mutable_query_stats()->set_query_plan(kqpResponse.GetQueryPlan());
                    }

                    if (reportPlan) {
                        response.mutable_result()->mutable_query_stats()->set_query_ast(kqpResponse.GetQueryAst());
                    }

                    Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);
                    Request_->SendSerializedResult(std::move(out), record.GetYdbStatus());
                }
            } else {
                if (kqpResponse.HasQueryStats()) {
                    NKqpProto::TKqpStatsQuery queryStats;
                    for (const auto& execStats: ExecutionProfiles_) {
                        /* copy as ExecutionProfiles_ vector will be used and cleared later */
                        queryStats.AddExecutions()->CopyFrom(*execStats);
                    }
                    response.mutable_result()->set_query_plan(SerializeAnalyzePlan(queryStats));
                } else {
                    response.mutable_result()->set_query_plan(kqpResponse.GetQueryPlan());
                }

                Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);
                Request_->SendSerializedResult(std::move(out), record.GetYdbStatus());
            }
        }

        ReplyFinishStream(record.GetYdbStatus(), issues, ctx);
    }

    void Handle(NKqp::TEvKqp::TEvProcessResponse::TPtr& ev, const TActorContext& ctx) {
        const auto& kqpResponse = ev->Get()->Record;
        NYql::TIssues issues;
        if (kqpResponse.HasError()) {
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, kqpResponse.GetError()));
        }

        ReplyFinishStream(kqpResponse.GetYdbStatus(), issues, ctx);
    }

    void Handle(NKqp::TEvKqp::TEvAbortExecution::TPtr& ev, const TActorContext& ctx) {
        auto& record = ev->Get()->Record;
        NYql::TIssues issues = ev->Get()->GetIssues();

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Got abort execution event, from: "
            << ev->Sender << ", code: " << NYql::NDqProto::StatusIds::StatusCode_Name(record.GetStatusCode())
            << ", message: " << issues.ToOneLineString());

        ReplyFinishStream(NYql::NDq::DqStatusToYdbStatus(record.GetStatusCode()), issues, ctx);
    }

    void Handle(NKqp::TEvKqpExecuter::TEvStreamData::TPtr& ev, const TActorContext& ctx) {
        TResponse response;
        response.set_status(StatusIds::SUCCESS);
        response.mutable_result()->mutable_result_set()->Swap(ev->Get()->Record.MutableResultSet());

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);

        GRpcResponsesSizeQueue_.push(out.size());
        GRpcResponsesSize_ += out.size();

        Request_->SendSerializedResult(std::move(out), StatusIds::SUCCESS);

        ui64 freeSpace = GRpcResponsesSize_ < RpcBufferSize_
            ? RpcBufferSize_ - GRpcResponsesSize_
            : 0;

        if (freeSpace == 0) {
            WaitOnSeqNo_ = ev->Get()->Record.GetSeqNo();
        }

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Send stream data ack"
            << ", seqNo: " << ev->Get()->Record.GetSeqNo()
            << ", freeSpace: " << freeSpace
            << ", to: " << ev->Sender
            << ", queue: " << GRpcResponsesSizeQueue_.size());

        auto resp = MakeHolder<NKqp::TEvKqpExecuter::TEvStreamDataAck>();
        resp->Record.SetSeqNo(ev->Get()->Record.GetSeqNo());
        resp->Record.SetFreeSpace(freeSpace);

        ctx.Send(ev->Sender, resp.Release());
    }

    void Handle(NKqp::TEvKqpExecuter::TEvStreamProfile::TPtr& ev, const TActorContext&) {
        auto req = Request_->GetProtoRequest();
        if (!NeedReportStats(*req)) {
            return;
        }

        // every TKqpExecuter sends its own profile
        auto profile = std::make_unique<NYql::NDqProto::TDqExecutionStats>();
        profile->Swap(ev->Get()->Record.MutableProfile());
        ExecutionProfiles_.emplace_back(std::move(profile));
    }

    void Handle(NKqp::TEvKqpExecuter::TEvExecuterProgress::TPtr& ev, const TActorContext& ctx) {
        ExecuterActorId_ = ActorIdFromProto(ev->Get()->Record.GetExecuterActorId());
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " ExecuterActorId: " << ExecuterActorId_);
    }

private:
    void SetTimeoutTimer(TDuration timeout, const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Set stream timeout timer for " << timeout);

        auto *ev = new IEventHandle(this->SelfId(), this->SelfId(), new TEvents::TEvWakeup(EWakeupTag::TimeoutTag));
        TimeoutTimerCookieHolder_.Reset(ISchedulerCookie::Make2Way());
        CreateLongTimer(ctx, timeout, ev, 0, TimeoutTimerCookieHolder_.Get());
    }

    void HandleClientLost(const TActorContext& ctx) {
        LOG_WARN_S(ctx, NKikimrServices::RPC_REQUEST, "Client lost, send abort event to executer " << ExecuterActorId_);

        if (ExecuterActorId_) {
            auto abortEv = TEvKqp::TEvAbortExecution::Aborted("Client lost"); // any status code can be here

            ctx.Send(ExecuterActorId_, abortEv.Release());
        }

        // We must try to finish stream otherwise grpc will not free allocated memory
        // If stream already scheduled to be finished (ReplyFinishStream already called)
        // this call do nothing but Die will be called after reply to grpc
        auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR,
            "Client should not see this message, if so... may the force be with you");
        ReplyFinishStream(StatusIds::INTERNAL_ERROR, issue, ctx);
    }

    void HandleTimeout(const TActorContext& ctx) {
        TInstant now = TAppData::TimeProvider->Now();
        TDuration timeout;

        if (InactiveClientTimeout_ && GRpcResponsesSizeQueue_.size() > 0) {
            TDuration processTime = now - LastDataStreamTimestamp_;
            if (processTime >= InactiveClientTimeout_) {
                auto message = TStringBuilder() << this->SelfId() << " Client cannot process data in " << processTime
                   << " which exceeds client timeout " << InactiveClientTimeout_;
                LOG_WARN_S(ctx, NKikimrServices::RPC_REQUEST, message);

                if (ExecuterActorId_) {
                    auto timeoutEv = MakeHolder<TEvKqp::TEvAbortExecution>(NYql::NDqProto::StatusIds::TIMEOUT, "Client timeout");

                    ctx.Send(ExecuterActorId_, timeoutEv.Release());
                }

                auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, message);
                return ReplyFinishStream(StatusIds::TIMEOUT, issue, ctx);
            }
            TDuration remain = InactiveClientTimeout_ - processTime;
            timeout = timeout ? Min(timeout, remain) : remain;
        }

        if (timeout) {
            SetTimeoutTimer(timeout, ctx);
        }
    }

    void ReplyFinishStream(Ydb::StatusIds::StatusCode status, const NYql::TIssue& issue, const TActorContext& ctx) {
        google::protobuf::RepeatedPtrField<TYdbIssueMessageType> issuesMessage;
        NYql::IssueToMessage(issue, issuesMessage.Add());

        ReplyFinishStream(status, issuesMessage, ctx);
    }

    void ReplyFinishStream(Ydb::StatusIds::StatusCode status, const NYql::TIssues& issues, const TActorContext& ctx) {
        google::protobuf::RepeatedPtrField<TYdbIssueMessageType> issuesMessage;
        for (auto& issue : issues) {
            auto item = issuesMessage.Add();
            NYql::IssueToMessage(issue, item);
        }

        ReplyFinishStream(status, issuesMessage, ctx);
    }

    void ReplyFinishStream(Ydb::StatusIds::StatusCode status,
        const google::protobuf::RepeatedPtrField<TYdbIssueMessageType>& message, const TActorContext& ctx)
    {
        LOG_INFO_S(ctx, NKikimrServices::RPC_REQUEST, "Finish grpc stream, status: "
            << Ydb::StatusIds::StatusCode_Name(status));

        // Skip sending empty result in case of success status - simplify client logic
        if (status != Ydb::StatusIds::SUCCESS) {
            TString out;
            TResponse response;
            response.set_status(status);
            response.mutable_issues()->CopyFrom(message);
            Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);
            Request_->SendSerializedResult(std::move(out), status);
        } else {
            for (auto& profile : ExecutionProfiles_) {
                TResponse response;
                if (!FillProfile(response, *profile)) {
                    break;
                }

                TString out;
                Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);
                Request_->SendSerializedResult(std::move(out), status);
            }
        }

        Request_->FinishStream();
        this->PassAway();
    }

private:
    std::unique_ptr<TRequestEv> Request_;
    const ui64 RpcBufferSize_;

    TDuration InactiveClientTimeout_;
    TQueue<ui64> GRpcResponsesSizeQueue_;
    ui64 GRpcResponsesSize_ = 0;
    TInstant LastDataStreamTimestamp_;
    TMaybe<ui64> WaitOnSeqNo_;

    TSchedulerCookieHolder TimeoutTimerCookieHolder_;

    TVector<std::unique_ptr<NYql::NDqProto::TDqExecutionStats>> ExecutionProfiles_;
    TActorId ExecuterActorId_;
};

} // namespace

void TGRpcRequestProxy::Handle(TEvExperimentalStreamQueryRequest::TPtr& ev, const TActorContext& ctx) {
    ui64 rpcBufferSize = GetAppConfig().GetTableServiceConfig().GetResourceManager().GetChannelBufferSize();
    ctx.Register(new TStreamExecuteScanQueryRPC<TEvExperimentalStreamQueryRequest,
        Ydb::Experimental::ExecuteStreamQueryResponse>(ev->Release().Release(), rpcBufferSize));
}

void DoExecuteScanQueryRequest(std::unique_ptr<IRequestNoOpCtx> p, const IFacilityProvider& f) {
    ui64 rpcBufferSize = f.GetAppConfig().GetTableServiceConfig().GetResourceManager().GetChannelBufferSize();
    auto* req = dynamic_cast<TEvStreamExecuteScanQueryRequest*>(p.release());
    Y_VERIFY(req != nullptr, "Wrong using of TGRpcRequestWrapper");
    TActivationContext::AsActorContext().Register(new TStreamExecuteScanQueryRPC<TEvStreamExecuteScanQueryRequest,
        Ydb::Table::ExecuteScanQueryPartialResponse>(req, rpcBufferSize));
}

} // namespace NGRpcService
} // namespace NKikimr
