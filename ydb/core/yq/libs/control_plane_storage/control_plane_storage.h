#pragma once

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <ydb/core/yq/libs/config/protos/common.pb.h>
#include <ydb/core/yq/libs/config/protos/control_plane_storage.pb.h>
#include <ydb/core/yq/libs/shared_resources/shared_resources.h>
#include <ydb/core/yq/libs/actors/logging/log.h>

#define CPS_LOG_D(s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_DEBUG(s)
#define CPS_LOG_I(s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_INFO(s)
#define CPS_LOG_W(s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_WARN(s)
#define CPS_LOG_E(s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_ERROR(s)
#define CPS_LOG_T(s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_TRACE(s)


#define CPS_LOG_AS_D(a, s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_AS_DEBUG(a, s)
#define CPS_LOG_AS_I(a, s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_AS_INFO(a, s)
#define CPS_LOG_AS_W(a, s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_AS_WARN(a, s)
#define CPS_LOG_AS_E(a, s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_AS_ERROR(a, s)
#define CPS_LOG_AS_T(a, s) \
    LOG_YQ_CONTROL_PLANE_STORAGE_AS_TRACE(a, s)


namespace NYq {

NActors::TActorId ControlPlaneStorageServiceActorId(ui32 nodeId = 0);

NActors::IActor* CreateInMemoryControlPlaneStorageServiceActor(const NConfig::TControlPlaneStorageConfig& config);

NActors::IActor* CreateYdbControlPlaneStorageServiceActor(
    const NConfig::TControlPlaneStorageConfig& config,
    const NConfig::TCommonConfig& common,
    const ::NMonitoring::TDynamicCounterPtr& counters,
    const NYq::TYqSharedResources::TPtr& yqSharedResources,
    const NKikimr::TYdbCredentialsProviderFactory& credentialsProviderFactory,
    const TString& tenantName);

} // namespace NYq
