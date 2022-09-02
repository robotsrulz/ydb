#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/tx/replication/controller/public_events.h>

#define LOG_D(stream) LOG_DEBUG_S (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_I(stream) LOG_INFO_S  (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_W(stream) LOG_WARN_S  (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)

namespace NKikimr {
namespace NSchemeShard {

namespace {

class TDropParts: public TSubOperationState {
    TString DebugHint() const override {
        return TStringBuilder()
            << "TDropReplication TDropParts"
            << " opId# " << OperationId << " ";
    }

public:
    explicit TDropParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState");

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropReplication);
        const auto& pathId = txState->TargetPathId;

        txState->ClearShardsInProgress();

        for (const auto& shard : txState->Shards) {
            Y_VERIFY(shard.TabletType == ETabletType::ReplicationController);

            Y_VERIFY(context.SS->ShardInfos.contains(shard.Idx));
            const auto tabletId = context.SS->ShardInfos.at(shard.Idx).TabletID;

            auto ev = MakeHolder<NReplication::TEvController::TEvDropReplication>();
            PathIdFromPathId(pathId, ev->Record.MutablePathId());
            ev->Record.MutableOperationId()->SetTxId(ui64(OperationId.GetTxId()));
            ev->Record.MutableOperationId()->SetPartId(ui32(OperationId.GetSubTxId()));

            LOG_D(DebugHint() << "Send TEvDropReplication to controller"
                << ": tabletId# " << tabletId
                << ", ev# " << ev->ToString());
            context.OnComplete.BindMsgToPipe(OperationId, tabletId, pathId, ev.Release());

            txState->ShardsInProgress.insert(shard.Idx);
        }

        return false;
    }

    bool HandleReply(NReplication::TEvController::TEvDropReplicationResult::TPtr& ev, TOperationContext& context) override {
        LOG_I(DebugHint() << "HandleReply " << ev->Get()->ToString());

        const auto tabletId = TTabletId(ev->Get()->Record.GetOrigin());
        const auto status = ev->Get()->Record.GetStatus();

        switch (status) {
        case NKikimrReplication::TEvDropReplicationResult::SUCCESS:
        case NKikimrReplication::TEvDropReplicationResult::NOT_FOUND:
            break;
        default:
            LOG_W(DebugHint() << "Ignoring unexpected TEvDropReplicationResult"
                << " tabletId# " << tabletId
                << " status# " << static_cast<int>(status));
            return false;
        }

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropReplication);
        Y_VERIFY(txState->State == TTxState::DropParts);

        const auto shardIdx = context.SS->MustGetShardIdx(tabletId);
        if (!txState->ShardsInProgress.erase(shardIdx)) {
            LOG_W(DebugHint() << "Ignoring duplicate TEvDropReplicationResult");
            return false;
        }

        context.OnComplete.UnbindMsgFromPipe(OperationId, tabletId, txState->TargetPathId);

        if (!txState->ShardsInProgress.empty()) {
            return false;
        }

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
        context.OnComplete.ActivateTx(OperationId);

        return true;
    }

private:
    const TOperationId OperationId;

}; // TDropParts

class TPropose: public TSubOperationState {
    TString DebugHint() const override {
        return TStringBuilder()
            << "TDropReplication TPropose"
            << " opId# " << OperationId << " ";
    }

public:
    explicit TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {
            NReplication::TEvController::TEvDropReplicationResult::EventType,
        });
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState");

        const auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropReplication);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const auto step = TStepId(ev->Get()->StepId);

        LOG_I(DebugHint() << "HandleReply TEvOperationPlan"
            << ": step# " << step);

        const auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropReplication);
        const auto& pathId = txState->TargetPathId;

        Y_VERIFY(context.SS->PathsById.contains(pathId));
        auto path = context.SS->PathsById.at(pathId);

        Y_VERIFY(context.SS->PathsById.contains(path->ParentPathId));
        auto parentPath = context.SS->PathsById.at(path->ParentPathId);

        NIceDb::TNiceDb db(context.GetDB());

        Y_VERIFY(!path->Dropped());
        path->SetDropped(step, OperationId.GetTxId());
        context.SS->PersistDropStep(db, pathId, step, OperationId);
        context.SS->PersistReplicationRemove(db, pathId);

        context.SS->TabletCounters->Simple()[COUNTER_USER_ATTRIBUTES_COUNT].Add(-path->UserAttrs->Size());
        context.SS->PersistUserAttributes(db, path->PathId, path->UserAttrs, nullptr);

        context.SS->TabletCounters->Simple()[COUNTER_REPLICATION_COUNT].Add(-1);
        context.SS->ResolveDomainInfo(pathId)->DecPathsInside();
        parentPath->DecAliveChildren();

        ++parentPath->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath);
        context.SS->ClearDescribePathCaches(parentPath);
        context.SS->ClearDescribePathCaches(path);

        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parentPath->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
        }

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

private:
   const TOperationId OperationId;

}; // TPropose

class TDropReplication: public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::DropParts;
    }

    static TTxState::ETxState NextState(TTxState::ETxState state) {
        switch (state) {
        case TTxState::DropParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch (state) {
        case TTxState::DropParts:
            return MakeHolder<TDropParts>(OperationId);
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
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
    explicit TDropReplication(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
        , State(TTxState::Invalid)
    {
    }

    explicit TDropReplication(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const auto& workingDir = Transaction.GetWorkingDir();
        const auto& op = Transaction.GetDrop();
        const auto& name = op.GetName();

        LOG_N("TDropReplication Propose"
            << ": opId# " << OperationId
            << ", path# " << workingDir << "/" << name);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(context.SS->SelfTabletId()));

        const auto path = op.HasId()
            ? TPath::Init(context.SS->MakeLocalId(op.GetId()), context.SS)
            : TPath::Resolve(workingDir, context.SS).Dive(name);
        {
            const auto checks = path.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .IsReplication()
                .NotDeleted()
                .NotUnderDeleting()
                .NotUnderOperation();

            if (!checks) {
                TString explain = TStringBuilder() << "path checks failed, path: " << path.PathString();
                const auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (path.IsResolved() && path->IsReplication() && (path->PlannedToDrop() || path->Dropped())) {
                    result->SetPathDropTxId(ui64(path->DropTxId));
                    result->SetPathId(path->PathId.LocalPathId);
                }

                return result;
            }
        }

        const auto parentPath = path.Parent();
        {
            const auto checks = parentPath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsCommonSensePath()
                .IsLikeDirectory();

            if (!checks) {
                TString explain = TStringBuilder() << "parent path checks failed, path: " << parentPath.PathString();
                const auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);

                return result;
            }
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxDropReplication, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        Y_VERIFY(context.SS->Replications.contains(path->PathId));
        auto replication = context.SS->Replications.at(path->PathId);
        Y_VERIFY(!replication->AlterData);

        Y_VERIFY(!context.SS->FindTx(OperationId));
        auto& txState = context.SS->CreateTx(OperationId, TTxState::TxDropReplication, path->PathId);
        txState.State = TTxState::DropParts;
        txState.MinStep = TStepId(1);

        for (const auto& shardIdx: path.DomainInfo()->GetReplicationControllers()) {
            Y_VERIFY_S(context.SS->ShardInfos.contains(shardIdx), "Unknown shardIdx " << shardIdx);
            const auto tabletType = context.SS->ShardInfos.at(shardIdx).TabletType;
            txState.Shards.emplace_back(shardIdx, tabletType, TTxState::DropParts);
        }

        path->PathState = TPathElement::EPathState::EPathStateDrop;
        path->DropTxId = OperationId.GetTxId();
        path->LastTxId = OperationId.GetTxId();

        NIceDb::TNiceDb db(context.GetDB());

        context.SS->PersistLastTxId(db, path.Base());
        context.SS->PersistTxState(db, OperationId);

        ++parentPath->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath.Base());
        context.SS->ClearDescribePathCaches(parentPath.Base());
        context.SS->ClearDescribePathCaches(path.Base());

        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parentPath->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, path->PathId);
        }

        context.OnComplete.ActivateTx(OperationId);

        State = NextState();
        SetState(SelectStateFunc(State));

        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TDropReplication");
    }

    void AbortUnsafe(TTxId txId, TOperationContext& context) override {
        LOG_N("TDropReplication AbortUnsafe"
            << ": opId# " << OperationId
            << ", txId# " << txId);
        context.OnComplete.DoneOperation(OperationId);
    }

private:
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State;

}; // TDropReplication

} // anonymous

ISubOperationBase::TPtr CreateDropReplication(TOperationId id, const TTxTransaction& tx) {
    return new TDropReplication(id, tx);
}

ISubOperationBase::TPtr CreateDropReplication(TOperationId id, TTxState::ETxState state) {
    return new TDropReplication(id, state);
}

} // NSchemeShard
} // NKikimr
