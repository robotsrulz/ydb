#include "change_collector.h"
#include "datashard_common_upload.h"

namespace NKikimr {
namespace NDataShard {

template <typename TEvRequest, typename TEvResponse>
TCommonUploadOps<TEvRequest, TEvResponse>::TCommonUploadOps(typename TEvRequest::TPtr& ev, bool breakLocks, bool collectChanges)
    : Ev(ev)
    , BreakLocks(breakLocks)
    , CollectChanges(collectChanges)
{
}

template <typename TEvRequest, typename TEvResponse>
bool TCommonUploadOps<TEvRequest, TEvResponse>::Execute(TDataShard* self, TTransactionContext& txc,
        const TRowVersion& readVersion, const TRowVersion& writeVersion)
{
    const auto& record = Ev->Get()->Record;
    Result = MakeHolder<TEvResponse>(self->TabletID());

    TInstant deadline = TInstant::MilliSeconds(record.GetCancelDeadlineMs());
    if (deadline && deadline < AppData()->TimeProvider->Now()) {
        SetError(NKikimrTxDataShard::TError::EXECUTION_CANCELLED, "Deadline exceeded");
        return true;
    }

    const ui64 tableId = record.GetTableId();
    const TTableId fullTableId(self->GetPathOwnerId(), tableId);
    const ui64 localTableId = self->GetLocalTableId(fullTableId);
    if (localTableId == 0) {
        SetError(NKikimrTxDataShard::TError::SCHEME_ERROR, Sprintf("Unknown table id %" PRIu64, tableId));
        return true;
    }
    const ui64 shadowTableId = self->GetShadowTableId(fullTableId);

    const TUserTable& tableInfo = *self->GetUserTables().at(tableId); /// ... find
    Y_VERIFY(tableInfo.LocalTid == localTableId);
    Y_VERIFY(tableInfo.ShadowTid == shadowTableId);

    // Check schemas
    if (record.GetRowScheme().KeyColumnIdsSize() != tableInfo.KeyColumnIds.size()) {
        SetError(NKikimrTxDataShard::TError::SCHEME_ERROR,
            Sprintf("Key column count mismatch: got %" PRIu64 ", expected %" PRIu64,
                record.GetRowScheme().KeyColumnIdsSize(), tableInfo.KeyColumnIds.size()));
        return true;
    }

    for (size_t i = 0; i < tableInfo.KeyColumnIds.size(); ++i) {
        if (record.GetRowScheme().GetKeyColumnIds(i) != tableInfo.KeyColumnIds[i]) {
            SetError(NKikimrTxDataShard::TError::SCHEME_ERROR, Sprintf("Key column schema at position %" PRISZT, i));
            return true;
        }
    }

    const bool writeToTableShadow = record.GetWriteToTableShadow();
    const bool readForTableShadow = writeToTableShadow && !shadowTableId;
    const ui32 writeTableId = writeToTableShadow && shadowTableId ? shadowTableId : localTableId;

    if (CollectChanges) {
        ChangeCollector.Reset(CreateChangeCollector(*self, txc.DB, tableInfo, true));
    }

    if (ChangeCollector) {
        ChangeCollector->SetWriteVersion(writeVersion);
        if (ChangeCollector->NeedToReadKeys()) {
            ChangeCollector->SetReadVersion(readVersion);
        }
    }

    // Prepare (id, Type) vector for value columns
    TVector<NTable::TTag> tagsForSelect;
    TVector<std::pair<ui32, NScheme::TTypeId>> valueCols;
    for (const auto& colId : record.GetRowScheme().GetValueColumnIds()) {
        if (readForTableShadow) {
            tagsForSelect.push_back(colId);
        }
        auto* col = tableInfo.Columns.FindPtr(colId);
        if (!col) {
            SetError(NKikimrTxDataShard::TError::SCHEME_ERROR, Sprintf("Missing column with id=%" PRIu32, colId));
            return true;
        }
        valueCols.emplace_back(colId, col->Type);
    }

    TVector<TRawTypeValue> key;
    TVector<NTable::TUpdateOp> value;

    TSerializedCellVec keyCells;
    TSerializedCellVec valueCells;

    bool pageFault = false;
    NTable::TRowState rowState;

    ui64 bytes = 0;
    for (const auto& r : record.GetRows()) {
        // TODO: use safe parsing!
        keyCells.Parse(r.GetKeyColumns());
        valueCells.Parse(r.GetValueColumns());

        bytes += keyCells.GetBuffer().size() + valueCells.GetBuffer().size();

        if (keyCells.GetCells().size() != tableInfo.KeyColumnTypes.size() ||
            valueCells.GetCells().size() != valueCols.size())
        {
            SetError(NKikimrTxDataShard::TError::SCHEME_ERROR, "Cell count doesn't match row scheme");
            return true;
        }

        key.clear();
        size_t ki = 0;
        ui64 keyBytes = 0;
        for (const auto& kt : tableInfo.KeyColumnTypes) {
            const TCell& c = keyCells.GetCells()[ki];
            if (kt == NScheme::NTypeIds::Uint8 && !c.IsNull() && c.AsValue<ui8>() > 127) {
                SetError(NKikimrTxDataShard::TError::BAD_ARGUMENT, "Keys with Uint8 column values >127 are currently prohibited");
                return true;
            }

            keyBytes += c.Size();
            key.emplace_back(TRawTypeValue(c.AsRef(), kt));
            ++ki;
        }

        if (keyBytes > NLimits::MaxWriteKeySize) {
            SetError(NKikimrTxDataShard::TError::BAD_ARGUMENT,
                     Sprintf("Row key size of %" PRISZT " bytes is larger than the allowed threshold %" PRIu64,
                             keyBytes, NLimits::MaxWriteKeySize));
            return true;
        }

        if (readForTableShadow) {
            rowState.Init(tagsForSelect.size());

            auto ready = txc.DB.Select(localTableId, key, tagsForSelect, rowState, 0 /* readFlags */, readVersion);
            if (ready == NTable::EReady::Page) {
                pageFault = true;
            }

            if (pageFault) {
                continue;
            }

            if (rowState == NTable::ERowOp::Erase || rowState == NTable::ERowOp::Reset) {
                // Row has been erased in the past, ignore this upsert
                continue;
            }
        }

        value.clear();
        size_t vi = 0;
        for (const auto& vt : valueCols) {
            if (valueCells.GetCells()[vi].Size() > NLimits::MaxWriteValueSize) {
                SetError(NKikimrTxDataShard::TError::BAD_ARGUMENT,
                         Sprintf("Row cell size of %" PRISZT " bytes is larger than the allowed threshold %" PRIu64,
                                 valueCells.GetBuffer().Size(), NLimits::MaxWriteValueSize));
                return true;
            }

            bool allowUpdate = true;
            if (readForTableShadow && rowState == NTable::ERowOp::Upsert && rowState.GetCellOp(vi) != NTable::ECellOp::Empty) {
                // We don't want to overwrite columns that already has some value
                allowUpdate = false;
            }

            if (allowUpdate) {
                value.emplace_back(NTable::TUpdateOp(vt.first, NTable::ECellOp::Set, TRawTypeValue(valueCells.GetCells()[vi].AsRef(), vt.second)));
            }
            ++vi;
        }

        if (readForTableShadow && rowState != NTable::ERowOp::Absent && value.empty()) {
            // We don't want to issue an Upsert when key already exists and there are no updates
            continue;
        }

        if (!writeToTableShadow) {
            if (ChangeCollector) {
                Y_VERIFY(CollectChanges);

                if (!ChangeCollector->Collect(fullTableId, NTable::ERowOp::Upsert, key, value)) {
                    pageFault = true;
                }

                if (pageFault) {
                    continue;
                }
            }

            if (BreakLocks) {
                self->SysLocksTable().BreakLock(fullTableId, keyCells.GetCells());
            }
        }

        txc.DB.Update(writeTableId, NTable::ERowOp::Upsert, key, value, writeVersion);
    }

    if (pageFault) {
        if (ChangeCollector) {
            ChangeCollector->Reset();
        }

        return false;
    }

    self->IncCounter(COUNTER_UPLOAD_ROWS, record.GetRows().size());
    self->IncCounter(COUNTER_UPLOAD_ROWS_BYTES, bytes);

    tableInfo.Stats.UpdateTime = TAppData::TimeProvider->Now();
    return true;
}

template <typename TEvRequest, typename TEvResponse>
void TCommonUploadOps<TEvRequest, TEvResponse>::GetResult(TDataShard* self, TActorId& target, THolder<IEventBase>& event, ui64& cookie) {
    Y_VERIFY(Result);

    if (Result->Record.GetStatus() == NKikimrTxDataShard::TError::OK) {
        self->IncCounter(COUNTER_BULK_UPSERT_SUCCESS);
    } else {
        self->IncCounter(COUNTER_BULK_UPSERT_ERROR);
    }

    target = Ev->Sender;
    event = std::move(Result);
    cookie = 0;
}

template <typename TEvRequest, typename TEvResponse>
const TEvRequest* TCommonUploadOps<TEvRequest, TEvResponse>::GetRequest() const {
    return Ev->Get();
}

template <typename TEvRequest, typename TEvResponse>
TEvResponse* TCommonUploadOps<TEvRequest, TEvResponse>::GetResult() {
    Y_VERIFY(Result);
    return Result.Get();
}

template <typename TEvRequest, typename TEvResponse>
TVector<NMiniKQL::IChangeCollector::TChange> TCommonUploadOps<TEvRequest, TEvResponse>::GetCollectedChanges() const {
    if (!ChangeCollector) {
        return {};
    }

    auto changes = std::move(ChangeCollector->GetCollected());
    return changes;
}

template <typename TEvRequest, typename TEvResponse>
void TCommonUploadOps<TEvRequest, TEvResponse>::SetError(ui32 status, const TString& descr) {
    Result->Record.SetStatus(status);
    Result->Record.SetErrorDescription(descr);
}

template class TCommonUploadOps<TEvDataShard::TEvUploadRowsRequest, TEvDataShard::TEvUploadRowsResponse>;
template class TCommonUploadOps<TEvDataShard::TEvUnsafeUploadRowsRequest, TEvDataShard::TEvUnsafeUploadRowsResponse>;

} // NDataShard
} // NKikimr
