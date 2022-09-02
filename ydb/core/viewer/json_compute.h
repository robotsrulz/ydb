#pragma once
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/mon.h>
#include <library/cpp/actors/core/interconnect.h>
#include <ydb/core/base/tablet.h>
#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/base/subdomain.h>
#include <ydb/core/protos/services.pb.h>
#include <ydb/core/cms/console/console.h>
#include <ydb/core/base/hive.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/viewer/protos/viewer.pb.h>
#include <ydb/core/viewer/json/json.h>
#include "viewer.h"
#include "json_pipe_req.h"
#include "wb_aggregate.h"
#include "wb_merge.h"

namespace NKikimr {
namespace NViewer {

using namespace NActors;

class TJsonCompute : public TViewerPipeClient<TJsonCompute> {
    using TBase = TViewerPipeClient<TJsonCompute>;
    IViewer* Viewer;
    THashMap<TString, NKikimrViewer::TTenant> TenantByPath;
    THashMap<TPathId, NKikimrViewer::TTenant> TenantBySubDomainKey;
    THashMap<TPathId, TTabletId> HiveBySubDomainKey;
    THashMap<TString, THolder<NSchemeCache::TSchemeCacheNavigate>> NavigateResult;
    THashMap<TTabletId, THolder<TEvHive::TEvResponseHiveDomainStats>> HiveDomainStats;
    THashMap<TTabletId, THolder<TEvHive::TEvResponseHiveNodeStats>> HiveNodeStats;
    NMon::TEvHttpInfo::TPtr Event;
    THashSet<TNodeId> NodeIds;
    THashMap<TNodeId, THolder<TEvWhiteboard::TEvSystemStateResponse>> NodeSysInfo;
    TMap<TNodeId, THolder<TEvWhiteboard::TEvTabletStateResponse>> NodeTabletInfo;
    THolder<TEvInterconnect::TEvNodesInfo> NodesInfo;
    TJsonSettings JsonSettings;
    ui32 Timeout = 0;
    TString User;
    TString Path;
    TPathId FilterSubDomain;
    bool Tablets = true;
    TTabletId RootHiveId = 0;
    bool RootHiveRequested = false;
    NKikimrViewer::TComputeInfo Result;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::VIEWER_HANDLER;
    }

    TJsonCompute(IViewer* viewer, const TRequest& request)
        : Viewer(viewer)
        , Event(request.Event)
    {}

    TString GetDomainId(TPathId pathId) {
        return TStringBuilder() << pathId.OwnerId << '-' << pathId.LocalPathId;
    }

    void Bootstrap(const TActorContext& ) {
        const auto& params(Event->Get()->Request.GetParams());
        JsonSettings.EnumAsNumbers = !FromStringWithDefault<bool>(params.Get("enums"), true);
        JsonSettings.UI64AsString = !FromStringWithDefault<bool>(params.Get("ui64"), false);
        InitConfig(params);
        Timeout = FromStringWithDefault<ui32>(params.Get("timeout"), 10000);
        Tablets = FromStringWithDefault<bool>(params.Get("tablets"), Tablets);
        Path = params.Get("path");

        SendRequest(GetNameserviceActorId(), new TEvInterconnect::TEvListNodes());

        TIntrusivePtr<TDomainsInfo> domains = AppData()->DomainsInfo;
        TIntrusivePtr<TDomainsInfo::TDomain> domain = domains->Domains.begin()->second;

        RequestConsoleListTenants();

        TString domainPath = "/" + domain->Name;
        if (Path.empty() || domainPath == Path) {
            NKikimrViewer::TTenant& tenant = TenantByPath[domainPath];
            tenant.SetName(domainPath);
            tenant.SetState(Ydb::Cms::GetDatabaseStatusResult::RUNNING);
            tenant.SetType(NKikimrViewer::Domain);
            RequestSchemeCacheNavigate(domainPath);
        }
        RootHiveId = domains->GetHive(domain->DefaultHiveUid);
        if (Requests == 0) {
            ReplyAndPassAway();
        }

        Become(&TThis::StateRequested, TDuration::MilliSeconds(Timeout), new TEvents::TEvWakeup());
    }

    void PassAway() override {
        for (const TNodeId nodeId : NodeIds) {
            Send(TActivationContext::InterconnectProxy(nodeId), new TEvents::TEvUnsubscribe);
        }
        TBase::PassAway();
    }

    STATEFN(StateRequested) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvInterconnect::TEvNodesInfo, Handle);
            hFunc(NConsole::TEvConsole::TEvListTenantsResponse, Handle);
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            hFunc(TEvHive::TEvResponseHiveDomainStats, Handle);
            hFunc(TEvHive::TEvResponseHiveNodeStats, Handle);
            hFunc(TEvWhiteboard::TEvSystemStateResponse, Handle);
            hFunc(TEvWhiteboard::TEvTabletStateResponse, Handle);
            hFunc(TEvents::TEvUndelivered, Undelivered);
            hFunc(TEvInterconnect::TEvNodeDisconnected, Disconnected);
            hFunc(TEvTabletPipe::TEvClientConnected, TBase::Handle);
            cFunc(TEvents::TSystem::Wakeup, HandleTimeout);
        }
    }

    void Handle(TEvInterconnect::TEvNodesInfo::TPtr &ev) {
        NodesInfo = ev->Release();
        RequestDone();
    }

    void Handle(NConsole::TEvConsole::TEvListTenantsResponse::TPtr& ev) {
        Ydb::Cms::ListDatabasesResult listTenantsResult;
        ev->Get()->Record.GetResponse().operation().result().UnpackTo(&listTenantsResult);
        for (const TString& path : listTenantsResult.paths()) {
            if (!Path.empty() && path != Path) {
                continue;
            }
            TenantByPath[path];
            RequestSchemeCacheNavigate(path);
        }
        RequestDone();
    }

    void Handle(TEvHive::TEvResponseHiveDomainStats::TPtr& ev) {
        for (const NKikimrHive::THiveDomainStats& hiveStat : ev->Get()->Record.GetDomainStats()) {
            TPathId subDomainKey({hiveStat.GetShardId(), hiveStat.GetPathId()});
            if (FilterSubDomain && FilterSubDomain != subDomainKey) {
                continue;
            }
            NKikimrViewer::TTenant& tenant = TenantBySubDomainKey[subDomainKey];
            if (ev->Cookie != HiveBySubDomainKey[subDomainKey]) {
                continue; // we avoid overwrite of tenant stats by root stats
            }
            tenant.SetId(GetDomainId({hiveStat.GetShardId(), hiveStat.GetPathId()}));
            tenant.MutableStateStats()->CopyFrom(hiveStat.GetStateStats());
            tenant.MutableMetrics()->CopyFrom(hiveStat.GetMetrics());
            tenant.MutableNodeIds()->CopyFrom(hiveStat.GetNodeIds());
            tenant.SetAliveNodes(hiveStat.GetAliveNodes());

            for (TNodeId nodeId : hiveStat.GetNodeIds()) {
                if (NodeIds.insert(nodeId).second) {
                    TActorId whiteboardServiceId = MakeNodeWhiteboardServiceId(nodeId);
                    THolder<NNodeWhiteboard::TEvWhiteboard::TEvSystemStateRequest> request = MakeHolder<NNodeWhiteboard::TEvWhiteboard::TEvSystemStateRequest>();
                    SendRequest(whiteboardServiceId, request.Release(), IEventHandle::FlagTrackDelivery | IEventHandle::FlagSubscribeOnSession, nodeId);
                    if (Tablets) {
                        THolder<NNodeWhiteboard::TEvWhiteboard::TEvTabletStateRequest> request = MakeHolder<NNodeWhiteboard::TEvWhiteboard::TEvTabletStateRequest>();
                        SendRequest(whiteboardServiceId, request.Release(), IEventHandle::FlagTrackDelivery | IEventHandle::FlagSubscribeOnSession, nodeId);
                    }
                }
            }
        }
        HiveDomainStats[ev->Cookie] = std::move(ev->Release());
        RequestDone();
    }

    void Handle(TEvHive::TEvResponseHiveNodeStats::TPtr& ev) {
        HiveNodeStats[ev->Cookie] = std::move(ev->Release());
        RequestDone();
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        if (ev->Get()->Request->ResultSet.size() == 1 && ev->Get()->Request->ResultSet.begin()->Status == NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            auto domainInfo = ev->Get()->Request->ResultSet.begin()->DomainInfo;
            ui64 hiveId = domainInfo->Params.GetHive();
            if (hiveId) {
                RequestHiveDomainStats(hiveId);
                RequestHiveNodeStats(hiveId);
                HiveBySubDomainKey[domainInfo->DomainKey] = hiveId;
            } else {
                if (!RootHiveRequested) {
                    RequestHiveDomainStats(RootHiveId);
                    RequestHiveNodeStats(RootHiveId);
                    RootHiveRequested = true;
                }
                HiveBySubDomainKey[domainInfo->DomainKey] = RootHiveId;
            }
            if (domainInfo->ResourcesDomainKey != domainInfo->DomainKey) {
                TenantBySubDomainKey[domainInfo->ResourcesDomainKey].SetType(NKikimrViewer::Shared);
                TenantBySubDomainKey[domainInfo->DomainKey].SetType(NKikimrViewer::Serverless);
                TenantBySubDomainKey[domainInfo->DomainKey].SetResourceId(GetDomainId(domainInfo->ResourcesDomainKey));
            }

            TString path = CanonizePath(ev->Get()->Request->ResultSet.begin()->Path);
            NavigateResult[path] = std::move(ev->Get()->Request);
            if (Path && Path == path) {
                FilterSubDomain = domainInfo->DomainKey;
            }
        }
        RequestDone();
    }

    void Handle(NNodeWhiteboard::TEvWhiteboard::TEvSystemStateResponse::TPtr& ev) {
        ui32 nodeId = ev.Get()->Cookie;
        NodeSysInfo[nodeId] = ev->Release();
        RequestDone();
    }

    void Handle(NNodeWhiteboard::TEvWhiteboard::TEvTabletStateResponse::TPtr& ev) {
        ui32 nodeId = ev.Get()->Cookie;
        NodeTabletInfo[nodeId] = ev->Release();
        RequestDone();
    }

    void Undelivered(TEvents::TEvUndelivered::TPtr& ev) {
        ui32 nodeId = ev.Get()->Cookie;
        if (ev->Get()->SourceType == NNodeWhiteboard::TEvWhiteboard::EvSystemStateRequest) {
            if (NodeSysInfo.emplace(nodeId, nullptr).second) {
                RequestDone();
            }
        }
        if (ev->Get()->SourceType == NNodeWhiteboard::TEvWhiteboard::EvTabletStateRequest) {
            if (NodeTabletInfo.emplace(nodeId, nullptr).second) {
                RequestDone();
            }
        }
    }

    void Disconnected(TEvInterconnect::TEvNodeDisconnected::TPtr& ev) {
        ui32 nodeId = ev->Get()->NodeId;
        if (NodeSysInfo.emplace(nodeId, nullptr).second) {
            RequestDone();
        }
        if (NodeTabletInfo.emplace(nodeId, nullptr).second) {
            RequestDone();
        }
    }

    void ReplyAndPassAway() {
        THashMap<TNodeId, TVector<const NKikimrWhiteboard::TTabletStateInfo*>> tabletInfoIndex;
        THolder<TEvWhiteboard::TEvTabletStateResponse> tabletInfo = MergeWhiteboardResponses(NodeTabletInfo);
        for (const auto& info : tabletInfo->Record.GetTabletStateInfo()) {
            tabletInfoIndex[info.GetNodeId()].emplace_back(&info);
        }
        THashMap<TNodeId, const NKikimrHive::THiveNodeStats*> hiveNodeStatsIndex;
        auto itRootHiveNodeStats = HiveNodeStats.find(RootHiveId);
        if (itRootHiveNodeStats != HiveNodeStats.end()) {
            for (const auto& stats : itRootHiveNodeStats->second->Record.GetNodeStats()) {
                hiveNodeStatsIndex[stats.GetNodeId()] = &stats;
            }
        }
        for (const auto& prStats : HiveNodeStats) {
            if (prStats.first != RootHiveId) {
                for (const auto& stats : prStats.second->Record.GetNodeStats()) {
                    hiveNodeStatsIndex[stats.GetNodeId()] = &stats;
                }
            }
        }
        for (const std::pair<const TString, NKikimrViewer::TTenant>& prTenant : TenantByPath) {
            const TString& path = prTenant.first;
            NKikimrViewer::TComputeTenantInfo& computeTenantInfo = *Result.AddTenants();
            computeTenantInfo.SetName(path);
            auto itNavigate = NavigateResult.find(path);
            if (itNavigate != NavigateResult.end()) {
                NSchemeCache::TSchemeCacheNavigate::TEntry entry = itNavigate->second->ResultSet.front();
                TPathId subDomainKey(entry.DomainInfo->DomainKey);
                const NKikimrViewer::TTenant& tenantBySubDomainKey(TenantBySubDomainKey[subDomainKey]);
                for (TNodeId nodeId : tenantBySubDomainKey.GetNodeIds()) {
                    NKikimrViewer::TComputeNodeInfo& computeNodeInfo = *computeTenantInfo.AddNodes();
                    computeNodeInfo.SetNodeId(nodeId);
                    auto itSysInfo = NodeSysInfo.find(nodeId);
                    if (itSysInfo != NodeSysInfo.end()) {
                        if (itSysInfo->second != nullptr && itSysInfo->second->Record.SystemStateInfoSize() == 1) {
                            const NKikimrWhiteboard::TSystemStateInfo& sysInfo = itSysInfo->second->Record.GetSystemStateInfo(0);
                            if (sysInfo.HasStartTime()) {
                                computeNodeInfo.SetStartTime(sysInfo.GetStartTime());
                            }
                            if (sysInfo.HasChangeTime()) {
                                computeNodeInfo.SetChangeTime(sysInfo.GetChangeTime());
                            }
                            computeNodeInfo.MutableSystemLocation()->MergeFrom(sysInfo.GetSystemLocation());
                            computeNodeInfo.MutableLoadAverage()->MergeFrom(sysInfo.GetLoadAverage());
                            if (sysInfo.HasNumberOfCpus()) {
                                computeNodeInfo.SetNumberOfCpus(sysInfo.GetNumberOfCpus());
                            }
                            // TODO(xenoxeno)
                            if (sysInfo.HasSystemState()) {
                                computeNodeInfo.SetOverall(GetViewerFlag(sysInfo.GetSystemState()));
                            }
                            if (sysInfo.HasNodeId()) {
                                computeNodeInfo.SetNodeId(sysInfo.GetNodeId());
                            }
                            if (sysInfo.HasDataCenter()) {
                                computeNodeInfo.SetDataCenter(sysInfo.GetDataCenter());
                            }
                            if (sysInfo.HasRack()) {
                                computeNodeInfo.SetRack(sysInfo.GetRack());
                            }
                            if (sysInfo.HasHost()) {
                                computeNodeInfo.SetHost(sysInfo.GetHost());
                            }
                            if (sysInfo.HasVersion()) {
                                computeNodeInfo.SetVersion(sysInfo.GetVersion());
                            }
                            if (sysInfo.HasMemoryUsed()) {
                                computeNodeInfo.SetMemoryUsed(sysInfo.GetMemoryUsed());
                            }
                            if (sysInfo.HasMemoryLimit()) {
                                computeNodeInfo.SetMemoryLimit(sysInfo.GetMemoryLimit());
                            }
                            computeNodeInfo.MutablePoolStats()->MergeFrom(sysInfo.GetPoolStats());
                            computeNodeInfo.MutableEndpoints()->MergeFrom(sysInfo.GetEndpoints());
                            computeNodeInfo.MutableRoles()->MergeFrom(sysInfo.GetRoles());

                        }
                    }
                    auto itTabletInfo = tabletInfoIndex.find(nodeId);
                    if (itTabletInfo != tabletInfoIndex.end()) {
                        THashMap<std::pair<NKikimrTabletBase::TTabletTypes::EType, NKikimrViewer::EFlag>, NKikimrViewer::TTabletStateInfo> tablets;
                        for (const auto* pTabletInfo : itTabletInfo->second) {
                            const auto& tabletInfo = *pTabletInfo;
                            if (tabletInfo.GetState() != NKikimrWhiteboard::TTabletStateInfo::Deleted) {
                                NKikimrViewer::EFlag state = GetFlagFromTabletState(tabletInfo.GetState());
                                auto& tablet = tablets[std::make_pair(tabletInfo.GetType(), state)];
                                tablet.SetCount(tablet.GetCount() + 1);
                            }
                        }
                        for (const auto& [prTypeState, tabletInfo] : tablets) {
                            NKikimrViewer::TTabletStateInfo& tablet = *computeNodeInfo.AddTablets();
                            tablet.MergeFrom(tabletInfo);
                            tablet.SetType(NKikimrTabletBase::TTabletTypes::EType_Name(prTypeState.first));
                            tablet.SetState(prTypeState.second);
                        }
                    }
                    auto itHiveNodeStats = hiveNodeStatsIndex.find(nodeId);
                    if (itHiveNodeStats != hiveNodeStatsIndex.end()) {
                        computeNodeInfo.MutableMetrics()->CopyFrom(itHiveNodeStats->second->GetMetrics());
                    }
                }
            }

            // TODO(xenoxeno)
            computeTenantInfo.SetOverall(NKikimrViewer::EFlag::Green);
        }

        // TODO(xenoxeno)
        Result.SetOverall(NKikimrViewer::EFlag::Green);
        TStringStream json;
        TProtoToJson::ProtoToJson(json, Result, JsonSettings);
        Send(Event->Sender, new NMon::TEvHttpInfoRes(Viewer->GetHTTPOKJSON(Event->Get(), std::move(json.Str())), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
        PassAway();
    }

    void HandleTimeout() {
        Result.AddErrors("Timeout occurred");
        ReplyAndPassAway();
    }
};

template <>
struct TJsonRequestSchema<TJsonCompute> {
    static TString GetSchema() {
        TStringStream stream;
        TProtoToJson::ProtoToJsonSchema<NKikimrViewer::TNetInfo>(stream);
        return stream.Str();
    }
};

template <>
struct TJsonRequestParameters<TJsonCompute> {
    static TString GetParameters() {
        return R"___([{"name":"path","in":"query","description":"schema path","required":false,"type":"string"},
                      {"name":"enums","in":"query","description":"convert enums to strings","required":false,"type":"boolean"},
                      {"name":"ui64","in":"query","description":"return ui64 as number","required":false,"type":"boolean"},
                      {"name":"timeout","in":"query","description":"timeout in ms","required":false,"type":"integer"}])___";
    }
};

template <>
struct TJsonRequestSummary<TJsonCompute> {
    static TString GetSummary() {
        return "\"Database compute information\"";
    }
};

template <>
struct TJsonRequestDescription<TJsonCompute> {
    static TString GetDescription() {
        return "\"Returns information about compute layer of database\"";
    }
};

}
}
