#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <ydb/core/base/subdomain.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TConfigureParts: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TInitializeBuildIndex TConfigureParts"
            << " operationId#" << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType});
    }

    bool HandleReply(TEvDataShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvProposeTransactionResult"
                               << " at tabletId# " << ssId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " HandleReply TEvProposeTransactionResult"
                                << " message: " << ev->Get()->Record.ShortDebugString());

        return NTableState::CollectProposeTransactionResults(OperationId, ev, context);
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << " at tabletId# " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState->TxType == TTxState::TxInitializeBuildIndex);

        TPathId pathId = txState->TargetPathId;
        Y_VERIFY(context.SS->PathsById.contains(pathId));
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);

        Y_VERIFY(context.SS->Tables.contains(pathId));
        const TTableInfo::TPtr tableInfo = context.SS->Tables.at(pathId);

        NKikimrTxDataShard::TFlatSchemeTransaction txTemplate;
        auto initiate = txTemplate.MutableInitiateBuildIndex();
        PathIdFromPathId(pathId, initiate->MutablePathId());
        initiate->SetSnapshotName("Snapshot0");
        initiate->SetTableSchemaVersion(tableInfo->AlterVersion + 1);

        bool found = false;
        for (const auto& [childName, childPathId] : path->GetChildren()) {
            Y_VERIFY(context.SS->PathsById.contains(childPathId));
            auto childPath = context.SS->PathsById.at(childPathId);

            if (!childPath->IsTableIndex() || childPath->Dropped() || childPath->PlannedToDrop()) {
                continue;
            }

            Y_VERIFY(context.SS->Indexes.contains(childPathId));
            auto index = context.SS->Indexes.at(childPathId);

            if (index->State != TTableIndexInfo::EState::EIndexStateInvalid) {
                // doesn't exist yet so its state is invalid
                continue;
            }

            Y_VERIFY_S(!found, "Too many indexes are planned to create"
                << ": found# " << TPathId(initiate->GetIndexDescription().GetPathOwnerId(),
                    initiate->GetIndexDescription().GetLocalPathId())
                << ", another# " << childPathId);
            found = true;

            Y_VERIFY(index->AlterData);
            context.SS->DescribeTableIndex(childPathId, childName, index->AlterData, *initiate->MutableIndexDescription());
        }

        txState->ClearShardsInProgress();

        for (ui32 i = 0; i < txState->Shards.size(); ++i) {
            TShardIdx shardIdx = txState->Shards[i].Idx;
            TTabletId datashardId = context.SS->ShardInfos[shardIdx].TabletID;

            auto seqNo = context.SS->StartRound(*txState);

            NKikimrTxDataShard::TFlatSchemeTransaction tx(txTemplate);
            context.SS->FillSeqNo(tx, seqNo);

            TString txBody;
            Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&txBody);

            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        DebugHint() << " ProgressState"
                                    << " SEND TFlatSchemeTransaction to datashard: " << datashardId
                                    << " with create snapshot request"
                                    << " operationId: " << OperationId
                                    << " seqNo: " << seqNo
                                    << " at schemeshard: " << ssId);


            THolder<TEvDataShard::TEvProposeTransaction> event =
                THolder(new TEvDataShard::TEvProposeTransaction(NKikimrTxDataShard::TX_KIND_SCHEME,
                                                        context.SS->TabletID(),
                                                        context.Ctx.SelfID,
                                                        ui64(OperationId.GetTxId()),
                                                        txBody,
                                                        context.SS->SelectProcessingPrarams(txState->TargetPathId)));

            context.OnComplete.BindMsgToPipe(OperationId, datashardId, shardIdx,  event.Release());
        }

        txState->UpdateShardsInProgress();
        return false;
    }
};


class TPropose: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TInitializeBuildIndex TPropose"
            << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType, TEvDataShard::TEvProposeTransactionResult::EventType});
    }

    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();
        const auto& evRecord = ev->Get()->Record;

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvSchemaChanged"
                               << " at tablet: " << ssId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " HandleReply TEvSchemaChanged"
                                << " triggered early"
                                << ", message: " << evRecord.ShortDebugString());

        NTableState::CollectSchemaChanged(OperationId, ev, context);
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << " at tablet: " << ssId
                               << ", stepId: " << step);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState->TxType == TTxState::TxInitializeBuildIndex);

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->SnapshotsStepIds[OperationId.GetTxId()] = step;
        context.SS->PersistSnapshotStepId(db, OperationId.GetTxId(), step);

        const TTableInfo::TPtr tableInfo = context.SS->Tables.at(txState->TargetPathId);
        tableInfo->AlterVersion += 1;
        context.SS->PersistTableAlterVersion(db, txState->TargetPathId, tableInfo);

        auto tablePath = context.SS->PathsById.at(txState->TargetPathId);
        context.SS->ClearDescribePathCaches(tablePath);
        context.OnComplete.PublishToSchemeBoard(OperationId, tablePath->PathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply ProgressState"
                               << " at tablet: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxInitializeBuildIndex);

        TSet<TTabletId> shardSet;
        for (const auto& shard : txState->Shards) {
            TShardIdx idx = shard.Idx;
            TTabletId tablet = context.SS->ShardInfos.at(idx).TabletID;
            shardSet.insert(tablet);
        }

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, txState->MinStep, shardSet);
        return false;
    }
};



class TCreateTxShards: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TInitializeBuildIndex TCreateTxShards"
            << " operationId: " << OperationId;
    }

public:
    TCreateTxShards(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", operation type: " << TTxState::TypeName(txState->TxType)
                               << ", at tablet" << ssId);

        if (NTableState::CheckPartitioningChangedForTableModification(*txState, context)) {
            LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                       DebugHint() << " ProgressState"
                                   << " SourceTablePartitioningChangedForModification"
                                   << ", tx type: " << TTxState::TypeName(txState->TxType));
            NTableState::UpdatePartitioningForTableModification(OperationId, *txState, context);
        }


        NIceDb::TNiceDb db(context.GetDB());

        context.SS->ChangeTxState(db, OperationId, TTxState::ConfigureParts);

        return true;
    }
};


class TInitializeBuildIndex: public TSubOperation {
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
            return TTxState::ProposedWaitParts;
        case TTxState::ProposedWaitParts:
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
            return THolder(new TCreateTxShards(OperationId));
        case TTxState::ConfigureParts:
            return THolder(new TConfigureParts(OperationId));
        case TTxState::Propose:
            return THolder(new TPropose(OperationId));
        case TTxState::ProposedWaitParts:
            return THolder(new NTableState::TProposedWaitParts(OperationId));
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
    TInitializeBuildIndex(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
          , Transaction(tx)
    {
    }

    TInitializeBuildIndex(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
          , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        auto schema = Transaction.GetInitiateBuildIndexMainTable();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& tableName = schema.GetTableName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TInitializeBuildIndex Propose"
                         << ", path: " << parentPathStr << "/" << tableName
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId));

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

        NSchemeShard::TPath dstPath = parentPath.Child(tableName);
        {
            NSchemeShard::TPath::TChecker checks = dstPath.Check();
            checks
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotUnderDeleting()
                .NotUnderOperation()
                .IsCommonSensePath()
                .IsTable();

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

        TString errStr;

        TPathElement::TPtr pathEl = dstPath.Base();
        TPathId tablePathId = pathEl->PathId;
        result->SetPathId(tablePathId.LocalPathId);

        if (!context.SS->CheckLocks(dstPath.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        if (context.SS->TablesWithSnaphots.contains(tablePathId)) {
            TTxId shapshotTxId = context.SS->TablesWithSnaphots.at(tablePathId);
            if (OperationId.GetTxId() == shapshotTxId) {
                // already
                errStr = TStringBuilder()
                    << "Snapshot with the same txId already presents for table"
                    << ", tableId:" << tablePathId
                    << ", txId: " << OperationId.GetTxId()
                    << ", snapshotTxId: " << shapshotTxId
                    << ", snapshotStepId: " << context.SS->SnapshotsStepIds.at(shapshotTxId);
                result->SetError(TEvSchemeShard::EStatus::StatusAlreadyExists, errStr);
                return result;
            }

            errStr = TStringBuilder()
                << "Snapshot with another txId already presents for table, only one snapshot is allowed for table for now"
                << ", tableId:" << tablePathId
                << ", txId: " << OperationId.GetTxId()
                << ", snapshotTxId: " << shapshotTxId
                << ", snapshotStepId: " << context.SS->SnapshotsStepIds.at(shapshotTxId);
            result->SetError(TEvSchemeShard::EStatus::StatusSchemeError, errStr);
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxInitializeBuildIndex, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        NIceDb::TNiceDb db(context.GetDB());

        pathEl->LastTxId = OperationId.GetTxId();
        pathEl->PathState = NKikimrSchemeOp::EPathState::EPathStateAlter;

        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxInitializeBuildIndex, tablePathId);
        txState.State = TTxState::CreateParts;
        context.SS->PersistTxState(db, OperationId);

        TTableInfo::TPtr table = context.SS->Tables.at(tablePathId);
        for (auto splitTx: table->GetSplitOpsInFlight()) {
            context.OnComplete.Dependence(splitTx.GetTxId(), OperationId.GetTxId());
        }

        context.SS->TablesWithSnaphots.emplace(tablePathId, OperationId.GetTxId());
        context.SS->SnapshotTables[OperationId.GetTxId()].insert(tablePathId);
        context.SS->PersistSnapshotTable(db, OperationId.GetTxId(), tablePathId);
        context.SS->TabletCounters->Simple()[COUNTER_SNAPSHOTS_COUNT].Add(1);

        context.OnComplete.ActivateTx(OperationId);

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TInitializeBuildIndex");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TInitializeBuildIndex AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateInitializeBuildIndexMainTable(TOperationId id, const TTxTransaction& tx) {
    return new TInitializeBuildIndex(id, tx);
}

ISubOperationBase::TPtr CreateInitializeBuildIndexMainTable(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TInitializeBuildIndex(id, state);
}

}
}
