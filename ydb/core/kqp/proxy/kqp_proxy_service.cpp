#include "kqp_proxy_service.h"

#include <ydb/core/kqp/kqp_impl.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/path.h>
#include <ydb/core/base/location.h>
#include <ydb/core/base/statestorage.h>
#include <ydb/core/cms/console/configs_dispatcher.h>
#include <ydb/core/cms/console/console.h>
#include <ydb/core/mind/tenant_pool.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/common/kqp_lwtrace_probes.h>
#include <ydb/core/kqp/common/kqp_timeouts.h>
#include <ydb/core/kqp/kqp_worker_common.h>
#include <ydb/core/kqp/node/kqp_node.h>
#include <ydb/core/kqp/rm/kqp_rm.h>
#include <ydb/core/kqp/runtime/kqp_spilling_file.h>
#include <ydb/core/kqp/runtime/kqp_spilling.h>
#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/public/lib/operation_id/operation_id.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>

#include <ydb/library/yql/utils/actor_log/log.h>
#include <ydb/library/yql/core/services/mounts/yql_mounts.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>
#include <library/cpp/actors/interconnect/interconnect.h>
#include <library/cpp/lwtrace/mon/mon_lwtrace.h>
#include <library/cpp/monlib/service/pages/templates.h>
#include <library/cpp/resource/resource.h>


namespace NKikimr::NKqp {

namespace {

#define KQP_PROXY_LOG_T(stream) LOG_TRACE_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_D(stream) LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_I(stream) LOG_INFO_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_N(stream) LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_W(stream) LOG_WARN_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_E(stream) LOG_ERROR_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_C(stream) LOG_CRIT_S(*TlsActivationContext, NKikimrServices::KQP_PROXY, stream)

TString MakeKqpProxyBoardPath(const TString& database) {
    return "kqpprx+" + database;
}


static constexpr TDuration DEFAULT_KEEP_ALIVE_TIMEOUT = TDuration::MilliSeconds(5000);
static constexpr TDuration DEFAULT_EXTRA_TIMEOUT_WAIT = TDuration::MilliSeconds(10);
static constexpr TDuration DEFAULT_CREATE_SESSION_TIMEOUT = TDuration::MilliSeconds(5000);


using namespace NKikimrConfig;


std::optional<ui32> GetDefaultStateStorageGroupId(const TString& database) {
    if (auto* domainInfo = AppData()->DomainsInfo->GetDomainByName(ExtractDomain(database))) {
        return domainInfo->DefaultStateStorageGroup;
    }

    return std::nullopt;
}


std::optional<ui32> TryDecodeYdbSessionId(const TString& sessionId) {
    if (sessionId.empty()) {
        return std::nullopt;
    }

    try {
        NOperationId::TOperationId opId(sessionId);
        ui32 nodeId;
        const auto& nodeIds = opId.GetValue("node_id");
        if (nodeIds.size() != 1) {
            return std::nullopt;
        }

        if (!TryFromString(*nodeIds[0], nodeId)) {
            return std::nullopt;
        }

        return nodeId;
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

bool IsSqlQuery(const NKikimrKqp::EQueryType& queryType) {
    switch (queryType) {
        case NKikimrKqp::QUERY_TYPE_SQL_DML:
        case NKikimrKqp::QUERY_TYPE_SQL_DDL:
        case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT:
        case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT_STREAMING:
        case NKikimrKqp::QUERY_TYPE_SQL_SCAN:
            return true;

        default:
            break;
    }

    return false;
}

TString EncodeSessionId(ui32 nodeId, const TString& id) {
    Ydb::TOperationId opId;
    opId.SetKind(NOperationId::TOperationId::SESSION_YQL);
    NOperationId::AddOptionalValue(opId, "node_id", ToString(nodeId));
    NOperationId::AddOptionalValue(opId, "id", Base64Encode(id));
    return NOperationId::ProtoToString(opId);
}


class TLocalSessionsRegistry {
    THashMap<TString, TKqpSessionInfo> LocalSessions;
    THashMap<TActorId, TString> TargetIdIndex;
    THashSet<TString> ShutdownInFlightSessions;
    THashMap<TString, ui32> SessionsCountPerDatabase;
    std::vector<std::vector<TString>> ReadySessions;
    TIntrusivePtr<IRandomProvider> RandomProvider;

public:
    TLocalSessionsRegistry(TIntrusivePtr<IRandomProvider> randomProvider)
        : ReadySessions(2)
        , RandomProvider(randomProvider)
    {}

    TKqpSessionInfo* Create(const TString& sessionId, const TActorId& workerId,
        const TString& database, TKqpDbCountersPtr dbCounters, bool supportsBalancing)
    {
        std::vector<i32> pos(2, -1);
        pos[0] = ReadySessions[0].size();
        ReadySessions[0].push_back(sessionId);

        if (supportsBalancing) {
            pos[1] = ReadySessions[1].size();
            ReadySessions[1].push_back(sessionId);
        }

        auto result = LocalSessions.emplace(sessionId, TKqpSessionInfo(sessionId, workerId, database, dbCounters, std::move(pos)));
        SessionsCountPerDatabase[database]++;
        Y_VERIFY(result.second, "Duplicate session id!");
        TargetIdIndex.emplace(workerId, sessionId);
        return &result.first->second;
    }

    const THashSet<TString>& GetShutdownInFlight() const {
        return ShutdownInFlightSessions;
    }

    TKqpSessionInfo* StartShutdownSession(const TString& sessionId) {
        ShutdownInFlightSessions.emplace(sessionId);
        auto ptr = LocalSessions.FindPtr(sessionId);
        ptr->ShutdownStartedAt = TAppData::TimeProvider->Now();
        RemoveSessionFromLists(ptr);
        return ptr;
    }

    TKqpSessionInfo* PickSessionToShutdown(bool force, ui32 minReasonableToKick) {
        auto& sessions = force ? ReadySessions.at(0) : ReadySessions.at(1);
        if (sessions.size() >= minReasonableToKick) {
            ui64 idx = RandomProvider->GenRand() % sessions.size();
            return StartShutdownSession(sessions[idx]);
        }

        return nullptr;
    }

    THashMap<TString, TKqpSessionInfo>::const_iterator begin() const {
        return LocalSessions.begin();
    }

    THashMap<TString, TKqpSessionInfo>::const_iterator end() const {
        return LocalSessions.end();
    }

    size_t GetShutdownInFlightSize() const {
        return ShutdownInFlightSessions.size();
    }

    void Erase(const TString& sessionId) {
        auto it = LocalSessions.find(sessionId);
        if (it != LocalSessions.end()) {
            auto counter = SessionsCountPerDatabase.find(it->second.Database);
            if (counter != SessionsCountPerDatabase.end()) {
                counter->second--;
                if (counter->second == 0) {
                    SessionsCountPerDatabase.erase(counter);
                }
            }

            RemoveSessionFromLists(&(it->second));
            ShutdownInFlightSessions.erase(sessionId);
            TargetIdIndex.erase(it->second.WorkerId);
            LocalSessions.erase(it);
        }
    }

    void RemoveSessionFromLists(TKqpSessionInfo* ptr) {
        for(ui32 i = 0; i < ptr->ReadyPos.size(); ++i) {
            i32& pos = ptr->ReadyPos.at(i);
            auto& sessions = ReadySessions.at(i);
            if (pos != -1 && pos + 1 != static_cast<i32>(sessions.size())) {
                auto& lastPos = LocalSessions.at(sessions.back()).ReadyPos.at(i);
                Y_VERIFY(lastPos + 1 == static_cast<i32>(sessions.size()));
                std::swap(sessions[pos], sessions[lastPos]);
                lastPos = pos;
            }

            if (pos != -1) {
                sessions.pop_back();
                pos = -1;
            }
        }
    }

    const TKqpSessionInfo* IsPendingShutdown(const TString& sessionId) const {
        if (ShutdownInFlightSessions.find(sessionId) != ShutdownInFlightSessions.end()) {
            return FindPtr(sessionId);
        }

        return nullptr;
    }

    bool CheckDatabaseLimits(const TString& database, ui32 databaseLimit) {
        auto it = SessionsCountPerDatabase.find(database);
        if (it == SessionsCountPerDatabase.end()){
            return true;
        }

        if (it->second + 1 <= databaseLimit) {
            return true;
        }

        return false;
    }

    size_t size() const {
        return LocalSessions.size();
    }

    const TKqpSessionInfo* FindPtr(const TString& sessionId) const {
        return LocalSessions.FindPtr(sessionId);
    }

    void Erase(const TActorId& targetId) {
        auto it = TargetIdIndex.find(targetId);
        if (it != TargetIdIndex.end()){
            Erase(it->second);
        }
    }
};


class TKqpProxyService : public TActorBootstrapped<TKqpProxyService> {
    struct TEvPrivate {
        enum EEv {
            EvReadyToPublishResources = EventSpaceBegin(TEvents::ES_PRIVATE),
            EvCollectPeerProxyData,
            EvOnRequestTimeout,
        };

        struct TEvReadyToPublishResources : public TEventLocal<TEvReadyToPublishResources, EEv::EvReadyToPublishResources> {};
        struct TEvCollectPeerProxyData: public TEventLocal<TEvCollectPeerProxyData, EEv::EvCollectPeerProxyData> {};

        struct TEvOnRequestTimeout: public TEventLocal<TEvOnRequestTimeout, EEv::EvOnRequestTimeout> {
            public:
                ui64 RequestId;
                TDuration Timeout;

            TEvOnRequestTimeout(ui64 requestId, TDuration timeout):  RequestId(requestId), Timeout(timeout) {};
        };
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_PROXY_ACTOR;
    }

    TKqpProxyService(const NKikimrConfig::TLogConfig& logConfig,
        const NKikimrConfig::TTableServiceConfig& tableServiceConfig,
        TVector<NKikimrKqp::TKqpSetting>&& settings,
        std::shared_ptr<IQueryReplayBackendFactory> queryReplayFactory)
        : YqlLoggerScope(new NYql::NLog::TTlsLogBackend(new TNullLogBackend()))
        , LogConfig(logConfig)
        , TableServiceConfig(tableServiceConfig)
        , KqpSettings(std::make_shared<const TKqpSettings>(std::move(settings)))
        , QueryReplayFactory(std::move(queryReplayFactory))
        , PendingRequests()
        , TenantsReady(false)
        , Tenants()
        , ModuleResolverState()
    {}

    void Bootstrap() {
        NLwTraceMonPage::ProbeRegistry().AddProbesList(LWTRACE_GET_PROBES(KQP_PROVIDER));
        Counters = MakeIntrusive<TKqpCounters>(AppData()->Counters, &TlsActivationContext->AsActorContext());
        ModuleResolverState = MakeIntrusive<TModuleResolverState>();

        LocalSessions = std::make_unique<TLocalSessionsRegistry>(AppData()->RandomProvider);
        RandomProvider = AppData()->RandomProvider;
        if (!GetYqlDefaultModuleResolver(ModuleResolverState->ExprCtx, ModuleResolverState->ModuleResolver)) {
            TStringStream errorStream;
            ModuleResolverState->ExprCtx.IssueManager.GetIssues().PrintTo(errorStream);

            KQP_PROXY_LOG_E("Failed to load default YQL libraries: " << errorStream.Str());
            PassAway();
        }

        ModuleResolverState->FreezeGuardHolder =
            MakeHolder<NYql::TExprContext::TFreezeGuard>(ModuleResolverState->ExprCtx);

        UpdateYqlLogLevels();

        // Subscribe for TableService & Logger config changes
        ui32 tableServiceConfigKind = (ui32)NKikimrConsole::TConfigItem::TableServiceConfigItem;
        ui32 logConfigKind = (ui32)NKikimrConsole::TConfigItem::LogConfigItem;
        Send(NConsole::MakeConfigsDispatcherID(SelfId().NodeId()),
            new NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest(
                {tableServiceConfigKind, logConfigKind}),
            IEventHandle::FlagTrackDelivery);

        WhiteBoardService = NNodeWhiteboard::MakeNodeWhiteboardServiceId(SelfId().NodeId());
        // Subscribe for tenant changes
        Send(MakeTenantPoolRootID(), new TEvents::TEvSubscribe());

        if (auto& cfg = TableServiceConfig.GetSpillingServiceConfig().GetLocalFileConfig(); cfg.GetEnable()) {
            SpillingService = TlsActivationContext->ExecutorThread.RegisterActor(CreateKqpLocalFileSpillingService(cfg, Counters));
            TlsActivationContext->ExecutorThread.ActorSystem->RegisterLocalService(
                MakeKqpLocalFileSpillingServiceID(SelfId().NodeId()), SpillingService);
        }

        // Create compile service
        CompileService = TlsActivationContext->ExecutorThread.RegisterActor(CreateKqpCompileService(TableServiceConfig,
            KqpSettings, ModuleResolverState, Counters, std::move(QueryReplayFactory)));
        TlsActivationContext->ExecutorThread.ActorSystem->RegisterLocalService(
            MakeKqpCompileServiceID(SelfId().NodeId()), CompileService);

        KqpNodeService = TlsActivationContext->ExecutorThread.RegisterActor(CreateKqpNodeService(TableServiceConfig, Counters));
        TlsActivationContext->ExecutorThread.ActorSystem->RegisterLocalService(
            MakeKqpNodeServiceID(SelfId().NodeId()), KqpNodeService);

        NActors::TMon* mon = AppData()->Mon;
        if (mon) {
             NMonitoring::TIndexMonPage* actorsMonPage = mon->RegisterIndexPage("actors", "Actors");
             mon->RegisterActorPage(actorsMonPage, "kqp_proxy", "KQP Proxy", false, TlsActivationContext->ExecutorThread.ActorSystem, SelfId());
        }

        Become(&TKqpProxyService::MainState);
        StartCollectPeerProxyData();
        PublishResourceUsage();
        AskSelfNodeInfo();
        SendWhiteboardRequest();
    }

    void AskSelfNodeInfo() {
        Send(GetNameserviceActorId(), new TEvInterconnect::TEvGetNode(SelfId().NodeId()));
    }

    void Handle(TEvInterconnect::TEvNodeInfo::TPtr& ev) {
        if (const auto& node = ev->Get()->Node) {
            SelfDataCenterId = node->Location.GetDataCenterId();
        } else {
            SelfDataCenterId = TString();
        }

        NodeResources.SetNodeId(SelfId().NodeId());
        NodeResources.SetDataCenterNumId(DataCenterFromString(*SelfDataCenterId));
        NodeResources.SetDataCenterId(*SelfDataCenterId);
        PublishResourceUsage();
        StartCollectPeerProxyData();
    }

    void StartCollectPeerProxyData() {
        Send(SelfId(), new TEvPrivate::TEvCollectPeerProxyData());
    }

    void SendBoardPublishPoison(){
        if (BoardPublishActor) {
            Send(BoardPublishActor, new TEvents::TEvPoison);
            BoardPublishActor = TActorId();
            PublishBoardPath = TString();
        }
    }

    void SendWhiteboardRequest() {
        auto ev = std::make_unique<NNodeWhiteboard::TEvWhiteboard::TEvSystemStateRequest>();
        Send(WhiteBoardService, ev.release(), IEventHandle::FlagTrackDelivery, SelfId().NodeId());
    }

    void Handle(NNodeWhiteboard::TEvWhiteboard::TEvSystemStateResponse::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        if (record.SystemStateInfoSize() != 1)  {
            KQP_PROXY_LOG_C("Unexpected whiteboard info");
            return;
        }

        const auto& info = record.GetSystemStateInfo(0);
        if (AppData()->UserPoolId >= info.PoolStatsSize()) {
            KQP_PROXY_LOG_C("Unexpected whiteboard info: pool size is smaller than user pool id"
                << ", pool size: " << info.PoolStatsSize()
                << ", user pool id: " << AppData()->UserPoolId);
            return;
        }

        const auto& pool = info.GetPoolStats(AppData()->UserPoolId);

        KQP_PROXY_LOG_D("Received node white board pool stats: " << pool.usage());
        NodeResources.SetCpuUsage(pool.usage());
        NodeResources.SetThreads(pool.threads());
    }

    void DoPublishResources() {
        SendBoardPublishPoison();

        SendWhiteboardRequest();
        if (Tenants.empty() || !SelfDataCenterId) {
            KQP_PROXY_LOG_E("Cannot start publishing usage, tenants: " << Tenants.size() << ", " << SelfDataCenterId.value_or("empty"));
            return;
        }

        const TString& database = *Tenants.begin();
        auto groupId = GetDefaultStateStorageGroupId(database);
        if (!groupId) {
            KQP_PROXY_LOG_D("Unable to determine default state storage group id for database " << database);
            return;
        }

        NodeResources.SetActiveWorkersCount(LocalSessions->size());
        PublishBoardPath = MakeKqpProxyBoardPath(database);
        auto actor = CreateBoardPublishActor(PublishBoardPath, NodeResources.SerializeAsString(), SelfId(), *groupId, 0, true);
        BoardPublishActor = Register(actor);
        LastPublishResourcesAt = TAppData::TimeProvider->Now();
    }

    void PublishResourceUsage() {
        if (ResourcesPublishScheduled) {
            return;
        }

        const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        auto now = TAppData::TimeProvider->Now();
        TDuration batchingInterval = TDuration::MilliSeconds(sbs.GetBoardPublishIntervalMs());
        if (LastPublishResourcesAt && now - *LastPublishResourcesAt < batchingInterval) {
            ResourcesPublishScheduled = true;
            Schedule(batchingInterval, new TEvPrivate::TEvReadyToPublishResources());
            return;
        }

        DoPublishResources();
    }

    void Handle(TEvPrivate::TEvReadyToPublishResources::TPtr& ev) {
        Y_UNUSED(ev);
        ResourcesPublishScheduled = false;
        DoPublishResources();
    }

    void PassAway() override {
        Send(CompileService, new TEvents::TEvPoisonPill());
        Send(SpillingService, new TEvents::TEvPoison);
        Send(KqpNodeService, new TEvents::TEvPoison);
        if (BoardPublishActor) {
            Send(BoardPublishActor, new TEvents::TEvPoison);
        }
        return TActor::PassAway();
    }

    void Handle(TEvTenantPool::TEvTenantPoolStatus::TPtr& ev) {
        const auto &event = ev->Get()->Record;

        TenantsReady = true;
        Tenants.clear();
        for (auto &slot : event.GetSlots()) {
            Tenants.insert(slot.GetAssignedTenant());
        }

        KQP_PROXY_LOG_I("Received tenant pool status, serving tenants: " << JoinRange(", ", Tenants.begin(), Tenants.end()));
        for (auto& [_, sessionInfo] : *LocalSessions) {
            if (!sessionInfo.Database.empty() && !Tenants.contains(sessionInfo.Database)) {
                auto closeSessionEv = MakeHolder<TEvKqp::TEvCloseSessionRequest>();
                closeSessionEv->Record.MutableRequest()->SetSessionId(sessionInfo.SessionId);
                Send(sessionInfo.WorkerId, closeSessionEv.Release());
            }
        }

        PublishResourceUsage();
    }

    void Handle(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse::TPtr& ev) {
        Y_UNUSED(ev);
        KQP_PROXY_LOG_D("Subscribed for config changes.");
    }

    void Handle(NConsole::TEvConsole::TEvConfigNotificationRequest::TPtr& ev) {
        auto &event = ev->Get()->Record;

        TableServiceConfig.Swap(event.MutableConfig()->MutableTableServiceConfig());
        KQP_PROXY_LOG_D("Updated table service config.");

        LogConfig.Swap(event.MutableConfig()->MutableLogConfig());
        UpdateYqlLogLevels();

        auto responseEv = MakeHolder<NConsole::TEvConsole::TEvConfigNotificationResponse>(event);
        Send(ev->Sender, responseEv.Release(), IEventHandle::FlagTrackDelivery, ev->Cookie);
        StartCollectPeerProxyData();
        PublishResourceUsage();
    }

    void Handle(TEvents::TEvUndelivered::TPtr& ev) {
        switch (ev->Get()->SourceType) {
            case NConsole::TEvConfigsDispatcher::EvSetConfigSubscriptionRequest:
                KQP_PROXY_LOG_C("Failed to deliver subscription request to config dispatcher.");
                break;

            case NConsole::TEvConsole::EvConfigNotificationResponse:
                KQP_PROXY_LOG_E("Failed to deliver config notification response.");
                break;

            case NNodeWhiteboard::TEvWhiteboard::EvSystemStateRequest:
                KQP_PROXY_LOG_D("Failed to get system details");
                break;

            case TKqpEvents::EvCreateSessionRequest: {
                KQP_PROXY_LOG_D("Remote create session request failed");
                ReplyProcessError(Ydb::StatusIds::UNAVAILABLE, "Session not found.", ev->Cookie);
                break;
            }

            case TKqpEvents::EvQueryRequest:
            case TKqpEvents::EvPingSessionRequest: {
                KQP_PROXY_LOG_D("Session not found, targetId: " << ev->Sender << " requestId: " << ev->Cookie);

                ReplyProcessError(Ydb::StatusIds::BAD_SESSION, "Session not found.", ev->Cookie);
                RemoveSession("", ev->Sender);
                break;
            }

            default:
                KQP_PROXY_LOG_E("Undelivered event with unexpected source type: " << ev->Get()->SourceType);
                break;
        }
    }

    void Handle(TEvKqp::TEvInitiateShutdownRequest::TPtr& ev) {
        KQP_PROXY_LOG_N("KQP proxy shutdown requested.");
        ShutdownRequested = true;
        ShutdownState.Reset(ev->Get()->ShutdownState.Get());
        ShutdownState->Update(LocalSessions->size());
        auto& shs = TableServiceConfig.GetShutdownSettings();
        ui32 hardTimeout = shs.GetHardTimeoutMs();
        ui32 softTimeout = shs.GetSoftTimeoutMs();
        for(auto& [idx, sessionInfo] : *LocalSessions) {
            Send(sessionInfo.WorkerId, new TEvKqp::TEvInitiateSessionShutdown(softTimeout, hardTimeout));
        }
    }

    bool CreateRemoteSession(TEvKqp::TEvCreateSessionRequest::TPtr& ev) {
        auto& event = ev->Get()->Record;
        if (!event.GetCanCreateRemoteSession() || LocalDatacenterProxies.empty()) {
            return false;
        }

        const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        if (!sbs.GetSupportRemoteSessionCreation()) {
            return false;
        }

        ui64 randomNumber = RandomProvider->GenRand();
        ui32 nodeId = LocalDatacenterProxies[randomNumber % LocalDatacenterProxies.size()];
        if (nodeId == SelfId().NodeId()){
            return false;
        }

        std::unique_ptr<TEvKqp::TEvCreateSessionRequest> remoteRequest = std::make_unique<TEvKqp::TEvCreateSessionRequest>();
        remoteRequest->Record.SetDeadlineUs(event.GetDeadlineUs());
        remoteRequest->Record.SetTraceId(event.GetTraceId());
        remoteRequest->Record.SetSupportsBalancing(event.GetSupportsBalancing());
        remoteRequest->Record.MutableRequest()->SetDatabase(event.GetRequest().GetDatabase());

        ui64 requestId = PendingRequests.RegisterRequest(ev->Sender, ev->Cookie, event.GetTraceId(), TKqpEvents::EvCreateSessionRequest);
        Send(MakeKqpProxyID(nodeId), remoteRequest.release(), IEventHandle::FlagTrackDelivery, requestId);
        TDuration timeout = DEFAULT_CREATE_SESSION_TIMEOUT;
        StartQueryTimeout(requestId, timeout);
        return true;
    }

    void Handle(TEvKqp::TEvCreateSessionRequest::TPtr& ev) {
        auto& event = ev->Get()->Record;
        auto& request = event.GetRequest();
        TKqpRequestInfo requestInfo(event.GetTraceId());
        if (CreateRemoteSession(ev)) {
            return;
        }

        auto responseEv = MakeHolder<TEvKqp::TEvCreateSessionResponse>();

        TProcessResult<TKqpSessionInfo*> result;
        TKqpDbCountersPtr dbCounters;

        const auto deadline = TInstant::MicroSeconds(event.GetDeadlineUs());

        if (CheckRequestDeadline(requestInfo, deadline, result) &&
            CreateNewSessionWorker(requestInfo, TString(DefaultKikimrPublicClusterName), true, request.GetDatabase(), event.GetSupportsBalancing(), result))
        {
            auto& response = *responseEv->Record.MutableResponse();
            response.SetSessionId(result.Value->SessionId);
            dbCounters = result.Value->DbCounters;
        } else {
            dbCounters = Counters->GetDbCounters(request.GetDatabase());
        }

        LogRequest(request, requestInfo, ev->Sender, dbCounters);

        responseEv->Record.SetResourceExhausted(result.ResourceExhausted);
        responseEv->Record.SetYdbStatus(result.YdbStatus);
        responseEv->Record.SetError(result.Error);

        LogResponse(event.GetTraceId(), responseEv->Record, dbCounters);
        Send(ev->Sender, responseEv.Release(), 0, ev->Cookie);
    }

    void Handle(TEvKqp::TEvQueryRequest::TPtr& ev) {
        auto& event = ev->Get()->Record;
        auto& request = *event.MutableRequest();
        TString traceId = event.GetTraceId();
        TKqpRequestInfo requestInfo(traceId);
        ui64 requestId = PendingRequests.RegisterRequest(ev->Sender, ev->Cookie, traceId, TKqpEvents::EvQueryRequest);

        auto dbCounters = GetDbCountersForSession(request.GetSessionId());

        auto queryLimitBytes = TableServiceConfig.GetQueryLimitBytes();
        if (queryLimitBytes && IsSqlQuery(request.GetType())) {
            auto querySizeBytes = request.GetQuery().size();
            if (querySizeBytes > queryLimitBytes) {
                TString error = TStringBuilder() << "Query text size exceeds limit (" << querySizeBytes << "b > " << queryLimitBytes << "b)";
                ReplyProcessError(Ydb::StatusIds::BAD_REQUEST, error, requestId);
                if (!dbCounters) {
                    dbCounters = Counters->GetDbCounters(request.GetDatabase());
                }
                LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);
                return;
            }
        }

        auto paramsLimitBytes = TableServiceConfig.GetParametersLimitBytes();
        if (paramsLimitBytes) {
            auto paramsBytes = request.GetParameters().ByteSizeLong();
            if (paramsBytes > paramsLimitBytes) {
                if (!dbCounters) {
                    dbCounters = Counters->GetDbCounters(request.GetDatabase());
                }
                LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);

                TString error = TStringBuilder() << "Parameters size exceeds limit (" << paramsBytes << "b > " << paramsLimitBytes << "b)";
                ReplyProcessError(Ydb::StatusIds::BAD_REQUEST, error, requestId);
                return;
            }
        }

        if (request.HasTxControl() && request.GetTxControl().has_begin_tx()) {
            switch (request.GetTxControl().begin_tx().tx_mode_case()) {
                case Ydb::Table::TransactionSettings::kSnapshotReadOnly:
                    if (!AppData()->FeatureFlags.GetEnableMvccSnapshotReads()) {
                        ReplyProcessError(Ydb::StatusIds::BAD_REQUEST,
                            "Snapshot reads not supported in current database", requestId);
                        return;
                    }

                default:
                    break;
            }
        }

        TActorId targetId;
        if (!request.GetSessionId().empty()) {
            TProcessResult<TActorId> result;
            if (!TryGetSessionTargetActor(request.GetSessionId(), requestInfo, result)) {
                if (!dbCounters) {
                    dbCounters = Counters->GetDbCounters(request.GetDatabase());
                }
                LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);
                ReplyProcessError(result.YdbStatus, result.Error, requestId);
                return;
            }

            targetId = result.Value;

            LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);
        } else {
            TProcessResult<TKqpSessionInfo*> result;
            if (!CreateNewSessionWorker(requestInfo, TString(DefaultKikimrPublicClusterName), false,
                request.GetDatabase(), false, result))
            {
                if (!dbCounters) {
                    dbCounters = Counters->GetDbCounters(request.GetDatabase());
                }
                LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);
                ReplyProcessError(result.YdbStatus, result.Error, requestId);
                return;
            }

            targetId = result.Value->WorkerId;
            request.SetSessionId(result.Value->SessionId);
            dbCounters = result.Value->DbCounters;

            LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);
        }

        TString sessionId = request.GetSessionId();
        PendingRequests.SetSessionId(requestId, sessionId, dbCounters);
        // We add extra milliseconds to the user-specified timeout, so it means we give additional priority for worker replies,
        // because it is much better to give detailed error message rather than generic timeout.
        // For example, it helps to avoid race in event order when worker and proxy recieve timeout at the same moment.
        // If worker located in the different datacenter we should better substract some RTT estimate, but at this point it's not done.
        auto timeoutMs = GetQueryTimeout(request.GetType(), request.GetTimeoutMs(), TableServiceConfig) + DEFAULT_EXTRA_TIMEOUT_WAIT;
        StartQueryTimeout(requestId, timeoutMs);
        Send(targetId, ev->Release().Release(), IEventHandle::FlagTrackDelivery, requestId);
        KQP_PROXY_LOG_D(TKqpRequestInfo(traceId, sessionId) << "Sent request to target, requestId: " << requestId << ", targetId: " << targetId);
    }

    void Handle(TEvKqp::TEvCloseSessionRequest::TPtr& ev) {
        auto& event = ev->Get()->Record;
        auto& request = event.GetRequest();

        TKqpRequestInfo requestInfo(event.GetTraceId());

        TString sessionId = request.GetSessionId();
        auto dbCounters = GetDbCountersForSession(sessionId);

        LogRequest(request, requestInfo, ev->Sender, dbCounters);

        auto sessionInfo = LocalSessions->IsPendingShutdown(sessionId);
        if (sessionInfo) {
            if (dbCounters) {
                // session is pending shutdown, and we close it
                // but direct request from user.
                Counters->ReportSessionGracefulShutdownHit(sessionInfo->DbCounters);
            }
        }

        if (!sessionId.empty()) {
            TProcessResult<TActorId> result;
            if (TryGetSessionTargetActor(sessionId, requestInfo, result)) {
                Send(result.Value, ev->Release().Release());
            }
        }
    }

    void Handle(TEvKqp::TEvPingSessionRequest::TPtr& ev) {
        auto& event = ev->Get()->Record;
        auto& request = event.GetRequest();

        auto traceId = event.GetTraceId();
        TKqpRequestInfo requestInfo(traceId);
        auto sessionId = request.GetSessionId();
        ui64 requestId = PendingRequests.RegisterRequest(ev->Sender, ev->Cookie, traceId, TKqpEvents::EvPingSessionRequest);

        auto dbCounters = GetDbCountersForSession(sessionId);

        LogRequest(request, requestInfo, ev->Sender, requestId, dbCounters);

        TProcessResult<TActorId> result;
        if (!TryGetSessionTargetActor(sessionId, requestInfo, result)) {
            ReplyProcessError(result.YdbStatus, result.Error, requestId);
            return;
        }

        TDuration timeout = DEFAULT_KEEP_ALIVE_TIMEOUT;
        if (request.GetTimeoutMs() > 0) {
            timeout = TDuration::MilliSeconds(Min(timeout.MilliSeconds(), (ui64)request.GetTimeoutMs()));
        }

        PendingRequests.SetSessionId(requestId, sessionId, dbCounters);
        StartQueryTimeout(requestId, timeout);
        Send(result.Value, ev->Release().Release(), IEventHandle::FlagTrackDelivery, requestId);
    }

    template<typename TEvent>
    void ForwardEvent(TEvent ev) {
        ui64 requestId = ev->Cookie;

        StopQueryTimeout(requestId);
        auto proxyRequest = PendingRequests.FindPtr(requestId);
        if (!proxyRequest) {
            KQP_PROXY_LOG_E("Unknown sender for proxy response, requestId: " << requestId);
            return;
        }

        LogResponse(proxyRequest->TraceId, ev->Get()->Record, proxyRequest->DbCounters);
        Send(proxyRequest->Sender, ev->Release().Release(), 0, proxyRequest->SenderCookie);

        TKqpRequestInfo requestInfo(proxyRequest->TraceId);
        KQP_PROXY_LOG_D(requestInfo << "Forwarded response to sender actor, requestId: " << requestId
            << ", sender: " << proxyRequest->Sender << ", selfId: " << SelfId());

        PendingRequests.Erase(requestId);
    }

    void LookupPeerProxyData() {
        if (!SelfDataCenterId || BoardLookupActor || Tenants.empty()) {
            return;
        }

        const TString& database = *Tenants.begin();
        auto groupId = GetDefaultStateStorageGroupId(database);
        if (!groupId) {
            KQP_PROXY_LOG_W("Unable to determine default state storage group id");
            return;
        }

        if (PublishBoardPath) {
            auto actor = CreateBoardLookupActor(PublishBoardPath, SelfId(), *groupId, EBoardLookupMode::Majority, false, false);
            BoardLookupActor = Register(actor);
        }
    }

    void Handle(TEvPrivate::TEvCollectPeerProxyData::TPtr& ev) {
        Y_UNUSED(ev);
        LookupPeerProxyData();
        if (!ShutdownRequested) {
            const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
            ui64 millis = sbs.GetBoardLookupIntervalMs();
            TDuration d = TDuration::MilliSeconds(millis + (RandomProvider->GenRand() % millis));
            Schedule(d, new TEvPrivate::TEvCollectPeerProxyData());
        }
    }

    void Handle(TEvStateStorage::TEvBoardInfo::TPtr& ev) {
        auto boardInfo = ev->Get();
        BoardLookupActor = TActorId();

        if (boardInfo->Status != TEvStateStorage::TEvBoardInfo::EStatus::Ok || PublishBoardPath != boardInfo->Path) {
            PeerProxyNodeResources.clear();
            KQP_PROXY_LOG_D("Received unexpected data from board: " << boardInfo->Path << ", current board path "
                << PublishBoardPath << ", status: " << (int) boardInfo->Status);
            return;
        }

        Y_VERIFY(SelfDataCenterId);
        PeerProxyNodeResources.resize(boardInfo->InfoEntries.size());
        size_t idx = 0;
        auto getDataCenterId = [](const auto& entry) {
            return entry.HasDataCenterId() ? entry.GetDataCenterId() : DataCenterToString(entry.GetDataCenterNumId());
        };

        LocalDatacenterProxies.clear();
        for(auto& [ownerId, entry] : boardInfo->InfoEntries) {
            Y_PROTOBUF_SUPPRESS_NODISCARD PeerProxyNodeResources[idx].ParseFromString(entry.Payload);
            if (getDataCenterId(PeerProxyNodeResources[idx]) == *SelfDataCenterId) {
                LocalDatacenterProxies.emplace_back(PeerProxyNodeResources[idx].GetNodeId());
            }
            ++idx;
        }

        PeerStats = CalcPeerStats(PeerProxyNodeResources, *SelfDataCenterId);
        TryKickSession();
    }

    bool ShouldStartBalancing(const TSimpleResourceStats& stats, const double minResourceThreshold, const double currentResourceUsage) const {
        const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        if (stats.CV < sbs.GetMinCVTreshold()) {
            return false;
        }

        if (stats.CV < sbs.GetMaxCVTreshold() && ServerWorkerBalancerComplete) {
            return false;
        }

        if (stats.Mean < currentResourceUsage && minResourceThreshold < currentResourceUsage) {
            return true;
        }

        return false;
    }

    std::pair<bool, ui32> GetBalancerEnableSettings() const {
        const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        ui32 maxInFlightSize = sbs.GetMaxSessionsShutdownInFlightSize();
        bool force = false;

        auto tier = sbs.GetEnableTier();
        if (sbs.GetEnabled()) {
            // it's legacy configuration.
            tier = TTableServiceConfig_TSessionBalancerSettings::TIER_ENABLED_FOR_ALL;
        }

        switch(tier) {
            case TTableServiceConfig_TSessionBalancerSettings::TIER_DISABLED:
                return {false, 0};
            case TTableServiceConfig_TSessionBalancerSettings::TIER_ENABLED_FOR_ALL:
                return {true, maxInFlightSize};
            case TTableServiceConfig_TSessionBalancerSettings::TIER_ENABLED_FOR_SESSIONS_WITH_SUPPORT:
                return {false, maxInFlightSize};
            default:
                return {false, 0};
        }

        return {force, maxInFlightSize};
    }

    void TryKickSession() {

        const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        const std::pair<bool, ui32> settings = GetBalancerEnableSettings();

        Y_VERIFY(PeerStats);

        bool isReasonableToKick = false;

        ui32 strategy = static_cast<ui32>(sbs.GetStrategy());
        ui32 balanceByCpu = strategy & TTableServiceConfig_TSessionBalancerSettings::BALANCE_BY_CPU;
        ui32 balanceByCount = strategy & TTableServiceConfig_TSessionBalancerSettings::BALANCE_BY_COUNT;

        if (sbs.GetLocalDatacenterPolicy()) {
            if (balanceByCount) {
                isReasonableToKick |= ShouldStartBalancing(PeerStats->LocalSessionCount, static_cast<double>(sbs.GetMinNodeSessions()), static_cast<double>(LocalSessions->size()));
            }

            if (balanceByCpu) {
                isReasonableToKick |= ShouldStartBalancing(PeerStats->LocalCpu, sbs.GetMinCpuBalancerThreshold(), NodeResources.GetCpuUsage());
            }

        } else {
            if (balanceByCount) {
                isReasonableToKick |= ShouldStartBalancing(PeerStats->CrossAZSessionCount, static_cast<double>(sbs.GetMinNodeSessions()), static_cast<double>(LocalSessions->size()));
            }

            if (balanceByCpu) {
                isReasonableToKick |= ShouldStartBalancing(PeerStats->CrossAZCpu, sbs.GetMinCpuBalancerThreshold(), NodeResources.GetCpuUsage());
            }
        }

        if (!isReasonableToKick) {
            // Start balancing
            ServerWorkerBalancerComplete = true;
            return;
        } else {
            ServerWorkerBalancerComplete = false;
        }

        while(LocalSessions->GetShutdownInFlightSize() < settings.second) {
            auto sessionInfo = LocalSessions->PickSessionToShutdown(settings.first, sbs.GetMinNodeSessions());
            if (!sessionInfo) {
                break;
            }

            StartSessionGraceShutdown(sessionInfo);
        }
    }

    void StartSessionGraceShutdown(const TKqpSessionInfo* sessionInfo) {
        if (!sessionInfo)
            return;

        const auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        KQP_PROXY_LOG_D("Started grace shutdown of session, session id: " << sessionInfo->SessionId);
        ui32 hardTimeout = sbs.GetHardSessionShutdownTimeoutMs();
        ui32 softTimeout = sbs.GetSoftSessionShutdownTimeoutMs();
        Counters->ReportSessionShutdownRequest(sessionInfo->DbCounters);
        Send(sessionInfo->WorkerId, new TEvKqp::TEvInitiateSessionShutdown(softTimeout, hardTimeout));
    }

    void ProcessMonShutdownQueue(ui32 wantsToShutdown) {
        for(ui32 i = 0; i < wantsToShutdown; ++i) {
            const TKqpSessionInfo* candidate = LocalSessions->PickSessionToShutdown(true, 0);
            if (!candidate)
                break;

            StartSessionGraceShutdown(candidate);
        }
    }

    void Handle(NMon::TEvHttpInfo::TPtr& ev) {
        TStringStream str;

        auto& sbs = TableServiceConfig.GetSessionBalancerSettings();
        const TCgiParameters& cgi = ev->Get()->Request.GetParams();

        if (cgi.Has("force_shutdown")) {
            const TString& forceShutdown = cgi.Get("force_shutdown");
            ui32 wantsToShutdown = 0;
            if (forceShutdown == "all") {
                wantsToShutdown = LocalSessions->size();
            } else {
                wantsToShutdown = FromStringWithDefault<ui32>(forceShutdown, 0);
            }

            ProcessMonShutdownQueue(wantsToShutdown);
            str << "{\"status\": \"OK\", \"queueSize\": " << wantsToShutdown << "}";
            Send(ev->Sender, new NMon::TEvHttpInfoRes(str.Str()));
            return;
        }

        HTML(str) {
            PRE() {
                str << "Self:" << Endl;
                str << "  - NodeId: " << SelfId().NodeId() << Endl;
                if (SelfDataCenterId) {
                    str << "  - DataCenterId: " << *SelfDataCenterId << Endl;
                }

                str << "Serving tenants: " << Endl;
                for(auto& tenant: Tenants) {
                    str << "  - "  << tenant << Endl;
                }
                str << Endl;

                {
                    auto cgiTmp = cgi;
                    cgiTmp.InsertUnescaped("force_shutdown", "all");
                    str << "Force shutdown all sessions: <a href=\"kqp_proxy?" << cgiTmp.Print() << "\">Execute</a>" << Endl;
                }

                const std::pair<bool, ui32> sbsSettings = GetBalancerEnableSettings();
                str << "Allow shutdown all sessions: " << (sbsSettings.first ? "true": "false") << Endl;
                str << "MaxSessionsShutdownInFlightSize: " << sbsSettings.second << Endl;
                str << "LocalDatacenterPolicy: " << (sbs.GetLocalDatacenterPolicy() ? "true" : "false") << Endl;
                str << "MaxCVTreshold: " << sbs.GetMaxCVTreshold() << Endl;
                str << "MinCVTreshold: " << sbs.GetMinCVTreshold() << Endl;
                str << "Balance strategy: " << TTableServiceConfig_TSessionBalancerSettings_EBalancingStrategy_Name(sbs.GetStrategy()) << Endl;

                str << Endl;

                if (BoardPublishActor) {
                    str << "Publish status: " << Endl;
                    if (LastPublishResourcesAt) {
                        str << "Last published resources at " << *LastPublishResourcesAt << Endl;
                    }

                    if (PublishBoardPath) {
                        str << "Publish board path: " << PublishBoardPath << Endl;
                    }
                }

                str << Endl;

                str << "EnableSessionActor: "
                    << (AppData()->FeatureFlags.GetEnableKqpSessionActor() ? "true" : "false") << Endl;
                str << "Active workers/session_actors count on node: " << LocalSessions->size() << Endl;

                const auto& sessionsShutdownInFlight = LocalSessions->GetShutdownInFlight();
                if (!sessionsShutdownInFlight.empty()) {
                    str << Endl;
                    str << "Sessions shutdown in flight: " << Endl;
                    auto now = TAppData::TimeProvider->Now();
                    for(const auto& sessionId : sessionsShutdownInFlight) {
                        auto session = LocalSessions->FindPtr(sessionId);
                        str << "Session " << sessionId << " is under shutdown for " << (now - session->ShutdownStartedAt).SecondsFloat() << " seconds. " << Endl;
                    }

                    str << Endl;
                }

                if (!PeerStats) {
                    str << "No peer proxy data available." << Endl;
                } else {
                    str << Endl << "Peer Proxy data: " << Endl;
                    str << "Session count stats: " << Endl;
                    str << "Local: " << PeerStats->LocalSessionCount << Endl;
                    str << "Cross AZ: " << PeerStats->CrossAZSessionCount << Endl;

                    str << Endl << "CPU usage stats:" << Endl;
                    str << "Local: " << PeerStats->LocalCpu << Endl;
                    str << "Cross AZ: " << PeerStats->CrossAZCpu << Endl;

                    str << Endl;
                    for(const auto& entry : PeerProxyNodeResources) {
                        str << "Peer(NodeId: " << entry.GetNodeId() << ", DataCenter: " << entry.GetDataCenterId() << "): active workers: "
                            << entry.GetActiveWorkersCount() << "): cpu usage: " << entry.GetCpuUsage() << ", threads count: " << entry.GetThreads() << Endl;
                    }
                 }
            }
        }

        Send(ev->Sender, new NMon::TEvHttpInfoRes(str.Str()));
    }

    void StartQueryTimeout(ui64 requestId, TDuration timeout) {
        TActorId timeoutTimer = CreateLongTimer(
            TlsActivationContext->AsActorContext(), timeout,
            new IEventHandle(SelfId(), SelfId(), new TEvPrivate::TEvOnRequestTimeout(requestId, timeout))
        );

        KQP_PROXY_LOG_D("Scheduled timeout timer for requestId: " << requestId << " timeout: " << timeout << " actor id: " << timeoutTimer);
        if (timeoutTimer) {
            TimeoutTimers.emplace(requestId, timeoutTimer);
        }
   }

    void StopQueryTimeout(ui64 requestId) {
        auto it = TimeoutTimers.find(requestId);
        if (it != TimeoutTimers.end()) {
            Send(it->second, new TEvents::TEvPoison);
            TimeoutTimers.erase(it);
        }
    }

    void Handle(TEvPrivate::TEvOnRequestTimeout::TPtr& ev) {
        ui64 requestId = ev->Get()->RequestId;

        KQP_PROXY_LOG_D("Handle TEvPrivate::TEvOnRequestTimeout(" << requestId << ")");
        const TKqpProxyRequest* reqInfo = PendingRequests.FindPtr(requestId);
        if (!reqInfo) {
            KQP_PROXY_LOG_D("Invalid request info while on request timeout handle. RequestId: " <<  requestId);
            return;
        }

        TString message = TStringBuilder() << "Query did not complete within specified timeout, session id " << reqInfo->SessionId;
        KQP_PROXY_LOG_D("Reply timeout: requestId " <<  requestId << " sessionId" << reqInfo->SessionId);
        ReplyProcessError(Ydb::StatusIds::TIMEOUT, message, requestId);
    }

    void Handle(TEvKqp::TEvCloseSessionResponse::TPtr& ev) {
        const auto &event = ev->Get()->Record;
        if (event.GetStatus() == Ydb::StatusIds::SUCCESS && event.GetResponse().GetClosed()) {
            auto sessionId = event.GetResponse().GetSessionId();
            TActorId workerId = ev->Sender;

            RemoveSession(sessionId, workerId);

            KQP_PROXY_LOG_D("Session closed, sessionId: " << event.GetResponse().GetSessionId()
                << ", workerId: " << workerId << ", local sessions count: " << LocalSessions->size());
        }
    }

    STATEFN(MainState) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvInterconnect::TEvNodeInfo, Handle);
            hFunc(NMon::TEvHttpInfo, Handle);
            hFunc(TEvStateStorage::TEvBoardInfo, Handle);
            hFunc(TEvPrivate::TEvCollectPeerProxyData, Handle);
            hFunc(TEvPrivate::TEvReadyToPublishResources, Handle);
            hFunc(TEvents::TEvUndelivered, Handle);
            hFunc(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse, Handle);
            hFunc(NConsole::TEvConsole::TEvConfigNotificationRequest, Handle);
            hFunc(TEvTenantPool::TEvTenantPoolStatus, Handle);
            hFunc(TEvKqp::TEvQueryRequest, Handle);
            hFunc(TEvKqp::TEvCloseSessionRequest, Handle);
            hFunc(TEvKqp::TEvQueryResponse, ForwardEvent);
            hFunc(TEvKqp::TEvProcessResponse, ForwardEvent);
            hFunc(TEvKqp::TEvCreateSessionRequest, Handle);
            hFunc(TEvKqp::TEvPingSessionRequest, Handle);
            hFunc(TEvKqp::TEvCloseSessionResponse, Handle);
            hFunc(TEvKqp::TEvPingSessionResponse, ForwardEvent);
            hFunc(TEvKqp::TEvInitiateShutdownRequest, Handle);
            hFunc(TEvPrivate::TEvOnRequestTimeout, Handle);
            hFunc(NNodeWhiteboard::TEvWhiteboard::TEvSystemStateResponse, Handle);
            hFunc(TEvKqp::TEvCreateSessionResponse, ForwardEvent);
        default:
            Y_FAIL("TKqpProxyService: unexpected event type: %" PRIx32 " event: %s",
                ev->GetTypeRewrite(), ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?");
        }
    }

private:
    void LogResponse(const TKqpRequestInfo& requestInfo,
        const NKikimrKqp::TEvProcessResponse& event, TKqpDbCountersPtr dbCounters)
    {
        auto status = event.GetYdbStatus();
        if (status != Ydb::StatusIds::SUCCESS) {
            KQP_PROXY_LOG_W(requestInfo << event.GetError());
        }

        Counters->ReportResponseStatus(dbCounters, event.ByteSize(), status);
    }

    void LogResponse(const TKqpRequestInfo& requestInfo,
        const TEvKqp::TProtoArenaHolder<NKikimrKqp::TEvQueryResponse>& holder,
        TKqpDbCountersPtr dbCounters)
    {
        Y_UNUSED(requestInfo);
        const auto& event = holder.GetRef();

        Counters->ReportResponseStatus(dbCounters, event.ByteSize(),
            event.GetYdbStatus());

        for (auto& issue : event.GetResponse().GetQueryIssues()) {
            Counters->ReportIssues(dbCounters, issue);
        }

        ui64 resultsBytes = 0;
        for (auto& result : event.GetResponse().GetResults()) {
            resultsBytes += result.ByteSize();
        }
        Counters->ReportResultsBytes(dbCounters, resultsBytes);
    }

    void LogResponse(const TKqpRequestInfo& requestInfo,
        const NKikimrKqp::TEvCreateSessionResponse& event, TKqpDbCountersPtr dbCounters)
    {
        Y_UNUSED(requestInfo);

        Counters->ReportResponseStatus(dbCounters, event.ByteSize(),
            event.GetYdbStatus());
    }

    void LogResponse(const TKqpRequestInfo& requestInfo,
        const NKikimrKqp::TEvPingSessionResponse& event, TKqpDbCountersPtr dbCounters)
    {
        Y_UNUSED(requestInfo);

        Counters->ReportResponseStatus(dbCounters, event.ByteSize(), event.GetStatus());
    }

    void LogRequest(const NKikimrKqp::TCloseSessionRequest& request,
        const TKqpRequestInfo& requestInfo, const TActorId& sender,
        TKqpDbCountersPtr dbCounters)
    {
        KQP_PROXY_LOG_D(requestInfo << "Received close session request, sender: " << sender << ", SessionId: " << request.GetSessionId());
        Counters->ReportCloseSession(dbCounters, request.ByteSize());
    }

    void LogRequest(const NKikimrKqp::TQueryRequest& request,
        const TKqpRequestInfo& requestInfo, const TActorId& sender, ui64 requestId,
        TKqpDbCountersPtr dbCounters)
    {
        KQP_PROXY_LOG_D(requestInfo << "Received new query request, sender: " << sender << ", RequestId: " << requestId
            << ", Query: \"" << request.GetQuery().substr(0, 10000) << "\"");
        Counters->ReportQueryRequest(dbCounters, request);
    }

    void LogRequest(const NKikimrKqp::TCreateSessionRequest& request,
        const TKqpRequestInfo& requestInfo, const TActorId& sender,
        TKqpDbCountersPtr dbCounters)
    {
        KQP_PROXY_LOG_D(requestInfo << "Received create session request, sender: " << sender);
        Counters->ReportCreateSession(dbCounters, request.ByteSize());
    }

    void LogRequest(const NKikimrKqp::TPingSessionRequest& request,
        const TKqpRequestInfo& requestInfo, const TActorId& sender, ui64 requestId,
        TKqpDbCountersPtr dbCounters)
    {
        KQP_PROXY_LOG_D(requestInfo << "Received ping session request, sender: " << sender << " selfID: " << SelfId() << ", RequestId: " << requestId);
        Counters->ReportPingSession(dbCounters, request.ByteSize());
    }

    bool ReplyProcessError(Ydb::StatusIds::StatusCode ydbStatus, const TString& message, ui64 requestId)
    {
        auto response = TEvKqp::TEvProcessResponse::Error(ydbStatus, message);
        return Send(SelfId(), response.Release(), 0, requestId);
    }

    bool CheckRequestDeadline(const TKqpRequestInfo& requestInfo, const TInstant deadline, TProcessResult<TKqpSessionInfo*>& result)
    {
        if (!deadline) {
            return true;
        }
        auto now = TInstant::Now();
        if (now >= deadline) {
            TString error = TStringBuilder() << "Request deadline has expired for " << now - deadline << " seconds";
            KQP_PROXY_LOG_E(requestInfo << error);

            // In theory client should not see this status due to internal grpc deadline accounting.
            result.YdbStatus = Ydb::StatusIds::TIMEOUT;
            result.Error = error;
            return false;
        } else {
            KQP_PROXY_LOG_D(requestInfo << "Request has " << deadline - now << " seconds to be completed");
            return true;
        }
    }

    bool CreateNewSessionWorker(const TKqpRequestInfo& requestInfo,
        const TString& cluster, bool longSession, const TString& database, bool supportsBalancing, TProcessResult<TKqpSessionInfo*>& result)
    {
        if (!database.empty()) {
            if (!TenantsReady) {
                TString error = TStringBuilder() << "Node isn't ready to serve database requests.";

                KQP_PROXY_LOG_E(requestInfo << error);

                result.YdbStatus = Ydb::StatusIds::UNAVAILABLE;
                result.Error = error;
                return false;
            }
        }

        if (ShutdownRequested) {
            TString error = TStringBuilder() << "Cannot create session: system shutdown requested.";

            KQP_PROXY_LOG_N(requestInfo << error);

            result.ResourceExhausted = true;
            result.YdbStatus = Ydb::StatusIds::OVERLOADED;
            result.Error = error;
            return false;
        }

        auto sessionsLimitPerNode = TableServiceConfig.GetSessionsLimitPerNode();
        if (sessionsLimitPerNode && !LocalSessions->CheckDatabaseLimits(database, sessionsLimitPerNode)) {
            TString error = TStringBuilder() << "Active sessions limit exceeded, maximum allowed: "
                << sessionsLimitPerNode;
            KQP_PROXY_LOG_W(requestInfo << error);

            result.YdbStatus = Ydb::StatusIds::OVERLOADED;
            result.Error = error;
            return false;
        }

        auto sessionId = EncodeSessionId(SelfId().NodeId(), CreateGuidAsString());

        auto dbCounters = Counters->GetDbCounters(database);

        TKqpWorkerSettings workerSettings(cluster, database, TableServiceConfig, dbCounters);
        workerSettings.LongSession = longSession;

        auto config = CreateConfig(KqpSettings, workerSettings);

        IActor* workerActor = AppData()->FeatureFlags.GetEnableKqpSessionActor() && config->HasKqpForceNewEngine()
                ? CreateKqpSessionActor(SelfId(), sessionId, KqpSettings, workerSettings, ModuleResolverState, Counters)
                : CreateKqpWorkerActor(SelfId(), sessionId, KqpSettings, workerSettings, ModuleResolverState, Counters);
        auto workerId = TlsActivationContext->ExecutorThread.RegisterActor(workerActor, TMailboxType::HTSwap, AppData()->UserPoolId);
        TKqpSessionInfo* sessionInfo = LocalSessions->Create(sessionId, workerId, database, dbCounters, supportsBalancing);

        KQP_PROXY_LOG_D(requestInfo << "Created new session"
            << ", sessionId: " << sessionInfo->SessionId
            << ", workerId: " << sessionInfo->WorkerId
            << ", database: " << sessionInfo->Database
            << ", longSession: " << longSession
            << ", local sessions count: " << LocalSessions->size());

        result.YdbStatus = Ydb::StatusIds::SUCCESS;
        result.Error.clear();
        result.Value = sessionInfo;
        PublishResourceUsage();
        return true;
    }

    bool TryGetSessionTargetActor(const TString& sessionId, const TKqpRequestInfo& requestInfo, TProcessResult<TActorId>& result)
    {
        result.YdbStatus = Ydb::StatusIds::SUCCESS;
        result.Error.clear();

        auto nodeId = TryDecodeYdbSessionId(sessionId);
        if (!nodeId) {
            TString error = TStringBuilder() << "Failed to parse session id: " << sessionId;
            KQP_PROXY_LOG_W(requestInfo << error);

            result.YdbStatus = Ydb::StatusIds::BAD_REQUEST;
            result.Error = error;
            return false;
        }

        if (*nodeId == SelfId().NodeId()) {
            auto localSession = LocalSessions->FindPtr(sessionId);
            if (!localSession) {
                TString error = TStringBuilder() << "Session not found: " << sessionId;
                KQP_PROXY_LOG_N(requestInfo << error);

                result.YdbStatus = Ydb::StatusIds::BAD_SESSION;
                result.Error = error;
                return false;
            }

            result.Value = localSession->WorkerId;
            return true;
        }

        if (!Tenants.empty()) {
            auto counters = Counters->GetDbCounters(*Tenants.begin());
            Counters->ReportProxyForwardedRequest(counters);
        }

        result.Value = MakeKqpProxyID(*nodeId);
        return true;
    }

    void RemoveSession(const TString& sessionId, const TActorId& workerId) {
        if (!sessionId.empty()) {
            LocalSessions->Erase(sessionId);
            PublishResourceUsage();
            if (ShutdownRequested) {
                ShutdownState->Update(LocalSessions->size());
            }

            return;
        }

        LocalSessions->Erase(workerId);
        PublishResourceUsage();
        if (ShutdownRequested) {
            ShutdownState->Update(LocalSessions->size());
        }
    }

    void UpdateYqlLogLevels() {
        const auto& kqpYqlName = NKikimrServices::EServiceKikimr_Name(NKikimrServices::KQP_YQL);
        for (auto &entry : LogConfig.GetEntry()) {
            if (entry.GetComponent() == kqpYqlName && entry.HasLevel()) {
                auto yqlPriority = static_cast<NActors::NLog::EPriority>(entry.GetLevel());
                NYql::NDq::SetYqlLogLevels(yqlPriority);
                KQP_PROXY_LOG_D("Updated YQL logs priority: " << (ui32)yqlPriority);
                return;
            }
        }

        // Set log level based on current logger settings
        ui8 currentLevel = TlsActivationContext->LoggerSettings()->GetComponentSettings(NKikimrServices::KQP_YQL).Raw.X.Level;
        auto yqlPriority = static_cast<NActors::NLog::EPriority>(currentLevel);

        KQP_PROXY_LOG_D("Updated YQL logs priority to current level: " << (ui32)yqlPriority);
        NYql::NDq::SetYqlLogLevels(yqlPriority);
    }

    TKqpDbCountersPtr GetDbCountersForSession(const TString& sessionId) const {
        auto localSession = LocalSessions->FindPtr(sessionId);
        return localSession ? localSession->DbCounters : nullptr;
    }

private:
    NYql::NLog::YqlLoggerScope YqlLoggerScope;
    NKikimrConfig::TLogConfig LogConfig;
    NKikimrConfig::TTableServiceConfig TableServiceConfig;
    TKqpSettings::TConstPtr KqpSettings;
    std::shared_ptr<IQueryReplayBackendFactory> QueryReplayFactory;

    std::optional<TPeerStats> PeerStats;
    TKqpProxyRequestTracker PendingRequests;
    bool TenantsReady;
    bool ShutdownRequested = false;
    THashMap<ui64, NKikimrConsole::TConfigItem::EKind> ConfigSubscriptions;
    THashMap<ui64, TActorId> TimeoutTimers;
    THashSet<TString> Tenants;

    TIntrusivePtr<TKqpShutdownState> ShutdownState;
    TIntrusivePtr<TModuleResolverState> ModuleResolverState;

    TIntrusivePtr<TKqpCounters> Counters;
    std::unique_ptr<TLocalSessionsRegistry> LocalSessions;

    bool ServerWorkerBalancerComplete = false;
    std::optional<TString> SelfDataCenterId;
    TIntrusivePtr<IRandomProvider> RandomProvider;
    std::vector<ui64> LocalDatacenterProxies;
    TVector<NKikimrKqp::TKqpProxyNodeResources> PeerProxyNodeResources;
    bool ResourcesPublishScheduled = false;
    TString PublishBoardPath;
    std::optional<TInstant> LastPublishResourcesAt;
    TActorId BoardLookupActor;
    TActorId BoardPublishActor;
    TActorId CompileService;
    TActorId KqpNodeService;
    TActorId SpillingService;
    TActorId WhiteBoardService;
    NKikimrKqp::TKqpProxyNodeResources NodeResources;
};

} // namespace

IActor* CreateKqpProxyService(const NKikimrConfig::TLogConfig& logConfig,
    const NKikimrConfig::TTableServiceConfig& tableServiceConfig,
    TVector<NKikimrKqp::TKqpSetting>&& settings,
    std::shared_ptr<IQueryReplayBackendFactory> queryReplayFactory)
{
    return new TKqpProxyService(logConfig, tableServiceConfig, std::move(settings), std::move(queryReplayFactory));
}

} // namespace NKikimr::NKqp
