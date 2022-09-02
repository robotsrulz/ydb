#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_path_element.h"

#include "schemeshard_impl.h"

#include <ydb/core/base/path.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TConfigureParts: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TDropIndexAtMainTable TConfigureParts"
            << " operationId#" << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool HandleReply(TEvDataShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvProposeTransactionResult"
                               << " at tabletId# " << ssId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " HandleReply TEvProposeTransactionResult"
                                << " message# " << ev->Get()->Record.ShortDebugString());

        if (!NTableState::CollectProposeTransactionResults(OperationId, ev, context)) {
            return false;
        }

        return true;
    }


    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropTableIndexAtMainTable);

        //fill txShards
        if (NTableState::CheckPartitioningChangedForTableModification(*txState, context)) {
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        DebugHint() << " UpdatePartitioningForTableModification");
            NTableState::UpdatePartitioningForTableModification(OperationId, *txState, context);
        }

        txState->ClearShardsInProgress();

        TString txBody;
        {
            TPathId pathId = txState->TargetPathId;
            Y_VERIFY(context.SS->PathsById.contains(pathId));
            TPathElement::TPtr path = context.SS->PathsById.at(pathId);
            Y_VERIFY(path);

            Y_VERIFY(context.SS->Tables.contains(pathId));
            TTableInfo::TPtr table = context.SS->Tables.at(pathId);
            Y_VERIFY(table);

            auto seqNo = context.SS->StartRound(*txState);

            NKikimrTxDataShard::TFlatSchemeTransaction tx;
            context.SS->FillSeqNo(tx, seqNo);

            auto notice = tx.MutableDropIndexNotice();
            PathIdFromPathId(pathId, notice->MutablePathId());
            notice->SetTableSchemaVersion(table->AlterVersion + 1);

            bool found = false;
            for (const auto& [_, childPathId] : path->GetChildren()) {
                Y_VERIFY(context.SS->PathsById.contains(childPathId));
                auto childPath = context.SS->PathsById.at(childPathId);

                if (!childPath->IsTableIndex() || !childPath->PlannedToDrop()) {
                    continue;
                }

                Y_VERIFY_S(!found, "Too many indexes are planned to drop"
                    << ": found# " << PathIdFromPathId(notice->GetIndexPathId())
                    << ", another# " << childPathId);
                found = true;

                PathIdFromPathId(childPathId, notice->MutableIndexPathId());
            }

            Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&txBody);
        }

        Y_VERIFY(txState->Shards.size());
        for (ui32 i = 0; i < txState->Shards.size(); ++i) {
            auto idx = txState->Shards[i].Idx;
            auto datashardId = context.SS->ShardInfos[idx].TabletID;

            THolder<TEvDataShard::TEvProposeTransaction> event =
                THolder(new TEvDataShard::TEvProposeTransaction(NKikimrTxDataShard::TX_KIND_SCHEME,
                                                        context.SS->TabletID(),
                                                        context.Ctx.SelfID,
                                                        ui64(OperationId.GetTxId()),
                                                        txBody,
                                                        context.SS->SelectProcessingPrarams(txState->TargetPathId)));

            context.OnComplete.BindMsgToPipe(OperationId, datashardId, idx, event.Release());
        }

        txState->UpdateShardsInProgress(TTxState::ConfigureParts);
        return false;
    }
};

class TPropose: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TDropIndexAtMainTable TPropose"
            << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {TEvDataShard::TEvProposeTransactionResult::EventType});
    }

    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvDataShard::TEvSchemaChanged"
                               << " triggers early, save it"
                               << ", at schemeshard: " << ssId);

        NTableState::CollectSchemaChanged(OperationId, ev, context);
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << ", step: " << step
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState->TxType == TTxState::TxDropTableIndexAtMainTable);

        TPathId pathId = txState->TargetPathId;
        Y_VERIFY(context.SS->PathsById.contains(pathId));
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);

        Y_VERIFY(context.SS->Tables.contains(pathId));
        TTableInfo::TPtr table = context.SS->Tables.at(pathId);

        NIceDb::TNiceDb db(context.GetDB());

        table->AlterVersion += 1;
        context.SS->PersistTableAlterVersion(db, pathId, table);

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, path->PathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropTableIndexAtMainTable);

        TSet<TTabletId> shardSet;
        for (const auto& shard : txState->Shards) {
            TShardIdx idx = shard.Idx;
            Y_VERIFY(context.SS->ShardInfos.contains(idx));
            TTabletId tablet = context.SS->ShardInfos.at(idx).TabletID;
            shardSet.insert(tablet);
        }

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, txState->MinStep, shardSet);
        return false;
    }
};



class TDropIndexAtMainTable: public TSubOperation {
private:
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

    TTxState::ETxState NextState() {
        return TTxState::ConfigureParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
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
    TDropIndexAtMainTable(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TDropIndexAtMainTable(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        auto dropOperation = Transaction.GetDropIndex();

        const TString workingDir = Transaction.GetWorkingDir();
        const TString mainTableName = dropOperation.GetTableName();
        const TString indexName = dropOperation.GetIndexName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropIndexAtMainTable Propose"
                         << ", path: " << workingDir << "/" << mainTableName
                         << ", index name: " << indexName
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropIndexAtMainTable Propose"
                        << ", message: " << Transaction.ShortDebugString()
                        << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId));


        if (!dropOperation.HasIndexName() && !indexName) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "No index name present");
            return result;
        }

        TPath tablePath = TPath::Resolve(workingDir, context.SS).Dive(mainTableName);
        {
            TPath::TChecker checks = tablePath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsTable()
                .NotUnderOperation()
                .IsCommonSensePath();

            if (!checks) {
                TString explain = TStringBuilder() << "path fail checks"
                                                   << ", path: " << tablePath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        TString errStr;

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        if (!context.SS->CheckLocks(tablePath.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        TPath indexPath = tablePath.Child(indexName);
        {
            TPath::TChecker checks = indexPath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsTableIndex()
                .NotUnderOperation();

            if (!checks) {
                TString explain = TStringBuilder() << "path fail checks"
                                                   << ", path: " << indexPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxDropTableIndexAtMainTable, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        Y_VERIFY(context.SS->Tables.contains(tablePath.Base()->PathId));
        TTableInfo::TPtr table = context.SS->Tables.at(tablePath.Base()->PathId);

        Y_VERIFY(table->AlterVersion != 0);
        Y_VERIFY(!table->AlterData);

        Y_VERIFY(context.SS->Indexes.contains(indexPath.Base()->PathId));
        TTableIndexInfo::TPtr index = context.SS->Indexes.at(indexPath.Base()->PathId);

        Y_VERIFY(index->AlterVersion != 0);
        Y_VERIFY(!index->AlterData);

        Y_VERIFY(!context.SS->FindTx(OperationId));

        auto guard = context.DbGuard();
        context.MemChanges.GrabPath(context.SS, tablePath.Base()->PathId);
        context.MemChanges.GrabNewTxState(context.SS, OperationId);

        context.DbChanges.PersistPath(tablePath.Base()->PathId);
        context.DbChanges.PersistTxState(OperationId);

        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxDropTableIndexAtMainTable, tablePath.Base()->PathId);
        txState.State = TTxState::ConfigureParts;
        // do not fill txShards until all splits are done

        tablePath.Base()->PathState = NKikimrSchemeOp::EPathStateAlter;
        tablePath.Base()->LastTxId = OperationId.GetTxId();

        for (auto splitTx: table->GetSplitOpsInFlight()) {
            context.OnComplete.Dependence(splitTx.GetTxId(), OperationId.GetTxId());
        }

        context.OnComplete.ActivateTx(OperationId);

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropIndexAtMainTable AbortPropose"
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << context.SS->TabletID());
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropIndexAtMainTable AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};



}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateDropTableIndexAtMainTable(TOperationId id, TTxState::ETxState state) {
    return new TDropIndexAtMainTable(id, state);
}

ISubOperationBase::TPtr CreateDropTableIndexAtMainTable(TOperationId id, const TTxTransaction& tx) {
    return new TDropIndexAtMainTable(id, tx);
}

TVector<ISubOperationBase::TPtr> CreateDropIndex(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_VERIFY(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpDropIndex);

    LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "CreateDropIndex"
                    << ", message: " << tx.ShortDebugString()
                    << ", at schemeshard: " << context.SS->TabletID());

    auto dropOperation = tx.GetDropIndex();

    const TString workingDir = tx.GetWorkingDir();
    const TString mainTableName = dropOperation.GetTableName();
    const TString indexName = dropOperation.GetIndexName();

    TPath workingDirPath = TPath::Resolve(workingDir, context.SS);

    TPath mainTablePath = workingDirPath.Child(mainTableName);
    {
        TPath::TChecker checks = mainTablePath.Check();
        checks
            .NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .IsTable()
            .NotUnderDeleting()
            .NotUnderOperation()
            .IsCommonSensePath();

        if (!checks) {
            TString explain = TStringBuilder() << "path fail checks"
                                               << ", path: " << mainTablePath.PathString();
            auto status = checks.GetStatus(&explain);
            return {CreateReject(nextId, status, explain)};
        }
    }

    TPath indexPath = mainTablePath.Child(indexName);
    {
        TPath::TChecker checks = indexPath.Check();
        checks
            .NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .IsTableIndex()
            .NotUnderDeleting()
            .NotUnderOperation();

        if (!checks) {
            TString explain = TStringBuilder() << "path fail checks"
                                               << ", path: " << indexPath.PathString();
            auto status = checks.GetStatus(&explain);
            return {CreateReject(nextId, status, explain)};
        }
    }

    TString errStr;
    if (!context.SS->CheckApplyIf(tx, errStr)) {
        return {CreateReject(nextId, NKikimrScheme::StatusPreconditionFailed, errStr)};
    }

    if (!context.SS->CheckLocks(mainTablePath.Base()->PathId, tx, errStr)) {
        return {CreateReject(nextId, NKikimrScheme::StatusMultipleModifications, errStr)};
    }

    TVector<ISubOperationBase::TPtr> result;

    {
        auto mainTableIndexDropping = TransactionTemplate(workingDirPath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndexAtMainTable);
        auto operation = mainTableIndexDropping.MutableDropIndex();
        operation->SetTableName(mainTablePath.LeafName());
        operation->SetIndexName(indexPath.LeafName());

        result.push_back(CreateDropTableIndexAtMainTable(NextPartId(nextId, result), mainTableIndexDropping));
    }

    {
        auto indexDropping = TransactionTemplate(mainTablePath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndex);
        auto operation = indexDropping.MutableDrop();
        operation->SetName(ToString(indexPath.Base()->Name));

        result.push_back(CreateDropTableIndex(NextPartId(nextId, result), indexDropping));
    }

    for (const auto& items: indexPath.Base()->GetChildren()) {
        Y_VERIFY(context.SS->PathsById.contains(items.second));
        auto implPath = context.SS->PathsById.at(items.second);
        if (implPath->Dropped()) {
            continue;
        }

        auto implTable = context.SS->PathsById.at(items.second);
        Y_VERIFY(implTable->IsTable());

        auto implTableDropping = TransactionTemplate(indexPath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropTable);
        auto operation = implTableDropping.MutableDrop();
        operation->SetName(items.first);

        result.push_back(CreateDropTable(NextPartId(nextId, result), implTableDropping));
    }

    return result;
}

}
}
