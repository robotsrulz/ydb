#pragma once

#include "schemeshard__operation_part.h"
#include "schemeshard_tx_infly.h"

#include <util/generic/set.h>

namespace NKikimr {
namespace NSchemeShard {

struct TOperation: TSimpleRefCount<TOperation> {
    using TPtr = TIntrusivePtr<TOperation>;

    TTxId TxId;
    TVector<ISubOperationBase::TPtr> Parts;

    THashSet<TActorId> Subscribers;
    THashSet<TTxId> DependentOperations;
    THashSet<TTxId> WaitOperations;

    using TPreSerialisedMessage = std::pair<ui32, TIntrusivePtr<TEventSerializedData>>; // ui32 it's a type
    THashMap<TTabletId, TMap<TPipeMessageId, TPreSerialisedMessage>> PipeBindedMessages; // std::pair<ui64, ui64> it's a cookie

    THashMap<TTabletId, TSubTxId> RelationsByTabletId;
    THashMap<TShardIdx, TSubTxId> RelationsByShardIdx;

    TSet<TSubTxId> ReadyToProposeParts;
    using TProposeRec = std::tuple<TSubTxId, TPathId, TStepId>;
    TDeque<TProposeRec> Proposes;

    using TProposeShards = std::tuple<TSubTxId, TTabletId>;
    TDeque<TProposeShards> ShardsProposes;

    using TPublishPath = std::pair<TPathId, ui64>;
    TSet<TPublishPath> Publications;

    THashSet<TSubTxId> ReadyToNotifyParts;
    THashSet<TSubTxId> DoneParts;
    THashMap<TPathId, NKikimrSchemeOp::EPathState> ReleasePathAtDone;

    THashMap<TShardIdx, THashSet<TSubTxId>> WaitingShardCreatedByShard;
    THashMap<TSubTxId, THashSet<TShardIdx>> WaitingShardCreatedByPart;

    TMap<TSubTxId, TSet<TPublishPath>> WaitingPublicationsByPart;
    TMap<TPublishPath, TSet<TSubTxId>> WaitingPublicationsByPath;

    TMap<TString, TSet<TSubTxId>> Barriers;

    struct TConsumeQuotaResult {
        NKikimrScheme::EStatus Status = NKikimrScheme::StatusSuccess;
        TString Reason;
    };

    struct TSplitTransactionsResult {
        NKikimrScheme::EStatus Status = NKikimrScheme::StatusSuccess;
        TString Reason;
        TVector<TTxTransaction> Transactions;
    };

    TOperation(TTxId txId)
        : TxId(txId)
    {}
    ~TOperation() = default;

    TTxId GetTxId() const { return TxId; }

    static TConsumeQuotaResult ConsumeQuota(const TTxTransaction& tx, TOperationContext& context);

    static TSplitTransactionsResult SplitIntoTransactions(const TTxTransaction& tx, const TOperationContext& context);

    ISubOperationBase::TPtr RestorePart(TTxState::ETxType opType, TTxState::ETxState opState);

    ISubOperationBase::TPtr ConstructPart(NKikimrSchemeOp::EOperationType opType, const TTxTransaction& tx);
    TVector<ISubOperationBase::TPtr> ConstructParts(const TTxTransaction& tx, TOperationContext& context);
    void AddPart(ISubOperationBase::TPtr part) { Parts.push_back(part);}

    bool AddPublishingPath(TPathId pathId, ui64 version);
    bool IsPublished() const;

    void ReadyToNotifyPart(TSubTxId partId);
    bool IsReadyToNotify(const TActorContext& ctx) const;
    bool IsReadyToNotify() const;
    void AddNotifySubscriber(const TActorId& actorId);
    void DoNotify(TSchemeShard* ss, TSideEffects& sideEffects, const TActorContext& ctx);

    bool IsReadyToDone(const TActorContext& ctx) const;

    //propose operation to coordinator
    bool IsReadyToPropose(const TActorContext& ctx) const;
    bool IsReadyToPropose() const;
    void ProposePart(TSubTxId partId, TPathId pathId, TStepId minStep);
    void ProposePart(TSubTxId partId, TTabletId tableId);
    void DoPropose(TSchemeShard* ss, TSideEffects& sideEffects, const TActorContext& ctx) const;

    //route incomming messages to the parts
    void RegisterRelationByTabletId(TSubTxId partId, TTabletId tablet, const TActorContext& ctx);
    void RegisterRelationByShardIdx(TSubTxId partId, TShardIdx shardIdx, const TActorContext& ctx);
    TSubTxId FindRelatedPartByTabletId(TTabletId tablet, const TActorContext& ctx);
    TSubTxId FindRelatedPartByShardIdx(TShardIdx shardIdx, const TActorContext& ctx);

    void WaitShardCreated(TShardIdx shardIdx, TSubTxId partId);
    TVector<TSubTxId> ActivateShardCreated(TShardIdx shardIdx);

    void RegisterWaitPublication(TSubTxId partId, TPathId pathId, ui64 pathVersion);
    TSet<TOperationId> ActivatePartsWaitPublication(TPathId pathId, ui64 pathVersion);
    ui64 CountWaitPublication(TOperationId opId);

    void RegisterBarrier(TSubTxId partId, TString name) {
        Barriers[name].insert(partId);
        Y_VERIFY(Barriers.size() == 1);
    }

    bool HasBarrier() {
        Y_VERIFY(Barriers.size() <= 1);
        return Barriers.size() == 1;
    }

    bool IsDoneBarrier() {
        Y_VERIFY(Barriers.size() <= 1);
        for (const auto& item: Barriers) {
            for (TSubTxId blocked: item.second) {
                Y_VERIFY_S(!DoneParts.contains(blocked), "part is blocked and done: " << blocked);
            }
            return item.second.size() + DoneParts.size() == Parts.size();
        }

        return false;
    }

    void DropBarrier(TString name) {
        Y_VERIFY(IsDoneBarrier());
        Y_VERIFY(Barriers.begin()->first == name);
        Barriers.erase(name);
    }
    TOperationId NextPartId() { return TOperationId(TxId, TSubTxId(Parts.size())); }
};

inline TOperationId NextPartId(const TOperationId& opId, const TVector<ISubOperationBase::TPtr>& parts) {
    return TOperationId(opId.GetTxId(), opId.GetSubTxId() + parts.size());
}

}
}
