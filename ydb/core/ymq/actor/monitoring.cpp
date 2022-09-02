#include "monitoring.h"

#include <ydb/core/ymq/actor/cfg.h>
#include <ydb/core/ymq/base/run_query.h>


namespace NKikimr::NSQS {
    
    constexpr TDuration RETRY_PERIOD_MIN = TDuration::Seconds(30);
    constexpr TDuration RETRY_PERIOD_MAX = TDuration::Minutes(5);
    

    TMonitoringActor::TMonitoringActor(TIntrusivePtr<TMonitoringCounters> counters)
        : Counters(counters)
        , RetryPeriod(RETRY_PERIOD_MIN)
    {}

    void TMonitoringActor::Bootstrap(const TActorContext& ctx) {
        Become(&TMonitoringActor::StateFunc);

        TString removedQueuesTable = Cfg().GetRoot() + "/.RemovedQueues";
        RemovedQueuesQuery = TStringBuilder() << R"__(
            --!syntax_v1
            SELECT RemoveTimestamp FROM `)__" << removedQueuesTable <<  R"__(` ORDER BY RemoveTimestamp LIMIT 1000;
        )__";

        RequestMetrics(TDuration::Zero(), ctx);
    }

    void TMonitoringActor::HandleError(const TString& error, const TActorContext& ctx) {
        auto runAfter = RetryPeriod;
        RetryPeriod = Min(RetryPeriod * 2, RETRY_PERIOD_MAX);
        LOG_ERROR_S(ctx, NKikimrServices::SQS, "[monitoring] Got an error : " << error);
        RequestMetrics(runAfter, ctx);
    }
    
    void TMonitoringActor::RequestMetrics(TDuration runAfter, const TActorContext& ctx) {
        RunYqlQuery(RemovedQueuesQuery, std::nullopt, true, runAfter, Cfg().GetRoot(), ctx);
    }
    
    void TMonitoringActor::HandleQueryResponse(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record.GetRef();
        if (record.GetYdbStatus() != Ydb::StatusIds::SUCCESS) {
            HandleError(record.DebugString(), ctx);
            return;
        }
        RetryPeriod = RETRY_PERIOD_MIN;
        auto& response = record.GetResponse();

        Y_VERIFY(response.GetResults().size() == 1);
        const auto& rr = response.GetResults(0).GetValue().GetStruct(0);
        TDuration removeQueuesDataLag;
        
        if (!rr.GetList().empty()) {
            TInstant minRemoveQueueTimestamp = TInstant::MilliSeconds(rr.GetList()[0].GetStruct(0).GetOptional().GetUint64());
            removeQueuesDataLag = ctx.Now() - minRemoveQueueTimestamp;
        }
        
        LOG_DEBUG_S(ctx, NKikimrServices::SQS, "[monitoring] Report deletion queue data lag: " << removeQueuesDataLag << ", count: " << rr.GetList().size());
        *Counters->CleanupRemovedQueuesLagSec = removeQueuesDataLag.Seconds();
        *Counters->CleanupRemovedQueuesLagCount = rr.GetList().size();
        RequestMetrics(RetryPeriod, ctx);
    }

    void TMonitoringActor::HandleProcessResponse(NKqp::TEvKqp::TEvProcessResponse::TPtr& ev, const TActorContext& ctx) {
        HandleError(ev->Get()->Record.DebugString(), ctx);
    }

} // namespace NKikimr::NSQS
