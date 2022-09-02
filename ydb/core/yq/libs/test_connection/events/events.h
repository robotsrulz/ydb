#pragma once

#include <ydb/public/api/protos/yq.pb.h>
#include <ydb/core/yq/libs/events/event_subspace.h>

#include <ydb/core/yq/libs/control_plane_storage/events/events.h>

#include <library/cpp/actors/core/event_pb.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/interconnect/events_local.h>

#include <ydb/library/yql/public/issue/yql_issue.h>

namespace NYq {

struct TEvTestConnection {
    // Event ids.
    enum EEv : ui32 {
        EvTestConnectionRequest = YqEventSubspaceBegin(NYq::TYqEventSubspace::TestConnection),
        EvTestConnectionResponse,
        EvEnd,
    };

    static_assert(EvEnd <= YqEventSubspaceEnd(NYq::TYqEventSubspace::TestConnection), "All events must be in their subspace");

    struct TEvTestConnectionRequest : NActors::TEventLocal<TEvTestConnectionRequest, EvTestConnectionRequest> {
        explicit TEvTestConnectionRequest(const TString& scope,
                                          const YandexQuery::TestConnectionRequest& request,
                                          const TString& user,
                                          const TString& token,
                                          const TString& cloudId,
                                          const TPermissions& permissions,
                                          const TQuotaMap& quotas)
            : CloudId(cloudId)
            , Scope(scope)
            , Request(request)
            , User(user)
            , Token(token)
            , Permissions(permissions)
            , Quotas(quotas)
        {
        }

        TString CloudId;
        TString Scope;
        YandexQuery::TestConnectionRequest Request;
        TString User;
        TString Token;
        TPermissions Permissions;
        const TQuotaMap Quotas;
    };

    struct TEvTestConnectionResponse : NActors::TEventLocal<TEvTestConnectionResponse, EvTestConnectionResponse> {
        explicit TEvTestConnectionResponse(const YandexQuery::TestConnectionResult& result)
            : Result(result)
        {
        }

        explicit TEvTestConnectionResponse(const NYql::TIssues& issues)
            : Issues(issues)
        {
        }

        YandexQuery::TestConnectionResult Result;
        NYql::TIssues Issues;
    };
};

}
