#pragma once
#include <library/cpp/actors/core/event_local.h>
#include <library/cpp/actors/core/events.h>

namespace NYq {

struct TEventIds {
    enum EEventSpaceYqlProxy {
        ES_YQL_ANALYTICS_PROXY = 4205 //TKikimrEvents::ES_YQL_ANALYTICS_PROXY
    };

    static constexpr ui32 EventSpace = ES_YQL_ANALYTICS_PROXY;

    // Event ids.
    enum EEv : ui32 {
        EvBegin = EventSpaceBegin(EventSpace),

        //YQL Internal
        EvPingTaskRequest = EvBegin,
        EvPingTaskResponse,
        EvGetTaskRequest,
        EvGetTaskResponse,
        EvWriteTaskResultRequest,
        EvWriteTaskResultResponse,

        EvNodesHealthCheckRequest,
        EvNodesHealthCheckResponse,

        EvUpdateConfig,

        // Internal events
        EvAsyncContinue,
        EvDbRequest,
        EvDbResponse,
        EvDbFunctionRequest,
        EvDbFunctionResponse,
        EvEndpointRequest,
        EvEndpointResponse,
        EvDataStreamsReadRulesCreationResult,
        EvDataStreamsReadRulesDeletionResult,
        EvQueryActionResult,
        EvForwardPingRequest,
        EvForwardPingResponse,
        EvGraphParams,
        EvRaiseTransientIssues,
        EvSchemaCreated,
        EvCallback,

        EvCreateRateLimiterResourceRequest,
        EvCreateRateLimiterResourceResponse,
        EvDeleteRateLimiterResourceRequest,
        EvDeleteRateLimiterResourceResponse,

        EvSchemaDeleted,

        // Special events
        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(EventSpace), "expect EvEnd < EventSpaceEnd(EventSpace)");

};

} // namespace NYq
