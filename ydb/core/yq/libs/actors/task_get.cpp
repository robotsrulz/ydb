#include <ydb/core/yq/libs/config/protos/yq_config.pb.h>
#include "proxy_private.h"
#include "proxy.h"

#include <ydb/core/protos/services.pb.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>

#include <library/cpp/yson/node/node_io.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/log.h>

#include <ydb/core/yq/libs/common/entity_id.h>

#include <ydb/core/yq/libs/control_plane_storage/control_plane_storage.h>
#include <ydb/core/yq/libs/control_plane_storage/events/events.h>
#include <ydb/library/security/util.h>

#define LOG_E(stream) \
    LOG_ERROR_S(*TlsActivationContext, NKikimrServices::YQL_PRIVATE_PROXY, "PrivateGetTask - Owner: " << OwnerId << ", " << "Host: " << Host << ", Tenant: " << Tenant << ", " << stream)
#define LOG_D(stream) \
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::YQL_PRIVATE_PROXY, "PrivateGetTask - Owner: " << OwnerId << ", " << "Host: " << Host << ", Tenant: " << Tenant << ", " << stream)

namespace NYq {

using namespace NActors;
using namespace NMonitoring;

class TGetTaskRequestActor
    : public NActors::TActorBootstrapped<TGetTaskRequestActor>
{
public:
    TGetTaskRequestActor(
        const NActors::TActorId& sender,
        const NConfig::TTokenAccessorConfig& tokenAccessorConfig,
        TIntrusivePtr<ITimeProvider> timeProvider,
        TAutoPtr<TEvents::TEvGetTaskRequest> ev,
        TDynamicCounterPtr counters)
        : TokenAccessorConfig(tokenAccessorConfig)
        , Sender(sender)
        , TimeProvider(timeProvider)
        , Ev(std::move(ev))
        , Counters(std::move(counters->GetSubgroup("subsystem", "private_api")->GetSubgroup("subcomponent", "GetTask")))
        , LifetimeDuration(Counters->GetHistogram("LifetimeDurationMs",  ExponentialHistogram(10, 2, 50)))
        , RequestedMBytes(Counters->GetHistogram("RequestedMB",  ExponentialHistogram(6, 2, 3)))
        , StartTime(TInstant::Now())
    {
        if (TokenAccessorConfig.GetHmacSecretFile()) {
            Signer = ::NYq::CreateSignerFromFile(TokenAccessorConfig.GetHmacSecretFile());
        }
    }

    static constexpr char ActorName[] = "YQ_PRIVATE_GET_TASK";

    void OnUndelivered(NActors::TEvents::TEvUndelivered::TPtr& ev, const NActors::TActorContext& ctx) {
        LOG_E("TGetTaskRequestActor::OnUndelivered");
        auto response = MakeHolder<TEvents::TEvGetTaskResponse>();
        response->Status = Ydb::StatusIds::GENERIC_ERROR;
        response->Issues.AddIssue("UNDELIVERED");
        ctx.Send(ev->Sender, response.Release());
        Die(ctx);
    }

    void PassAway() final {
        LifetimeDuration->Collect((TInstant::Now() - StartTime).MilliSeconds());
        NActors::IActor::PassAway();
    }

    void Fail(const TString& message, Ydb::StatusIds::StatusCode reqStatus = Ydb::StatusIds::INTERNAL_ERROR) {
        Issues.AddIssue(message);
        const auto codeStr = Ydb::StatusIds_StatusCode_Name(reqStatus);
        LOG_E(TStringBuilder()
            << "Failed with code: " << codeStr
            << " Details: " << Issues.ToString());
        auto response = MakeHolder<TEvents::TEvGetTaskResponse>();
        response->Status = reqStatus;
        response->Issues.AddIssues(Issues);
        Send(Sender, response.Release());
        PassAway();
    }

    void Bootstrap(const TActorContext& ctx) {
        Become(&TGetTaskRequestActor::StateFunc);
        auto request = Ev->Record;
        LOG_D("Request CP::GetTask with size: " << request.ByteSize() << " bytes");
        RequestedMBytes->Collect(request.ByteSize() / 1024 / 1024);
        OwnerId = request.owner_id();
        Host = request.host();
        Tenant = request.tenant();
        ctx.Send(NYq::ControlPlaneStorageServiceActorId(),
            new NYq::TEvControlPlaneStorage::TEvGetTaskRequest(std::move(request)));
    }

private:
    void HandleResponse(NYq::TEvControlPlaneStorage::TEvGetTaskResponse::TPtr& ev, const TActorContext& ctx) { // YQ
        LOG_D("Got CP::GetTask Response");

        const auto& issues = ev->Get()->Issues;
        if (issues) {
            Issues.AddIssues(issues);
            Fail("ControlPlane::GetTaskError", Ydb::StatusIds::GENERIC_ERROR);
            return;
        }

        auto response = MakeHolder<TEvents::TEvGetTaskResponse>();
        response->Status = Ydb::StatusIds::SUCCESS;
        response->Record.ConstructInPlace();
        auto& record = *response->Record;
        record = ev->Get()->Record;
        try {
            for (auto& task : *record.mutable_tasks()) {
                THashMap<TString, TString> accountIdSignatures;
                for (auto& account : *task.mutable_service_accounts()) {
                    const auto serviceAccountId = account.value();
                    auto& signature = accountIdSignatures[serviceAccountId];
                    if (!signature && Signer) {
                        signature = Signer->SignAccountId(serviceAccountId);
                    }
                    account.set_signature(signature);
                }
            }
            ctx.Send(Sender, response.Release());
            Die(ctx);
        } catch (...) {
            const auto msg = TStringBuilder() << "Can't do GetTask: " << CurrentExceptionMessage();
            Fail(msg);
        }
    }

private:
    STRICT_STFUNC(
        StateFunc,
        CFunc(NActors::TEvents::TEvPoison::EventType, Die)
        HFunc(NActors::TEvents::TEvUndelivered, OnUndelivered)
        HFunc(NYq::TEvControlPlaneStorage::TEvGetTaskResponse, HandleResponse)
    )

    const NConfig::TTokenAccessorConfig TokenAccessorConfig;
    const TActorId Sender;
    TIntrusivePtr<ITimeProvider> TimeProvider;
    TAutoPtr<TEvents::TEvGetTaskRequest> Ev;
    TDynamicCounterPtr Counters;
    const THistogramPtr LifetimeDuration;
    const THistogramPtr RequestedMBytes;
    const TInstant StartTime;

    ::NYq::TSigner::TPtr Signer;

    NYql::TIssues Issues;
    TString OwnerId;
    TString Host;
    TString Tenant;
};

IActor* CreateGetTaskRequestActor(
    const NActors::TActorId& sender,
    const NConfig::TTokenAccessorConfig& tokenAccessorConfig,
    TIntrusivePtr<ITimeProvider> timeProvider,
    TAutoPtr<TEvents::TEvGetTaskRequest> ev,
    TDynamicCounterPtr counters) {
    return new TGetTaskRequestActor(
        sender,
        tokenAccessorConfig,
        timeProvider,
        std::move(ev),
        counters);
}

} /* NYq */
