#include "datashard_kqp.h"
#include "execution_unit_ctors.h"
#include "setup_sys_locks.h"


namespace NKikimr {
namespace NDataShard {

using namespace NMiniKQL;

class TExecuteDataTxUnit : public TExecutionUnit {
public:
    TExecuteDataTxUnit(TDataShard& dataShard,
                       TPipeline& pipeline);
    ~TExecuteDataTxUnit() override;

    bool IsReadyToExecute(TOperation::TPtr op) const override;
    EExecutionStatus Execute(TOperation::TPtr op,
                             TTransactionContext& txc,
                             const TActorContext& ctx) override;
    void Complete(TOperation::TPtr op,
                  const TActorContext& ctx) override;

private:
    void ExecuteDataTx(TOperation::TPtr op,
                       const TActorContext& ctx);
    void AddLocksToResult(TOperation::TPtr op, const TActorContext& ctx);
};

TExecuteDataTxUnit::TExecuteDataTxUnit(TDataShard& dataShard,
                                       TPipeline& pipeline)
    : TExecutionUnit(EExecutionUnitKind::ExecuteDataTx, true, dataShard, pipeline) {
}

TExecuteDataTxUnit::~TExecuteDataTxUnit() {
}

bool TExecuteDataTxUnit::IsReadyToExecute(TOperation::TPtr op) const {
    if (op->Result() || op->HasResultSentFlag() || op->IsImmediate() && WillRejectDataTx(op)) {
        return true;
    }

    if (DataShard.IsStopping()) {
        // Avoid doing any new work when datashard is stopping
        return false;
    }

    return !op->HasRuntimeConflicts();
}

EExecutionStatus TExecuteDataTxUnit::Execute(TOperation::TPtr op,
                                             TTransactionContext& txc,
                                             const TActorContext& ctx)
{
    if (op->Result() || op->HasResultSentFlag() || op->IsImmediate() && CheckRejectDataTx(op, ctx)) {
        return EExecutionStatus::Executed;
    }

    // We remember current time now, but will only count it when transaction succeeds
    TDuration waitExecuteLatency = op->GetCurrentElapsed();
    TDuration waitTotalLatency = op->GetTotalElapsed();

    if (op->IsImmediate()) {
        // Every time we execute immediate transaction we may choose a new mvcc version
        op->MvccReadWriteVersion.reset();
    }

    TSetupSysLocks guardLocks(op, DataShard);
    TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
    Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

    if (tx->IsTxDataReleased()) {
        switch (Pipeline.RestoreDataTx(tx, txc, ctx)) {
            case ERestoreDataStatus::Ok:
                break;

            case ERestoreDataStatus::Restart:
                return EExecutionStatus::Restart;

            case ERestoreDataStatus::Error:
                // For immediate transactions we want to translate this into a propose failure
                if (op->IsImmediate()) {
                    const auto& dataTx = tx->GetDataTx();
                    Y_VERIFY(!dataTx->Ready());
                    op->SetAbortedFlag();
                    BuildResult(op, NKikimrTxDataShard::TEvProposeTransactionResult::ERROR);
                    op->Result()->SetProcessError(dataTx->Code(), dataTx->GetErrors());
                    return EExecutionStatus::Executed;
                }

                // For planned transactions errors are not expected
                Y_FAIL("Failed to restore tx data: %s", tx->GetDataTx()->GetErrors().c_str());
        }
    }

    IEngineFlat* engine = tx->GetDataTx()->GetEngine();
    Y_VERIFY_S(engine, "missing engine for " << *op << " at " << DataShard.TabletID());

    if (op->IsImmediate() && !tx->ReValidateKeys()) {
        // Immediate transactions may be reordered with schema changes and become invalid
        const auto& dataTx = tx->GetDataTx();
        Y_VERIFY(!dataTx->Ready());
        op->SetAbortedFlag();
        BuildResult(op, NKikimrTxDataShard::TEvProposeTransactionResult::ERROR);
        op->Result()->SetProcessError(dataTx->Code(), dataTx->GetErrors());
        return EExecutionStatus::Executed;
    }

    // TODO: cancel tx in special execution unit.
    if (tx->GetDataTx()->CheckCancelled())
        engine->Cancel();
    else {
        ui64 consumed = tx->GetDataTx()->GetTxSize() + engine->GetMemoryAllocated();
        if (MaybeRequestMoreTxMemory(consumed, txc)) {
            LOG_TRACE_S(ctx, NKikimrServices::TX_DATASHARD, "Operation " << *op << " at " << DataShard.TabletID()
                << " requested " << txc.GetRequestedMemory() << " more memory");

            DataShard.IncCounter(COUNTER_TX_WAIT_RESOURCE);
            return EExecutionStatus::Restart;
        }
        engine->SetMemoryLimit(txc.GetMemoryLimit() - tx->GetDataTx()->GetTxSize());
    }

    try {
        try {
            ExecuteDataTx(op, ctx);
        } catch (const TNotReadyTabletException&) {
            // We want to try pinning (actually precharging) all required pages
            // before restarting the transaction, to minimize future restarts.
            ui64 pageFaultCount = tx->IncrementPageFaultCount();
            engine->PinPages(pageFaultCount);
            throw;
        }
    } catch (const TMemoryLimitExceededException&) {
        LOG_TRACE_S(ctx, NKikimrServices::TX_DATASHARD, "Operation " << *op << " at " << DataShard.TabletID()
            << " exceeded memory limit " << txc.GetMemoryLimit()
            << " and requests " << txc.GetMemoryLimit() * MEMORY_REQUEST_FACTOR
            << " more for the next try");

        txc.NotEnoughMemory();
        DataShard.IncCounter(DataShard.NotEnoughMemoryCounter(txc.GetNotEnoughMemoryCount()));

        engine->ReleaseUnusedMemory();
        txc.RequestMemory(txc.GetMemoryLimit() * MEMORY_REQUEST_FACTOR);

        tx->ReleaseTxData(txc, ctx);

        return EExecutionStatus::Restart;
    } catch (const TNotReadyTabletException&) {
        LOG_TRACE_S(ctx, NKikimrServices::TX_DATASHARD, "Tablet " << DataShard.TabletID()
            << " is not ready for " << *op << " execution");

        DataShard.IncCounter(COUNTER_TX_TABLET_NOT_READY);

        tx->ReleaseTxData(txc, ctx);

        return EExecutionStatus::Restart;
    }

    DataShard.IncCounter(COUNTER_WAIT_EXECUTE_LATENCY_MS, waitExecuteLatency.MilliSeconds());
    DataShard.IncCounter(COUNTER_WAIT_TOTAL_LATENCY_MS, waitTotalLatency.MilliSeconds());
    op->ResetCurrentTimer();

    if (op->IsReadOnly())
        return EExecutionStatus::Executed;

    return EExecutionStatus::ExecutedNoMoreRestarts;
}

void TExecuteDataTxUnit::ExecuteDataTx(TOperation::TPtr op,
                                       const TActorContext& ctx) {
    TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
    IEngineFlat* engine = tx->GetDataTx()->GetEngine();

    DataShard.ReleaseCache(*tx);
    tx->GetDataTx()->ResetCounters();

    auto [readVersion, writeVersion] = DataShard.GetReadWriteVersions(tx);
    tx->GetDataTx()->SetReadVersion(readVersion);
    tx->GetDataTx()->SetWriteVersion(writeVersion);

    // TODO: is it required to always prepare outgoing read sets?
    if (!engine->IsAfterOutgoingReadsetsExtracted()) {
        engine->PrepareOutgoingReadsets();
        engine->AfterOutgoingReadsetsExtracted();
    }

    for (auto& rs : op->InReadSets()) {
        for (auto& rsdata : rs.second) {
            engine->AddIncomingReadset(rsdata.Body);
        }
    }

    if (tx->GetDataTx()->CanCancel()) {
        engine->SetDeadline(tx->GetDataTx()->Deadline());
    }

    IEngineFlat::EResult engineResult = engine->Execute();
    if (engineResult != IEngineFlat::EResult::Ok) {
        TString errorMessage = TStringBuilder() << "Datashard execution error for " << *op << " at "
                                                << DataShard.TabletID() << ": " << engine->GetErrors();

        switch (engineResult) {
            case IEngineFlat::EResult::ResultTooBig:
                LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD, errorMessage);
                break;
            case IEngineFlat::EResult::Cancelled:
                LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, errorMessage);
                Y_VERIFY(tx->GetDataTx()->CanCancel());
                break;
            default:
                if (op->IsReadOnly() || op->IsImmediate()) {
                    LOG_CRIT_S(ctx, NKikimrServices::TX_DATASHARD, errorMessage);
                } else {
                    // TODO: Kill only current datashard tablet.
                    Y_FAIL_S("Unexpected execution error in read-write transaction: "
                             << errorMessage);
                }
                break;
        }
    }

    if (engineResult == IEngineFlat::EResult::Cancelled)
        DataShard.IncCounter(op->IsImmediate()
                                 ? COUNTER_IMMEDIATE_TX_CANCELLED
                                 : COUNTER_PLANNED_TX_CANCELLED);

    auto& result = BuildResult(op, NKikimrTxDataShard::TEvProposeTransactionResult::COMPLETE);
    result->Record.SetOrderId(op->GetTxId());
    if (!op->IsImmediate())
        result->Record.SetStep(op->GetStep());

    if (engine->GetStatus() == IEngineFlat::EStatus::Error) {
        result->SetExecutionError(ConvertErrCode(engineResult), engine->GetErrors());
    } else {
        result->SetTxResult(engine->GetShardReply(DataShard.TabletID()));

        op->ChangeRecords() = std::move(tx->GetDataTx()->GetCollectedChanges());
    }

    LOG_TRACE_S(ctx, NKikimrServices::TX_DATASHARD,
                "Executed operation " << *op << " at tablet " << DataShard.TabletID()
                                      << " with status " << result->GetStatus());

    auto& counters = tx->GetDataTx()->GetCounters();

    LOG_TRACE_S(ctx, NKikimrServices::TX_DATASHARD,
                "Datashard execution counters for " << *op << " at "
                                                    << DataShard.TabletID() << ": " << counters.ToString());

    KqpUpdateDataShardStatCounters(DataShard, counters);
    if (tx->GetDataTx()->CollectStats()) {
        KqpFillTxStats(DataShard, counters, *result);
    }

    if (counters.InvisibleRowSkips) {
        DataShard.SysLocksTable().BreakSetLocks(op->LockTxId(), op->LockNodeId());
    }

    AddLocksToResult(op, ctx);

    Pipeline.AddCommittingOp(op);
}

void TExecuteDataTxUnit::AddLocksToResult(TOperation::TPtr op, const TActorContext& ctx) {
    auto locks = DataShard.SysLocksTable().ApplyLocks();
    for (const auto& lock : locks) {
        if (lock.IsError()) {
            LOG_NOTICE_S(TActivationContext::AsActorContext(), NKikimrServices::TX_DATASHARD,
                         "Lock is not set for " << *op << " at " << DataShard.TabletID()
                                                << " lock " << lock);
        }
        op->Result()->AddTxLock(lock.LockId, lock.DataShard, lock.Generation, lock.Counter,
                                lock.SchemeShard, lock.PathId);
    }
    DataShard.SubscribeNewLocks(ctx);
}

void TExecuteDataTxUnit::Complete(TOperation::TPtr, const TActorContext&) {
}

THolder<TExecutionUnit> CreateExecuteDataTxUnit(TDataShard& dataShard, TPipeline& pipeline) {
    return THolder(new TExecuteDataTxUnit(dataShard, pipeline));
}

} // namespace NDataShard
} // namespace NKikimr
