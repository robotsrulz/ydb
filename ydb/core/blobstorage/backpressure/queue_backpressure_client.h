#pragma once

#include "defs.h"
#include "queue_backpressure_common.h"

namespace NKikimr {

    namespace NBackpressure {
        class TQueueClientId;
    } // NBackpressure

    struct TEvPruneQueue : public TEventLocal<TEvPruneQueue, TEvBlobStorage::EvPruneQueue>
    {};

    struct TEvProxyQueueState : public TEventLocal<TEvProxyQueueState, TEvBlobStorage::EvProxyQueueState> {
        TVDiskID VDiskId;
        NKikimrBlobStorage::EVDiskQueueId QueueId;
        bool IsConnected;
        bool ExtraBlockChecksSupport;

        TEvProxyQueueState(const TVDiskID &vDiskId, NKikimrBlobStorage::EVDiskQueueId queueId, bool isConnected,
                bool extraBlockChecksSupport)
            : VDiskId(vDiskId)
            , QueueId(queueId)
            , IsConnected(isConnected)
            , ExtraBlockChecksSupport(extraBlockChecksSupport)
        {}

        TString ToString() const {
            TStringStream str;
            str << "{VDiskId# " << VDiskId.ToString();
            str << " QueueId# " << static_cast<ui32>(QueueId);
            str << " IsConnected# " << (IsConnected ? "true" : "false");
            str << " ExtraBlockChecksSupport# " << (ExtraBlockChecksSupport ? "true" : "false");
            str << "}";
            return str.Str();
        }
    };

    struct TEvRequestProxyQueueState
        : public TEventLocal<TEvRequestProxyQueueState, TEvBlobStorage::EvRequestProxyQueueState>
    {};

    IActor* CreateVDiskBackpressureClient(const TIntrusivePtr<TBlobStorageGroupInfo>& info, TVDiskIdShort vdiskId,
        NKikimrBlobStorage::EVDiskQueueId queueId,const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters,
        const TBSProxyContextPtr& bspctx, const NBackpressure::TQueueClientId& clientId, const TString& queueName,
        ui32 interconnectChannel, bool local, TDuration watchdogTimeout,
        TIntrusivePtr<NBackpressure::TFlowRecord> &flowRecord, NMonitoring::TCountableBase::EVisibility visibility);

} // NKikimr
