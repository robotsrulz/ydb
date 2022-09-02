#pragma once
#include "defs.h"
#include <ydb/core/base/blobstorage.h>
#include <ydb/core/base/events.h>
#include <ydb/core/base/subdomain.h>
#include <ydb/core/base/tablet_types.h>
#include <ydb/core/blobstorage/groupinfo/blobstorage_groupinfo.h>
#include <ydb/core/blobstorage/groupinfo/blobstorage_groupinfo_iter.h>
#include <ydb/core/protos/node_whiteboard.pb.h>
#include <library/cpp/actors/interconnect/events_local.h>
#include <library/cpp/actors/core/interconnect.h>
#include <ydb/core/base/tracing.h>

namespace NKikimr {

using TTabletId = ui64;
using TFollowerId = ui32;
using TNodeId = ui32;

namespace NNodeWhiteboard {

struct TEvWhiteboard{
    enum EEv {
        EvTabletStateUpdate = EventSpaceBegin(TKikimrEvents::ES_NODE_WHITEBOARD),
        EvTabletStateRequest,
        EvTabletStateResponse,
        EvNodeStateUpdate,
        EvNodeStateDelete,
        EvNodeStateRequest,
        EvNodeStateResponse,
        EvPDiskStateUpdate,
        EvPDiskStateRequest,
        EvPDiskStateResponse,
        EvVDiskStateUpdate,
        EvVDiskStateRequest,
        EvVDiskStateResponse,
        EvSystemStateUpdate,
        EvSystemStateRequest,
        EvSystemStateResponse,
        EvBSGroupStateUpdate,
        EvBSGroupStateRequest,
        EvBSGroupStateResponse,
        EvVDiskStateDelete,
        EvSystemStateAddEndpoint,
        EvSystemStateAddRole,
        EvSystemStateSetTenant,
        EvSystemStateRemoveTenant,
        EvBSGroupStateDelete,
        EvIntrospectionData,
        EvTabletLookupRequest,
        EvTabletLookupResponse,
        EvTraceLookupRequest,
        EvTraceLookupResponse,
        EvTraceRequest,
        EvTraceResponse,
        EvSignalBodyRequest,
        EvSignalBodyResponse,
        EvPDiskStateDelete,
        EvVDiskStateGenerationChange,
        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_NODE_WHITEBOARD), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_NODE_WHITEBOARD)");

    struct TEvTabletStateUpdate : TEventPB<TEvTabletStateUpdate, NKikimrWhiteboard::TTabletStateInfo, EvTabletStateUpdate> {
        TEvTabletStateUpdate() = default;

        TEvTabletStateUpdate(TTabletId tabletId, TFollowerId followerId, NKikimrWhiteboard::TTabletStateInfo::ETabletState state, const TIntrusivePtr<TTabletStorageInfo>& storageInfo, ui32 generation, bool leader) {
            Record.SetTabletId(tabletId);
            Record.SetFollowerId(followerId);
            Record.SetType(storageInfo->TabletType);
            Record.SetState(state);
            Record.SetGeneration(generation);
            Record.SetLeader(leader);
            Record.MutableChannelGroupIDs()->Resize(storageInfo->Channels.size(), 0);
            for (const auto& channel : storageInfo->Channels) {
                const auto* latestEntry = channel.LatestEntry();
                ui32 groupId = latestEntry != nullptr ? latestEntry->GroupID : 0;
                Record.SetChannelGroupIDs(channel.Channel, groupId);
            }
            if (storageInfo->HiveId) {
                Record.SetHiveId(storageInfo->HiveId);
            }
            if (storageInfo->TenantPathId) {
                Record.MutableTenantId()->CopyFrom(TSubDomainKey(storageInfo->TenantPathId.OwnerId, storageInfo->TenantPathId.LocalPathId));
            }
        }

        TEvTabletStateUpdate(TTabletId tabletId, TFollowerId followerId, NKikimrWhiteboard::TTabletStateInfo::ETabletState state, ui32 generation) {
            Record.SetTabletId(tabletId);
            Record.SetFollowerId(followerId);
            Record.SetState(state);
            Record.SetGeneration(generation);
        }

        TEvTabletStateUpdate(TTabletId tabletId, TFollowerId followerId, NKikimrWhiteboard::TTabletStateInfo::ETabletState state, ui32 generation, bool leader) {
            Record.SetTabletId(tabletId);
            Record.SetFollowerId(followerId);
            Record.SetState(state);
            Record.SetGeneration(generation);
            Record.SetLeader(leader);
        }

        TEvTabletStateUpdate(TTabletId tabletId, ui32 userState) {
            Record.SetTabletId(tabletId);
            Record.SetUserState(userState);
        }
    };

    struct TEvTabletStateRequest : public TEventPB<TEvTabletStateRequest, NKikimrWhiteboard::TEvTabletStateRequest, EvTabletStateRequest> {};

    struct TEvTabletStateResponse : public TEventPB<TEvTabletStateResponse, NKikimrWhiteboard::TEvTabletStateResponse, EvTabletStateResponse> {};

    struct TEvPDiskStateUpdate : TEventPB<TEvPDiskStateUpdate, NKikimrWhiteboard::TPDiskStateInfo, EvPDiskStateUpdate> {
        TEvPDiskStateUpdate() = default;

        TEvPDiskStateUpdate(ui32 pDiskId, const TString& path, ui64 guid, ui64 category) {
            Record.SetPDiskId(pDiskId);
            Record.SetPath(path);
            Record.SetGuid(guid);
            Record.SetCategory(category);
        }

        TEvPDiskStateUpdate(ui32 pDiskId, ui64 availableSize, ui64 totalSize, ui32 state) {
            Record.SetPDiskId(pDiskId);
            Record.SetAvailableSize(availableSize);
            Record.SetTotalSize(totalSize);
            Record.SetState(static_cast<NKikimrBlobStorage::TPDiskState::E>(state));
        }

        TEvPDiskStateUpdate(ui32 pDiskId, NKikimrWhiteboard::EFlag realtime, NKikimrWhiteboard::EFlag device) {
            Record.SetPDiskId(pDiskId);
            Record.SetRealtime(realtime);
            Record.SetDevice(device);
        }
    };

    struct TEvPDiskStateRequest : public TEventPB<TEvPDiskStateRequest, NKikimrWhiteboard::TEvPDiskStateRequest, EvPDiskStateRequest> {};

    struct TEvPDiskStateResponse : public TEventPB<TEvPDiskStateResponse, NKikimrWhiteboard::TEvPDiskStateResponse, EvPDiskStateResponse> {};

    struct TEvVDiskStateUpdate : TEventPB<TEvVDiskStateUpdate, NKikimrWhiteboard::TVDiskStateInfo, EvVDiskStateUpdate> {
        const bool Initial = false;

        TEvVDiskStateUpdate() = default;

        // this message is generated by NodeWarden
        explicit TEvVDiskStateUpdate(const TVDiskID& vDiskId, const TMaybe<TString>& storagePoolName, ui32 pDiskId,
                ui32 vDiskSlotId, ui64 guid, NKikimrBlobStorage::TVDiskKind::EVDiskKind kind, bool donorMode,
                ui64 instanceGuid, std::vector<NKikimrBlobStorage::TVSlotId>&& donors)
            : Initial(true)
        {
            VDiskIDFromVDiskID(vDiskId, Record.MutableVDiskId());
            if (storagePoolName) {
                Record.SetStoragePoolName(*storagePoolName);
            }
            Record.SetPDiskId(pDiskId);
            Record.SetVDiskSlotId(vDiskSlotId);
            Record.SetGuid(guid);
            Record.SetKind(kind);
            if (donorMode) {
                Record.SetDonorMode(true);
            }
            Record.SetInstanceGuid(instanceGuid);
            for (auto&& donor : donors) {
                *Record.AddDonors() = std::move(donor);
            }
        }

        explicit TEvVDiskStateUpdate(NKikimrWhiteboard::TVDiskSatisfactionRank *satisfactionRank) {
            Record.MutableSatisfactionRank()->Swap(satisfactionRank);
        }

        explicit TEvVDiskStateUpdate(NKikimrWhiteboard::EVDiskState state,
                                     NKikimrWhiteboard::EFlag diskSpace,
                                     bool replicated,
                                     bool unreplicatedPhantoms,
                                     bool unreplicatedNonPhantoms,
                                     ui64 unsyncedVDisks,
                                     NKikimrWhiteboard::EFlag frontQueuesLigth,
                                     bool hasUnreadableBlobs) {
            Record.SetVDiskState(state);
            Record.SetDiskSpace(diskSpace);
            Record.SetReplicated(replicated);
            Record.SetUnreplicatedPhantoms(unreplicatedPhantoms);
            Record.SetUnreplicatedNonPhantoms(unreplicatedNonPhantoms);
            Record.SetUnsyncedVDisks(unsyncedVDisks);
            Record.SetFrontQueues(frontQueuesLigth);
            Record.SetHasUnreadableBlobs(hasUnreadableBlobs);
        }

        static constexpr struct TUpdateIncarnationGuid {} UpdateIncarnationGuid{};

        explicit TEvVDiskStateUpdate(TUpdateIncarnationGuid, ui64 incarnationGuid) {
            Record.SetIncarnationGuid(incarnationGuid);
        }

        explicit TEvVDiskStateUpdate(NKikimrWhiteboard::TVDiskStateInfo&& rec) {
            Record = std::move(rec);
        }
    };

    struct TEvVDiskStateDelete : TEventPB<TEvVDiskStateDelete, NKikimrWhiteboard::TVDiskStateInfo, EvVDiskStateDelete> {
        TEvVDiskStateDelete() = default;

        explicit TEvVDiskStateDelete(const TVDiskID& vDiskId) {
            VDiskIDFromVDiskID(vDiskId, Record.MutableVDiskId());
        }
    };

    struct TEvVDiskStateGenerationChange : TEventLocal<TEvVDiskStateGenerationChange, EvVDiskStateGenerationChange> {
        const TVDiskID VDiskId;
        const ui32 Generation;
        const ui64 InstanceGuid;

        TEvVDiskStateGenerationChange(const TVDiskID& vdiskId, ui32 generation, ui64 instanceGuid)
            : VDiskId(vdiskId)
            , Generation(generation)
            , InstanceGuid(instanceGuid)
        {}
    };

    struct TEvPDiskStateDelete : TEventPB<TEvPDiskStateDelete, NKikimrWhiteboard::TPDiskStateInfo, EvPDiskStateDelete> {
        TEvPDiskStateDelete() = default;

        explicit TEvPDiskStateDelete(const ui32& pdiskId) {
            Record.SetPDiskId(pdiskId);
        }
    };


    struct TEvVDiskStateRequest : public TEventPB<TEvVDiskStateRequest, NKikimrWhiteboard::TEvVDiskStateRequest, EvVDiskStateRequest> {};

    struct TEvVDiskStateResponse : public TEventPB<TEvVDiskStateResponse, NKikimrWhiteboard::TEvVDiskStateResponse, EvVDiskStateResponse> {};

    struct TEvBSGroupStateUpdate : TEventPB<TEvBSGroupStateUpdate, NKikimrWhiteboard::TBSGroupStateInfo, EvBSGroupStateUpdate> {
        TEvBSGroupStateUpdate() = default;

        TEvBSGroupStateUpdate(const TIntrusivePtr<TBlobStorageGroupInfo>& groupInfo) {
            Record.SetGroupID(groupInfo->GroupID);
            Record.SetGroupGeneration(groupInfo->GroupGeneration);
            Record.SetErasureSpecies(groupInfo->Type.ErasureSpeciesName(groupInfo->Type.GetErasure()));
            for (ui32 i = 0; i < groupInfo->GetTotalVDisksNum(); ++i) {
                VDiskIDFromVDiskID(groupInfo->GetVDiskId(i), Record.AddVDiskIds());
            }
            Record.SetStoragePoolName(groupInfo->GetStoragePoolName());
        }
    };

    struct TEvBSGroupStateDelete : TEventPB<TEvBSGroupStateDelete, NKikimrWhiteboard::TBSGroupStateInfo, EvBSGroupStateDelete> {
        TEvBSGroupStateDelete() = default;

        TEvBSGroupStateDelete(ui32 groupID) {
            Record.SetGroupID(groupID);
        }
    };

    struct TEvBSGroupStateRequest : public TEventPB<TEvBSGroupStateRequest, NKikimrWhiteboard::TEvBSGroupStateRequest, EvBSGroupStateRequest> {};

    struct TEvBSGroupStateResponse : public TEventPB<TEvBSGroupStateResponse, NKikimrWhiteboard::TEvBSGroupStateResponse, EvBSGroupStateResponse> {};

    struct TEvSystemStateUpdate : TEventPB<TEvSystemStateUpdate, NKikimrWhiteboard::TSystemStateInfo, EvSystemStateUpdate> {
        TEvSystemStateUpdate() = default;

        TEvSystemStateUpdate(
                TInstant startTime,
                ui32 numberOfCpus,
                const TString& version) {
            Record.SetStartTime(startTime.MilliSeconds());
            Record.SetNumberOfCpus(numberOfCpus);
            Record.SetVersion(version);
        }

        TEvSystemStateUpdate(
                TInstant startTime,
                ui32 numberOfCpus) {
            Record.SetStartTime(startTime.MilliSeconds());
            Record.SetNumberOfCpus(numberOfCpus);
        }

        TEvSystemStateUpdate(const TVector<double>& loadAverage) {
            for (double d : loadAverage) {
                Record.AddLoadAverage(d);
            }
        }

        TEvSystemStateUpdate(const TVector<std::tuple<TString, double, ui32>>& poolStats) {
            for (const auto& row : poolStats) {
                auto& pb = *Record.AddPoolStats();
                pb.SetName(std::get<0>(row));
                pb.SetUsage(std::get<1>(row));
                pb.SetThreads(std::get<2>(row));
            }
        }

        TEvSystemStateUpdate(const TNodeLocation& systemLocation) {
            systemLocation.Serialize(Record.MutableLocation(), false);
            const auto& x = systemLocation.GetLegacyValue();
            auto *pb = Record.MutableSystemLocation();
            pb->SetDataCenter(x.DataCenter);
            pb->SetRoom(x.Room);
            pb->SetRack(x.Rack);
            pb->SetBody(x.Body);
        }

        TEvSystemStateUpdate(const NKikimrWhiteboard::TSystemStateInfo& systemStateInfo) {
            Record.CopyFrom(systemStateInfo);
        }
    };

    struct TEvSystemStateAddEndpoint : TEventLocal<TEvSystemStateAddEndpoint, EvSystemStateAddEndpoint> {
        TString Name;
        TString Address;

        TEvSystemStateAddEndpoint(const TString& name, const TString& address)
            : Name(name)
            , Address(address)
        {}
    };

    struct TEvSystemStateAddRole : TEventLocal<TEvSystemStateAddRole, EvSystemStateAddRole> {
        TString Role;

        TEvSystemStateAddRole(const TString& role)
            : Role(role)
        {}
    };

    struct TEvSystemStateSetTenant : TEventLocal<TEvSystemStateSetTenant, EvSystemStateSetTenant> {
        TString Tenant;

        TEvSystemStateSetTenant(const TString& tenant)
            : Tenant(tenant)
        {}
    };

    struct TEvSystemStateRemoveTenant : TEventLocal<TEvSystemStateRemoveTenant, EvSystemStateRemoveTenant> {
        TString Tenant;

        TEvSystemStateRemoveTenant(const TString& tenant)
            : Tenant(tenant)
        {}
    };

    struct TEvSystemStateRequest : public TEventPB<TEvSystemStateRequest, NKikimrWhiteboard::TEvSystemStateRequest, EvSystemStateRequest> {};

    struct TEvSystemStateResponse : public TEventPB<TEvSystemStateResponse, NKikimrWhiteboard::TEvSystemStateResponse, EvSystemStateResponse> {};

    struct TEvNodeStateUpdate : TEventPB<TEvNodeStateUpdate, NKikimrWhiteboard::TNodeStateInfo, EvNodeStateUpdate> {
        TEvNodeStateUpdate() = default;

        TEvNodeStateUpdate(const TString& peerName, bool connected) {
            Record.SetPeerName(peerName);
            Record.SetConnected(connected);
        }

        TEvNodeStateUpdate(const TString& peerName, bool connected, NKikimrWhiteboard::EFlag connectStatus) {
            Record.SetPeerName(peerName);
            Record.SetConnected(connected);
            Record.SetConnectStatus(connectStatus);
        }
    };

    struct TEvNodeStateDelete : TEventPB<TEvNodeStateDelete, NKikimrWhiteboard::TNodeStateInfo, EvNodeStateDelete> {
        TEvNodeStateDelete() = default;

        TEvNodeStateDelete(const TString& peerName) {
            Record.SetPeerName(peerName);
        }
    };

    struct TEvNodeStateRequest : TEventPB<TEvNodeStateRequest, NKikimrWhiteboard::TEvNodeStateRequest, EvNodeStateRequest>
    {};

    struct TEvNodeStateResponse : TEventPB<TEvNodeStateResponse, NKikimrWhiteboard::TEvNodeStateResponse, EvNodeStateResponse>
    {};

    struct TEvIntrospectionData : TEventLocal<TEvIntrospectionData, EvIntrospectionData> {

        THolder<NTracing::ITrace> Trace;
        TTabletId TabletId;

        TEvIntrospectionData(TTabletId tabletId, NTracing::ITrace* trace)
            : Trace(trace)
            , TabletId(tabletId)
        {}
    };

    struct TEvTabletLookupRequest : TEventPB<TEvTabletLookupRequest, NKikimrWhiteboard::TEvTabletLookupRequest, EvTabletLookupRequest> {};
    struct TEvTabletLookupResponse : TEventPB<TEvTabletLookupResponse, NKikimrWhiteboard::TEvTabletLookupResponse, EvTabletLookupResponse> {};

    struct TEvTraceLookupRequest : TEventPB<TEvTraceLookupRequest, NKikimrWhiteboard::TEvTraceLookupRequest, EvTraceLookupRequest> {};
    struct TEvTraceLookupResponse : TEventPB<TEvTraceLookupResponse, NKikimrWhiteboard::TEvTraceLookupResponse, EvTraceLookupResponse> {};

    struct TEvTraceRequest : TEventPB<TEvTraceRequest, NKikimrWhiteboard::TEvTraceRequest, EvTraceRequest> {};
    struct TEvTraceResponse : TEventPB<TEvTraceResponse, NKikimrWhiteboard::TEvTraceResponse, EvTraceResponse> {};

    struct TEvSignalBodyRequest : TEventPB<TEvSignalBodyRequest, NKikimrWhiteboard::TEvSignalBodyRequest, EvSignalBodyRequest> {};
    struct TEvSignalBodyResponse : TEventPB<TEvSignalBodyResponse, NKikimrWhiteboard::TEvSignalBodyResponse, EvSignalBodyResponse> {};
};

inline TActorId MakeNodeWhiteboardServiceId(ui32 node) {
    char x[12] = {'n','o','d','e','w','h','i','t','e','b','o','a'};
    return TActorId(node, TStringBuf(x, 12));
}

IActor* CreateNodeWhiteboardService();

} // NTabletState
} // NKikimr
