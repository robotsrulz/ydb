#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/persqueue/config/config.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

bool ValidateConfig(const NKikimrSchemeOp::TCreateSolomonVolume& op,
                                       TEvSchemeShard::EStatus& status, TString& errStr)
{
    if (op.GetPartitionCount() && op.AdoptedPartitionsSize()) {
        errStr = "mutable exclusive parametrs PartitionCount and AdoptedPartitions are set";
        status = TEvSchemeShard::EStatus::StatusInvalidParameter;
        return false;
    }
    if (op.GetPartitionCount()) {
        if (!op.HasChannelProfileId()) {
            errStr = "set channel profile id, please";
            status = TEvSchemeShard::EStatus::StatusInvalidParameter;
        }
        return true;
    }

    if (op.HasChannelProfileId()) {
        errStr = "don't set channel profile id, please. We are going to adopt already created tablets";
        status = TEvSchemeShard::EStatus::StatusInvalidParameter;
    }

    // check unique
    TSet<ui64> tabletIds;
    TSet<std::pair<ui64, ui64>> owners;
    for (auto& portion: op.GetAdoptedPartitions()) {
        if (tabletIds.contains(portion.GetTabletId())) {
            errStr = "Dublicate tabletsId in AdoptedPartitions "
                    + ToString(portion.GetTabletId());
            status = TEvSchemeShard::EStatus::StatusInvalidParameter;
            return false;
        }
        tabletIds.insert(portion.GetTabletId());

        auto owner = std::make_pair<ui64, ui64>(portion.GetOwnerId(), portion.GetShardIdx());
        if (owners.contains(owner)) {
            errStr = "Dublicate pair owner and shard in AdoptedPartitions "
                    + ToString(owner.first) + " " +  ToString(owner.second);
            status = TEvSchemeShard::EStatus::StatusInvalidParameter;
            return false;
        }
        owners.insert(owner);
    }

    return true;
}

TSolomonVolumeInfo::TPtr CreateSolomon(const NKikimrSchemeOp::TCreateSolomonVolume& op, TTxState& state, TSchemeShard* ss)
{
    TSolomonVolumeInfo::TPtr solomonVolume = new TSolomonVolumeInfo(1);

    state.Shards.clear();
    solomonVolume->Partitions.clear();

    ui64 newParts = op.GetPartitionCount();
    ui64 adoptedParts = op.AdoptedPartitionsSize();

    ui64 count = newParts + adoptedParts;

    state.Shards.reserve(count);
    auto startShardIdx = ss->ReserveShardIdxs(newParts);
    for (ui64 i = 0; i < newParts; ++i) {
        const auto idx = ss->NextShardIdx(startShardIdx, i);
        solomonVolume->Partitions[idx] = new TSolomonPartitionInfo(i);
        state.Shards.emplace_back(idx, TTabletTypes::KeyValue, TTxState::CreateParts);
    }

    startShardIdx = ss->ReserveShardIdxs(adoptedParts);
    for (ui64 i = 0; i < adoptedParts; ++i) {
        const auto idx = ss->NextShardIdx(startShardIdx, i);
        solomonVolume->Partitions[idx] = new TSolomonPartitionInfo(newParts + i, TTabletId(op.GetAdoptedPartitions(i).GetTabletId()));
        state.Shards.emplace_back(idx, TTabletTypes::KeyValue, TTxState::CreateParts);
    }

    return solomonVolume;
}

class TConfigureParts: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
                << "TCreateSolomon TConfigureParts"
                << ", operationId: " << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(),
            {TEvHive::TEvCreateTabletReply::EventType, TEvHive::TEvAdoptTabletReply::EventType});
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at tablet" << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateSolomonVolume);

        auto solomonVol = context.SS->SolomonVolumes[txState->TargetPathId];
        Y_VERIFY_S(solomonVol, "solomon volume is null. PathId: " << txState->TargetPathId);
        Y_VERIFY(solomonVol->Partitions.size() == txState->Shards.size(),
                 "%" PRIu64 "solomon shards expected, %" PRIu64 " created",
                 solomonVol->Partitions.size(), txState->Shards.size());

        for (const auto& shard: txState->Shards) {
            auto solomonPartition = solomonVol->Partitions[shard.Idx];
            Y_VERIFY_S(solomonPartition, "rtmr partitions is null shard idx: " << shard.Idx << " Path: " << txState->TargetPathId);

            auto tabletId = context.SS->ShardInfos[shard.Idx].TabletID;
            solomonPartition->TabletId = tabletId;
        }

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
        return true;
    }
};

class TPropose: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
                << "TCreateSolomon TPropose"
                << ", operationId: " << OperationId;
    }
public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(),
            {TEvHive::TEvCreateTabletReply::EventType, TEvHive::TEvAdoptTabletReply::EventType});
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << ", step: " << step
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        if(!txState) {
            return false;
        }

        TPathId pathId = txState->TargetPathId;
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);

        NIceDb::TNiceDb db(context.GetDB());

        path->StepCreated = step;
        context.SS->PersistCreateStep(db, pathId, step);

        IncParentDirAlterVersionWithRepublish(OperationId, TPath::Init(pathId, context.SS), context);

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateSolomonVolume);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }
};

class TCreateSolomon: public TSubOperation {
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

    TTxState::ETxState NextState() {
        return TTxState::CreateParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return TTxState::ConfigureParts;
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
        return TTxState::Invalid;
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return THolder(new TCreateParts(OperationId));
        case TTxState::ConfigureParts:
            return THolder(new TConfigureParts(OperationId));
        case TTxState::Propose:
            return THolder(new TPropose(OperationId));
        case TTxState::Done:
            return THolder(new TDone(OperationId));
        default:
            return nullptr;
        }
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

public:
    TCreateSolomon(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TCreateSolomon(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto acceptExisted = !Transaction.GetFailOnExist();
        const auto& solomonDescription = Transaction.GetCreateSolomonVolume();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = solomonDescription.GetName();
        const ui32 channelProfileId = solomonDescription.GetChannelProfileId();

        const ui64 shardsToCreate = solomonDescription.GetPartitionCount() + solomonDescription.AdoptedPartitionsSize();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateSolomon Propose"
                         << ", path: "<< parentPathStr << "/" << name
                         << ", opId: " << OperationId
                         << ", channelProfileId: " << channelProfileId
                         << ", at schemeshard: " << ssId);

        THolder<TProposeResponse> result;
        result.Reset(new TEvSchemeShard::TEvModifySchemeTransactionResult(
            NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId)));

        TEvSchemeShard::EStatus status = NKikimrScheme::StatusAccepted;
        TString errStr;

        NSchemeShard::TPath parentPath = NSchemeShard::TPath::Resolve(parentPathStr, context.SS);
        {
            NSchemeShard::TPath::TChecker checks = parentPath.Check();
            checks
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsCommonSensePath()
                .IsLikeDirectory();

            if (!checks) {
                TString explain = TStringBuilder() << "parent path fail checks"
                                                   << ", path: " << parentPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        const TString acl = Transaction.GetModifyACL().GetDiffACL();

        NSchemeShard::TPath dstPath = parentPath.Child(name);
        {
            NSchemeShard::TPath::TChecker checks = dstPath.Check();
            checks.IsAtLocalSchemeShard();
            if (dstPath.IsResolved()) {
                checks
                    .IsResolved()
                    .NotUnderDeleting()
                    .FailOnExist(TPathElement::EPathType::EPathTypeSolomonVolume, acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .IsValidLeafName()
                    .DepthLimit()
                    .PathsLimit()
                    .DirChildrenLimit()
                    .ShardsLimit(shardsToCreate)
                    .PathShardsLimit(shardsToCreate)
                    .IsValidACL(acl);
            }

            if (!checks) {
                TString explain = TStringBuilder() << "dst path fail checks"
                                                   << ", path: " << dstPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (dstPath.IsResolved()) {
                    result->SetPathCreateTxId(ui64(dstPath.Base()->CreateTxId));
                    result->SetPathId(dstPath.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        if (!ValidateConfig(solomonDescription, status, errStr)) {
            result->SetError(status, errStr);
            return result;
        }

        const bool adoptingTablets = solomonDescription.AdoptedPartitionsSize() > 0;

        TChannelsBindings channelsBinding;
        if (!adoptingTablets && !context.SS->ResolveSolomonChannels(channelProfileId, dstPath.GetPathIdForDomain(), channelsBinding)) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "Unable to construct channel binding with the storage pool");
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxCreateSolomonVolume, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        dstPath.MaterializeLeaf(owner);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);

        TPathElement::TPtr newSolomon = dstPath.Base();
        newSolomon->CreateTxId = OperationId.GetTxId();
        newSolomon->LastTxId = OperationId.GetTxId();
        newSolomon->PathState = TPathElement::EPathState::EPathStateCreate;
        newSolomon->PathType = TPathElement::EPathType::EPathTypeSolomonVolume;


        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCreateSolomonVolume, newSolomon->PathId);

        auto solomonVolume = CreateSolomon(solomonDescription, txState, context.SS);
        if (!solomonVolume.Get()) {
            result->SetError(status, errStr);
            return result;
        }

        context.SS->SolomonVolumes[newSolomon->PathId] = solomonVolume;
        context.SS->TabletCounters->Simple()[COUNTER_SOLOMON_VOLUME_COUNT].Add(1);
        context.SS->TabletCounters->Simple()[COUNTER_SOLOMON_PARTITIONS_COUNT].Add(solomonVolume->Partitions.size());
        context.SS->IncrementPathDbRefCount(newSolomon->PathId);

        TShardInfo solomonPartitionInfo = TShardInfo::SolomonPartitionInfo(OperationId.GetTxId(), newSolomon->PathId);
        solomonPartitionInfo.BindedChannels = channelsBinding;

        TMap<TTabletId, TAdoptedShard> adoptedShards;
        for (auto& portion: solomonDescription.GetAdoptedPartitions()) {
            adoptedShards[TTabletId(portion.GetTabletId())] = TAdoptedShard{portion.GetOwnerId(), TLocalShardIdx(portion.GetShardIdx())};
        }

        NIceDb::TNiceDb db(context.GetDB());

        for (const auto& part: solomonVolume->Partitions) {
            TShardIdx shardIdx = part.first;
            context.SS->RegisterShardInfo(shardIdx, solomonPartitionInfo);

            if (part.second->TabletId != InvalidTabletId) {
                auto tabletId = part.second->TabletId;
                auto& shard = adoptedShards[tabletId];
                context.SS->AdoptedShards[shardIdx] = shard;
                context.SS->PersistAdoptedShardMapping(db, shardIdx, tabletId, shard.PrevOwner, shard.PrevShardIdx);
                context.SS->ShardInfos[shardIdx].TabletID = tabletId;
            }

            context.SS->PersistShardMapping(db, shardIdx, part.second->TabletId, newSolomon->PathId, OperationId.GetTxId(), solomonPartitionInfo.TabletType);
            context.SS->PersistChannelsBinding(db, shardIdx, channelsBinding);
        }
        context.SS->PersistSolomonVolume(db, newSolomon->PathId, solomonVolume);

        if (parentPath.Base()->HasActiveChanges()) {
            TTxId parentTxId = parentPath.Base()->PlannedToCreate() ? parentPath.Base()->CreateTxId : parentPath.Base()->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }

        context.SS->ChangeTxState(db, OperationId, TTxState::CreateParts);
        context.OnComplete.ActivateTx(OperationId);

        context.SS->PersistTxState(db, OperationId);

        context.SS->PersistPath(db, newSolomon->PathId);

        if (!acl.empty()) {
            newSolomon->ApplyACL(acl);
            context.SS->PersistACL(db, newSolomon);
        }

        context.SS->PersistUpdateNextPathId(db);
        context.SS->PersistUpdateNextShardIdx(db);

        IncParentDirAlterVersionWithRepublish(OperationId, dstPath, context);

        Y_VERIFY(shardsToCreate == txState.Shards.size());
        dstPath.DomainInfo()->IncPathsInside();
        dstPath.DomainInfo()->AddInternalShards(txState);

        dstPath.Base()->IncShardsInside(shardsToCreate);
        parentPath.Base()->IncAliveChildren();

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TCreateSolomon");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateSolomon AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateNewSolomon(TOperationId id, const TTxTransaction& tx) {
    return new TCreateSolomon(id, tx);
}

ISubOperationBase::TPtr CreateNewSolomon(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TCreateSolomon(id, state);
}

}
}
