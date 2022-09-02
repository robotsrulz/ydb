#include "control_plane_storage_counters.h"

namespace NYq {

TRequestCounters::TRequestCounters(const TString& name)
    : Name(name) {}

void TRequestCounters::Register(const ::NMonitoring::TDynamicCounterPtr& counters) {
    auto requestCounters = counters->GetSubgroup("request", Name);
    InFly = requestCounters->GetCounter("InFly", false);
    Ok = requestCounters->GetCounter("Ok", true);
    Error = requestCounters->GetCounter("Error", true);
    Retry = requestCounters->GetCounter("Retry", true);
    RequestBytes = requestCounters->GetCounter("RequestBytes", true);
    ResponseBytes = requestCounters->GetCounter("ResponseBytes", true);
    LatencyMs = requestCounters->GetHistogram("LatencyMs", GetLatencyHistogramBuckets());
    Issues = requestCounters->GetSubgroup("subcomponent", "Issues");
}

NMonitoring::IHistogramCollectorPtr TRequestCounters::GetLatencyHistogramBuckets() {
    return NMonitoring::ExplicitHistogram({0, 1, 2, 5, 10, 20, 50, 100, 500, 1000, 2000, 5000, 10000, 30000, 50000, 500000});
}


TFinalStatusCounters::TFinalStatusCounters(const ::NMonitoring::TDynamicCounterPtr& counters) {
    auto subgroup = counters->GetSubgroup("subcomponent", "FinalStatus");
    Completed = subgroup->GetCounter("COMPLETED", true);
    AbortedBySystem = subgroup->GetCounter("ABORTED_BY_SYSTEM", true);
    AbortedByUser = subgroup->GetCounter("ABORTED_BY_USER", true);
    Failed = subgroup->GetCounter("FAILED", true);
    Paused = subgroup->GetCounter("PAUSED", true);
}

void TFinalStatusCounters::IncByStatus(YandexQuery::QueryMeta::ComputeStatus finalStatus) {
    switch (finalStatus) {
    case YandexQuery::QueryMeta::COMPLETED:
        Completed->Inc();
        break;
    case YandexQuery::QueryMeta::FAILED:
        Failed->Inc();
        break;
    case YandexQuery::QueryMeta::ABORTED_BY_SYSTEM:
        AbortedBySystem->Inc();
        break;
    case YandexQuery::QueryMeta::ABORTED_BY_USER:
        AbortedByUser->Inc();
        break;
    case YandexQuery::QueryMeta::PAUSED:
        Paused->Inc();
        break;
    case YandexQuery::QueryMeta::COMPUTE_STATUS_UNSPECIFIED:
    case YandexQuery::QueryMeta::STARTING:
    case YandexQuery::QueryMeta::ABORTING_BY_USER:
    case YandexQuery::QueryMeta::ABORTING_BY_SYSTEM:
    case YandexQuery::QueryMeta::RESUMING:
    case YandexQuery::QueryMeta::RUNNING:
    case YandexQuery::QueryMeta::COMPLETING:
    case YandexQuery::QueryMeta::FAILING:
    case YandexQuery::QueryMeta::PAUSING:
        break;
    default:
        Y_ENSURE(true, "Unexpected status: " << YandexQuery::QueryMeta_ComputeStatus_Name(finalStatus));
    }
}

} // NYq
