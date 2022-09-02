#include "agent_impl.h"
#include "blocks.h"

namespace NKikimr::NBlobDepot {

    void TBlobDepotAgent::Handle(TEvTabletPipe::TEvClientConnected::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA03, "TEvClientConnected", (VirtualGroupId, VirtualGroupId),
            (Msg, ev->Get()->ToString()));
    }

    void TBlobDepotAgent::Handle(TEvTabletPipe::TEvClientDestroyed::TPtr ev) {
        STLOG(PRI_INFO, BLOB_DEPOT_AGENT, BDA04, "TEvClientDestroyed", (VirtualGroupId, VirtualGroupId),
            (Msg, ev->Get()->ToString()));
        PipeId = {};
        OnDisconnect();
        ConnectToBlobDepot();
    }

    void TBlobDepotAgent::ConnectToBlobDepot() {
        PipeId = Register(NTabletPipe::CreateClient(SelfId(), TabletId, NTabletPipe::TClientRetryPolicy::WithRetries()));
        const ui64 id = NextRequestId++;
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA05, "ConnectToBlobDepot", (VirtualGroupId, VirtualGroupId),
            (PipeId, PipeId), (RequestId, id));
        NTabletPipe::SendData(SelfId(), PipeId, new TEvBlobDepot::TEvRegisterAgent(VirtualGroupId, AgentInstanceId), id);
        RegisterRequest(id, this, nullptr, {}, true);
    }

    void TBlobDepotAgent::Handle(TRequestContext::TPtr /*context*/, NKikimrBlobDepot::TEvRegisterAgentResult& msg) {
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA06, "TEvRegisterAgentResult", (VirtualGroupId, VirtualGroupId),
            (Msg, msg));
        BlobDepotGeneration = msg.GetGeneration();
        DecommitGroupId = msg.HasDecommitGroupId() ? std::make_optional(msg.GetDecommitGroupId()) : std::nullopt;

        THashSet<NKikimrBlobDepot::TChannelKind::E> vanishedKinds;
        for (const auto& [kind, _] : ChannelKinds) {
            vanishedKinds.insert(kind);
        }

        ChannelToKind.clear();

        for (const auto& ch : msg.GetChannelKinds()) {
            const NKikimrBlobDepot::TChannelKind::E kind = ch.GetChannelKind();
            vanishedKinds.erase(kind);

            auto [it, inserted] = ChannelKinds.try_emplace(kind, kind);
            auto& v = it->second;

            v.ChannelToIndex.fill(0);
            v.ChannelGroups.clear();

            for (const auto& channelGroup : ch.GetChannelGroups()) {
                const ui8 channel = channelGroup.GetChannel();
                const ui32 groupId = channelGroup.GetGroupId();
                v.ChannelToIndex[channel] = v.ChannelGroups.size();
                v.ChannelGroups.emplace_back(channel, groupId);
                ChannelToKind[channel] = &v;
            }
        }

        for (const NKikimrBlobDepot::TChannelKind::E kind : vanishedKinds) {
            STLOG(PRI_INFO, BLOB_DEPOT_AGENT, BDA07, "kind vanished", (VirtualGroupId, VirtualGroupId), (Kind, kind));
            ChannelKinds.erase(kind);
        }

        for (const auto& [channel, kind] : ChannelToKind) {
            kind->Trim(channel, BlobDepotGeneration - 1, Max<ui32>());

            auto& wif = kind->WritesInFlight;
            const TBlobSeqId min{channel, 0, 0, 0};
            const TBlobSeqId max{channel, BlobDepotGeneration - 1, Max<ui32>(), TBlobSeqId::MaxIndex};
            wif.erase(wif.lower_bound(min), wif.upper_bound(max));
        }

        for (auto& [_, kind] : ChannelKinds) {
            IssueAllocateIdsIfNeeded(kind);
        }
    }

    void TBlobDepotAgent::IssueAllocateIdsIfNeeded(TChannelKind& kind) {
        if (!kind.IdAllocInFlight && kind.GetNumAvailableItems() < 100 && PipeId) {
            const ui64 id = NextRequestId++;
            STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA08, "IssueAllocateIdsIfNeeded", (VirtualGroupId, VirtualGroupId),
                (ChannelKind, NKikimrBlobDepot::TChannelKind::E_Name(kind.Kind)),
                (IdAllocInFlight, kind.IdAllocInFlight), (NumAvailableItems, kind.GetNumAvailableItems()),
                (RequestId, id));
            NTabletPipe::SendData(SelfId(), PipeId, new TEvBlobDepot::TEvAllocateIds(kind.Kind, 100), id);
            RegisterRequest(id, this, std::make_shared<TAllocateIdsContext>(kind.Kind), {}, true);
            kind.IdAllocInFlight = true;
        }
    }

    void TBlobDepotAgent::Handle(TRequestContext::TPtr context, NKikimrBlobDepot::TEvAllocateIdsResult& msg) {
        auto& allocateIdsContext = context->Obtain<TAllocateIdsContext>();
        const auto it = ChannelKinds.find(allocateIdsContext.ChannelKind);
        Y_VERIFY_S(it != ChannelKinds.end(), "Kind# " << NKikimrBlobDepot::TChannelKind::E_Name(allocateIdsContext.ChannelKind)
            << " Msg# " << SingleLineProto(msg));
        auto& kind = it->second;

        Y_VERIFY(kind.IdAllocInFlight);
        kind.IdAllocInFlight = false;

        Y_VERIFY(msg.GetChannelKind() == allocateIdsContext.ChannelKind);
        Y_VERIFY(msg.GetGeneration() == BlobDepotGeneration);

        if (msg.HasGivenIdRange()) {
            kind.IssueGivenIdRange(msg.GetGivenIdRange());
        }

        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA09, "TEvAllocateIdsResult", (VirtualGroupId, VirtualGroupId),
            (Msg, msg), (NumAvailableItems, kind.GetNumAvailableItems()));
    }

    void TBlobDepotAgent::OnDisconnect() {
        while (TabletRequestInFlight) {
            auto [id, request] = std::move(*TabletRequestInFlight.begin());
            request.Sender->OnRequestComplete(id, TTabletDisconnected{});
            TabletRequestInFlight.erase(id);
        }

        for (auto& [_, kind] : ChannelKinds) {
            kind.IdAllocInFlight = false;
        }
    }

    void TBlobDepotAgent::ProcessResponse(ui64 /*id*/, TRequestContext::TPtr context, TResponse response) {
        std::visit([&](auto&& response) {
            using T = std::decay_t<decltype(response)>;
            if constexpr (std::is_same_v<T, TEvBlobDepot::TEvRegisterAgentResult*>
                    || std::is_same_v<T, TEvBlobDepot::TEvAllocateIdsResult*>) {
                Handle(std::move(context), response->Record);
            } else if constexpr (!std::is_same_v<T, TTabletDisconnected>) {
                Y_FAIL_S("unexpected response received Type# " << TypeName<T>());
            }
        }, response);
    }

    void TBlobDepotAgent::Issue(NKikimrBlobDepot::TEvBlock msg, TRequestSender *sender, TRequestContext::TPtr context) {
        auto ev = std::make_unique<TEvBlobDepot::TEvBlock>();
        msg.Swap(&ev->Record);
        Issue(std::move(ev), sender, std::move(context));
    }

    void TBlobDepotAgent::Issue(NKikimrBlobDepot::TEvResolve msg, TRequestSender *sender, TRequestContext::TPtr context) {
        auto ev = std::make_unique<TEvBlobDepot::TEvResolve>();
        msg.Swap(&ev->Record);
        Issue(std::move(ev), sender, std::move(context));
    }

    void TBlobDepotAgent::Issue(NKikimrBlobDepot::TEvCommitBlobSeq msg, TRequestSender *sender, TRequestContext::TPtr context) {
        auto ev = std::make_unique<TEvBlobDepot::TEvCommitBlobSeq>();
        msg.Swap(&ev->Record);
        Issue(std::move(ev), sender, std::move(context));
    }

    void TBlobDepotAgent::Issue(std::unique_ptr<IEventBase> ev, TRequestSender *sender, TRequestContext::TPtr context) {
        const ui64 id = NextRequestId++;
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA10, "Issue", (VirtualGroupId, VirtualGroupId), (Id, id), (Msg, ev->ToString()));
        NTabletPipe::SendData(SelfId(), PipeId, ev.release(), id);
        RegisterRequest(id, sender, std::move(context), {}, true);
    }

    void TBlobDepotAgent::Handle(TEvBlobDepot::TEvPushNotify::TPtr ev) {
        auto response = std::make_unique<TEvBlobDepot::TEvPushNotifyResult>();

        auto& msg = ev->Get()->Record;

        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA11, "TEvPushNotify", (VirtualGroupId, VirtualGroupId), (Msg, msg),
            (Id, ev->Cookie));

        BlocksManager.OnBlockedTablets(msg.GetBlockedTablets());

        for (const auto& item : msg.GetInvalidatedSteps()) {
            const ui8 channel = item.GetChannel();
            Y_VERIFY(item.GetGeneration() == BlobDepotGeneration);
            const auto it = ChannelToKind.find(channel);
            Y_VERIFY(it != ChannelToKind.end());
            TChannelKind& kind = *it->second;
            const ui32 numAvailableItemsBefore = kind.GetNumAvailableItems();
            kind.Trim(channel, item.GetGeneration(), item.GetInvalidatedStep());

            // report writes in flight that are trimmed
            const TBlobSeqId first{channel, item.GetGeneration(), 0, 0};
            const TBlobSeqId last{channel, item.GetGeneration(), item.GetInvalidatedStep(), Max<ui32>()};
            for (auto it = kind.WritesInFlight.lower_bound(first); it != kind.WritesInFlight.end() && *it <= last; ++it) {
                it->ToProto(response->Record.AddWritesInFlight());
            }

            STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA12, "TrimChannel", (VirtualGroupId, VirtualGroupId),
                (Channel, int(channel)), (NumAvailableItemsBefore, numAvailableItemsBefore),
                (NumAvailableItemsAfter, kind.GetNumAvailableItems()));
        }

        // it is essential to send response through the pipe -- otherwise we can break order with, for example, commits:
        // this message can outrun previously sent commit and lead to data loss
        NTabletPipe::SendData(SelfId(), PipeId, response.release(), ev->Cookie);

        for (auto& [_, kind] : ChannelKinds) {
            IssueAllocateIdsIfNeeded(kind);
        }
    }

} // NKikimr::NBlobDepot
