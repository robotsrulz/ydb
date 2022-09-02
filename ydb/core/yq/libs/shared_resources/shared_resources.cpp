#include "shared_resources.h"

#include <ydb/core/protos/services.pb.h>
#include <ydb/core/yq/libs/events/events.h>
#include <ydb/library/logger/actor.h>

#include <library/cpp/actors/core/actorsystem.h>

#include <util/generic/cast.h>
#include <util/generic/strbuf.h>
#include <util/stream/file.h>
#include <util/stream/output.h>
#include <util/string/strip.h>
#include <util/system/compiler.h>
#include <util/system/spinlock.h>

#include <atomic>
#include <memory>

namespace NYq {

namespace {

struct TActorSystemPtrMixin {
    NKikimr::TDeferredActorLogBackend::TSharedAtomicActorSystemPtr ActorSystemPtr = std::make_shared<NKikimr::TDeferredActorLogBackend::TAtomicActorSystemPtr>(nullptr);
};

struct TYqSharedResourcesImpl : public TActorSystemPtrMixin, public TYqSharedResources {
    explicit TYqSharedResourcesImpl(
        const NYq::NConfig::TConfig& config,
        const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
        const ::NMonitoring::TDynamicCounterPtr& counters)
        : TYqSharedResources(NYdb::TDriver(GetYdbDriverConfig(config.GetCommon().GetYdbDriverConfig())))
    {
        CreateDbPoolHolder(config.GetDbPool(), credentialsProviderFactory, counters);
    }

    void Init(NActors::TActorSystem* actorSystem) override {
        Y_VERIFY(!ActorSystemPtr->load(std::memory_order_relaxed), "Double IYqSharedResources init");
        ActorSystemPtr->store(actorSystem, std::memory_order_relaxed);
    }

    void Stop() override {
        CoreYdbDriver.Stop(true);
        // UserSpaceYdbDriver.Stop(true); // For now it points to the same driver as CoreYdbDriver, so don't call Stop
    }

    NYdb::TDriverConfig GetYdbDriverConfig(const NYq::NConfig::TYdbDriverConfig& config) {
        NYdb::TDriverConfig cfg;
        if (config.GetNetworkThreadsNum()) {
            cfg.SetNetworkThreadsNum(config.GetNetworkThreadsNum());
        }
        if (config.GetClientThreadsNum()) {
            cfg.SetClientThreadsNum(config.GetClientThreadsNum());
        }
        if (config.GetGrpcMemoryQuota()) {
            cfg.SetGrpcMemoryQuota(config.GetGrpcMemoryQuota());
        }
        cfg.SetDiscoveryMode(NYdb::EDiscoveryMode::Async); // We are in actor system!
        cfg.SetLog(MakeHolder<NKikimr::TDeferredActorLogBackend>(ActorSystemPtr, NKikimrServices::EServiceKikimr::YDB_SDK));
        return cfg;
    }

    void CreateDbPoolHolder(
        const NYq::NConfig::TDbPoolConfig& config,
        const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
        const ::NMonitoring::TDynamicCounterPtr& counters) {
        DbPoolHolder = MakeIntrusive<NYq::TDbPoolHolder>(config, CoreYdbDriver, credentialsProviderFactory, counters);
    }
};

} // namespace

TYqSharedResources::TPtr CreateYqSharedResourcesImpl(
        const NYq::NConfig::TConfig& config,
        const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
        const ::NMonitoring::TDynamicCounterPtr& counters) {
    return MakeIntrusive<TYqSharedResourcesImpl>(config, credentialsProviderFactory, counters);
}

TYqSharedResources::TYqSharedResources(NYdb::TDriver driver)
    : CoreYdbDriver(driver)
    , UserSpaceYdbDriver(std::move(driver))
{
}

TYqSharedResources::TPtr TYqSharedResources::Cast(const IYqSharedResources::TPtr& ptr) {
    return CheckedCast<TYqSharedResources*>(ptr.Get());
}

} // namespace NYq
