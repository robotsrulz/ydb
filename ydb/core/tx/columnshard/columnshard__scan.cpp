#include "columnshard__scan.h"
#include "columnshard__index_scan.h"
#include "columnshard__stats_scan.h"

#include <ydb/core/tx/columnshard/blob_cache.h>
#include <ydb/core/tx/columnshard/columnshard_impl.h>
#include <ydb/core/tx/columnshard/columnshard_txs.h>
#include <ydb/core/tablet_flat/flat_row_celled.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <ydb/core/kqp/kqp_compute.h>
#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/library/yql/core/issue/yql_issue.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>

namespace NKikimr::NColumnShard {

using namespace NKqp;
using NBlobCache::TBlobRange;

class TTxScan : public TTxReadBase {
public:
    using TReadMetadataPtr = NOlap::TReadMetadataBase::TConstPtr;

    TTxScan(TColumnShard* self, TEvColumnShard::TEvScan::TPtr& ev)
        : TTxReadBase(self)
        , Ev(ev)
    {}

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override;
    void Complete(const TActorContext& ctx) override;
    TTxType GetTxType() const override { return TXTYPE_START_SCAN; }

private:
    std::shared_ptr<NOlap::TReadMetadataBase> CreateReadMetadata(const TActorContext& ctx, TReadDescription& read,
        bool isIndexStats, bool isReverse, ui64 limit);

private:
    TEvColumnShard::TEvScan::TPtr Ev;
    TVector<TReadMetadataPtr> ReadMetadataRanges;
};


constexpr ui64 INIT_BATCH_ROWS = 1000;
constexpr i64 DEFAULT_READ_AHEAD_BYTES = 1*1024*1024;
constexpr TDuration SCAN_HARD_TIMEOUT = TDuration::Minutes(10);
constexpr TDuration SCAN_HARD_TIMEOUT_GAP = TDuration::Seconds(5);

class TColumnShardScan : public TActorBootstrapped<TColumnShardScan>, NArrow::IRowWriter {
public:
    static constexpr auto ActorActivityType() {
        return NKikimrServices::TActivity::KQP_OLAP_SCAN;
    }

public:
    TColumnShardScan(const TActorId& columnShardActorId, const TActorId& scanComputeActorId,
                     ui32 scanId, ui64 txId, ui32 scanGen, ui64 requestCookie,
                     const TString& table, TDuration timeout, TVector<TTxScan::TReadMetadataPtr>&& readMetadataList,
                     NKikimrTxDataShard::EScanDataFormat dataFormat)
        : ColumnShardActorId(columnShardActorId)
        , ScanComputeActorId(scanComputeActorId)
        , BlobCacheActorId(NBlobCache::MakeBlobCacheServiceId())
        , ScanId(scanId)
        , TxId(txId)
        , ScanGen(scanGen)
        , RequestCookie(requestCookie)
        , DataFormat(dataFormat)
        , TablePath(table)
        , ReadMetadataRanges(std::move(readMetadataList))
        , ReadMetadataIndex(0)
        , Deadline(TInstant::Now() + (timeout ? timeout + SCAN_HARD_TIMEOUT_GAP : SCAN_HARD_TIMEOUT))
    {
        KeyYqlSchema = ReadMetadataRanges[ReadMetadataIndex]->GetKeyYqlSchema();
    }

    void Bootstrap(const TActorContext& ctx) {
        ScanActorId = ctx.SelfID;

        TimeoutActorId = CreateLongTimer(ctx, Deadline - TInstant::Now(),
            new IEventHandle(SelfId(), SelfId(), new TEvents::TEvWakeup));

        Y_VERIFY(!ScanIterator);
        ScanIterator = ReadMetadataRanges[ReadMetadataIndex]->StartScan();

        // propagate self actor id // TODO: FlagSubscribeOnSession ?
        Send(ScanComputeActorId, new TEvKqpCompute::TEvScanInitActor(ScanId, ctx.SelfID, ScanGen), IEventHandle::FlagTrackDelivery);

        Become(&TColumnShardScan::StateScan);
    }

private:
    STATEFN(StateScan) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvKqpCompute::TEvScanDataAck, HandleScan);
            hFunc(NBlobCache::TEvBlobCache::TEvReadBlobRangeResult, HandleScan);
            hFunc(TEvKqp::TEvAbortExecution, HandleScan);
            hFunc(TEvents::TEvUndelivered, HandleScan);
            hFunc(TEvents::TEvWakeup, HandleScan);
            default:
                Y_FAIL("TColumnShardScan: unexpected event 0x%08" PRIx32, ev->GetTypeRewrite());
        }
    }

    bool ReadNextBlob() {
        auto blobRange = ScanIterator->GetNextBlobToRead();
        if (!blobRange.BlobId.IsValid()) {
            return false;
        }

        auto& externBlobs = ReadMetadataRanges[ReadMetadataIndex]->ExternBlobs;
        bool fallback = externBlobs && externBlobs->count(blobRange.BlobId);
        Send(BlobCacheActorId, new NBlobCache::TEvBlobCache::TEvReadBlobRange(blobRange, true, fallback));
        ++InFlightReads;
        InFlightReadBytes += blobRange.Size;
        return true;
    }

    void HandleScan(TEvKqpCompute::TEvScanDataAck::TPtr& ev) {
        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " got ScanDataAck"
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
            << " freeSpace: " << ev->Get()->FreeSpace << " prevFreeSpace: " << PeerFreeSpace);

        --InFlightScanDataMessages;

        if (!ComputeActorId) {
            ComputeActorId = ev->Sender;
            InFlightScanDataMessages = 0;
        }

        Y_VERIFY(ev->Get()->Generation == ScanGen);

        PeerFreeSpace = ev->Get()->FreeSpace;

        ContinueProcessing();
    }

    void HandleScan(NBlobCache::TEvBlobCache::TEvReadBlobRangeResult::TPtr& ev) {
        --InFlightReads;

        auto& event = *ev->Get();
        const auto& blobRange = event.BlobRange;

        if (event.Status != NKikimrProto::EReplyStatus::OK) {
            LOG_WARN_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
                "Scan " << ScanActorId << " got TEvReadBlobRangeResult error"
                << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
                << " blob: " << ev->Get()->BlobRange
                << " status: " <<  NKikimrProto::EReplyStatus_Name(event.Status));
            SendError(event.Status);
            return Finish();
        }

        Y_VERIFY(event.Data.size() == blobRange.Size,
            "Read %s, size %" PRISZT, event.BlobRange.ToString().c_str(), event.Data.size());

        InFlightReadBytes -= blobRange.Size;

        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " got TEvReadBlobRangeResult"
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
            << " blob: " << ev->Get()->BlobRange
            << " prevFreeSpace: " << PeerFreeSpace);

        ScanIterator->AddData(blobRange, event.Data);

        ContinueProcessing();
    }

    // Returns true if it was able to produce new batch
    bool ProduceResults() {
        Y_VERIFY(!Finished);

        if (ScanIterator->Finished()) {
            LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
                "Scan " << ScanActorId << " producing result: scan iterator is finished"
                << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath);
            return false;
        }

        auto result = ScanIterator->GetBatch();
        if (ResultYqlSchema.empty() && DataFormat != NKikimrTxDataShard::EScanDataFormat::ARROW) {
            ResultYqlSchema = ReadMetadataRanges[ReadMetadataIndex]->GetResultYqlSchema();
        }
        if (!result.ResultBatch) {
            LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
                "Scan " << ScanActorId << " producing result: no data is ready yet"
                << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath);
            return false;
        }

        auto& batch = result.ResultBatch;
        int numRows = batch->num_rows();
        int numColumns = batch->num_columns();
        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " producing result: got ready result"
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
            << " blob (" << numColumns << " columns, " << numRows << " rows)");

        switch (DataFormat) {
            case NKikimrTxDataShard::EScanDataFormat::UNSPECIFIED:
            case NKikimrTxDataShard::EScanDataFormat::CELLVEC: {
                MakeResult(INIT_BATCH_ROWS);
                NArrow::TArrowToYdbConverter batchConverter(ResultYqlSchema, *this);
                TString errStr;
                bool ok = batchConverter.Process(*batch, errStr);
                Y_VERIFY(ok, "%s", errStr.c_str());
                break;
            }
            case NKikimrTxDataShard::EScanDataFormat::ARROW: {
                MakeResult(0);
                Result->ArrowBatch = batch;

                Rows += batch->num_rows();
                Bytes += NArrow::GetBatchDataSize(batch);
                break;
            }
        } // switch DataFormat
        if (result.LastReadKey) {
            Result->LastKey = ConvertLastKey(result.LastReadKey);
        } else {
            Y_VERIFY(numRows == 0, "Got non-empty result batch without last key");
        }
        SendResult(false, false);
        return true;
    }

    void ContinueProcessingStep() {
        if (!ScanIterator) {
            LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
                "Scan " << ScanActorId << " iterator is not initialized"
                << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath);
            return;
        }

        if (PeerFreeSpace == 0) {
            // Throttle down until the compute actor is ready to receive more rows

            LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
                "Scan " << ScanActorId << " waiting for peer free space"
                << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath);
            return;
        }

        // Send new results if there is available capacity
        i64 MAX_SCANDATA_MESSAGES_IN_FLIGHT = 2;
        while (InFlightScanDataMessages < MAX_SCANDATA_MESSAGES_IN_FLIGHT) {
            if (!ProduceResults()) {
                break;
            }
        }

        // Switch to the next range if the current one is finished
        if (ScanIterator->Finished() && !InFlightReads) {
            NextReadMetadata();
        }

        size_t MIN_READY_RESULTS_IN_QUEUE = 3;
        if (ScanIterator && ScanIterator->ReadyResultsCount() < MIN_READY_RESULTS_IN_QUEUE) {
            // Make read-ahead requests for the subsequent blobs
            while (InFlightReadBytes < MaxReadAheadBytes || !InFlightReads) {
                if (!ReadNextBlob()) {
                    break;
                }
            }
        }
    }

    void ContinueProcessing() {
        const i64 maxSteps = ReadMetadataRanges.size();
        for (i64 step = 0; step <= maxSteps; ++step) {
            ContinueProcessingStep();

            // Only exist the loop if either:
            // * we have finished scanning ALL the ranges
            // * or there is an in-flight blob read or ScanData message for which
            //   we will get a reply and will be able to proceed futher
            if  (!ScanIterator || InFlightScanDataMessages != 0 || InFlightReads != 0) {
                return;
            }
        }

        // The loop has finished without any progress!
        LOG_ERROR_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " is hanging"
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath);
    }

    void HandleScan(TEvKqp::TEvAbortExecution::TPtr& ev) {
        auto& msg = ev->Get()->Record;
        TString reason = ev->Get()->GetIssues().ToOneLineString();

        auto prio = msg.GetStatusCode() == NYql::NDqProto::StatusIds::SUCCESS ? NActors::NLog::PRI_DEBUG : NActors::NLog::PRI_WARN;
        LOG_LOG_S(*TlsActivationContext, prio, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " got AbortExecution"
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
            << " code: " << NYql::NDqProto::StatusIds_StatusCode_Name(msg.GetStatusCode())
            << " reason: " << reason);

        AbortReason = std::move(reason);
        SendError(NKikimrProto::EReplyStatus::ERROR); // TODO: better status?
        Finish();
    }

    void HandleScan(TEvents::TEvUndelivered::TPtr& ev) {
        ui32 eventType = ev->Get()->SourceType;
        switch (eventType) {
            case TEvKqpCompute::TEvScanInitActor::EventType:
                AbortReason = "init failed";
                break;
            case TEvKqpCompute::TEvScanData::EventType:
                AbortReason = "failed to send data batch";
                break;
        }

        LOG_WARN_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " undelivered event: " << eventType
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
            << " reason: " << ev->Get()->Reason
            << " description: " << AbortReason);

        Finish();
    }

    void HandleScan(TEvents::TEvWakeup::TPtr&) {
        LOG_ERROR_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " guard execution timeout"
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath);

        TimeoutActorId = {};
        Finish();
    }

private:
    void MakeResult(size_t reserveRows = 0) {
        if (!Finished && !Result) {
            Result = MakeHolder<TEvKqpCompute::TEvScanData>(ScanId, ScanGen);
            if (reserveRows) {
                Y_VERIFY(DataFormat != NKikimrTxDataShard::EScanDataFormat::ARROW);
                Result->Rows.reserve(reserveRows);
            }
        }
    }

    void NextReadMetadata() {
        ScanIterator.reset();

        ++ReadMetadataIndex;

        if (ReadMetadataIndex == ReadMetadataRanges.size()) {
            // Send empty batch with "finished" flag
            MakeResult();
            SendResult(false, true);
            return Finish();
        }

        ScanIterator = ReadMetadataRanges[ReadMetadataIndex]->StartScan();

        // Used in TArrowToYdbConverter
        ResultYqlSchema.clear();
    }

    void AddRow(const TConstArrayRef<TCell>& row) override {
        Result->Rows.emplace_back(TOwnedCellVec::Make(row));
        ++Rows;

        // NOTE: Some per-row overhead to deal with the case when no columns were requested
        Bytes += std::max((ui64)8, (ui64)Result->Rows.back().DataSize());
    }

    TOwnedCellVec ConvertLastKey(const std::shared_ptr<arrow::RecordBatch>& lastReadKey) {
        Y_VERIFY(lastReadKey, "last key must be passed");

        struct TSingeRowWriter : public IRowWriter {
            TOwnedCellVec Row;
            bool Done = false;
            void AddRow(const TConstArrayRef<TCell>& row) override {
                Y_VERIFY(!Done);
                Row = TOwnedCellVec::Make(row);
                Done = true;
            }
        } singleRowWriter;
        NArrow::TArrowToYdbConverter converter(KeyYqlSchema, singleRowWriter);
        TString errStr;
        bool ok = converter.Process(*lastReadKey, errStr);
        Y_VERIFY(ok, "%s", errStr.c_str());

        Y_VERIFY(singleRowWriter.Done);
        return singleRowWriter.Row;
    }

    bool SendResult(bool pageFault, bool lastBatch){
        if (Finished) {
            return true;
        }

        Result->PageFault = pageFault;
        Result->PageFaults = PageFaults;
        Result->Finished = lastBatch;
        TDuration totalElapsedTime = TDuration::Seconds(GetElapsedTicksAsSeconds());
        // Result->TotalTime = totalElapsedTime - LastReportedElapsedTime;
        // TODO: Result->CpuTime = ...
        LastReportedElapsedTime = totalElapsedTime;

        PageFaults = 0;

        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " send ScanData to " << ComputeActorId
            << " txId: " << TxId << " scanId: " << ScanId << " gen: " << ScanGen << " table: " << TablePath
            << " bytes: " << Bytes << " rows: " << Rows << " page faults: " << Result->PageFaults
            << " finished: " << Result->Finished << " pageFault: " << Result->PageFault);

        if (PeerFreeSpace < Bytes) {
            PeerFreeSpace = 0;
        } else {
            PeerFreeSpace -= Bytes;
        }

        Finished = Result->Finished;

        Send(ComputeActorId, Result.Release(), IEventHandle::FlagTrackDelivery); // TODO: FlagSubscribeOnSession ?
        ++InFlightScanDataMessages;

        ReportStats();

        return true;
    }

    void SendError(NKikimrProto::EReplyStatus status) {
        auto ev = MakeHolder<TEvKqpCompute::TEvScanError>(ScanGen);

        ev->Record.SetStatus(Ydb::StatusIds::GENERIC_ERROR);
        auto issue = NYql::YqlIssue({}, NYql::TIssuesIds::KIKIMR_RESULT_UNAVAILABLE, TStringBuilder()
            << "Table " << TablePath << " scan failed, reason: " << NKikimrProto::EReplyStatus_Name(status));
        NYql::IssueToMessage(issue, ev->Record.MutableIssues()->Add());

        Send(ComputeActorId, ev.Release());
    }

    void Finish() {
        if (TimeoutActorId) {
            Send(TimeoutActorId, new TEvents::TEvPoison);
        }

        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_COLUMNSHARD_SCAN,
            "Scan " << ScanActorId << " finished");

        Send(ColumnShardActorId, new TEvPrivate::TEvReadFinished(RequestCookie, TxId));
        ReportStats();
        PassAway();
    }

    void ReportStats() {
        Send(ColumnShardActorId, new TEvPrivate::TEvScanStats(Rows, Bytes));
        Rows = 0;
        Bytes = 0;
    }

private:
    const TActorId ColumnShardActorId;
    const TActorId ScanComputeActorId;
    TActorId ComputeActorId;
    TActorId ScanActorId;
    TActorId BlobCacheActorId;
    const ui32 ScanId;
    const ui64 TxId;
    const ui32 ScanGen;
    const ui64 RequestCookie;
    const i64 MaxReadAheadBytes = DEFAULT_READ_AHEAD_BYTES;
    const NKikimrTxDataShard::EScanDataFormat DataFormat;

    const TString TablePath;

    TVector<NOlap::TReadMetadataBase::TConstPtr> ReadMetadataRanges;
    ui32 ReadMetadataIndex;
    std::unique_ptr<TScanIteratorBase> ScanIterator;

    TVector<std::pair<TString, NScheme::TTypeId>> ResultYqlSchema;
    TVector<std::pair<TString, NScheme::TTypeId>> KeyYqlSchema;
    const TSerializedTableRange TableRange;
    const TSmallVec<bool> SkipNullKeys;
    const TInstant Deadline;

    TActorId TimeoutActorId;
    TMaybe<TString> AbortReason;

    ui64 PeerFreeSpace = 0;
    THolder<TEvKqpCompute::TEvScanData> Result;
    i64 InFlightReads = 0;
    i64 InFlightReadBytes = 0;
    i64 InFlightScanDataMessages = 0;
    bool Finished = false;
    ui64 Rows = 0;
    ui64 Bytes = 0;
    ui32 PageFaults = 0;
    TDuration LastReportedElapsedTime;
};

static void FillPredicatesFromRange(TReadDescription& read, const ::NKikimrTx::TKeyRange& keyRange,
                                    const TVector<std::pair<TString, NScheme::TTypeId>>& ydbPk, ui64 tabletId) {
    TSerializedTableRange range(keyRange);

    read.GreaterPredicate = std::make_shared<NOlap::TPredicate>();
    read.LessPredicate = std::make_shared<NOlap::TPredicate>();
    std::tie(*read.GreaterPredicate, *read.LessPredicate) = RangePredicates(range, ydbPk);

    LOG_S_DEBUG("TTxScan range predicate. From key size: " << range.From.GetCells().size()
        << " To key size: " << range.To.GetCells().size()
        << " greater predicate over columns: " << read.GreaterPredicate->ToString()
        << " less predicate over columns: " << read.LessPredicate->ToString()
        << " at tablet " << tabletId);

    if (read.GreaterPredicate && read.GreaterPredicate->Empty()) {
        read.GreaterPredicate.reset();
    }

    if (read.LessPredicate && read.LessPredicate->Empty()) {
        read.LessPredicate.reset();
    }
}

std::shared_ptr<NOlap::TReadStatsMetadata>
PrepareStatsReadMetadata(ui64 tabletId, const TReadDescription& read, const std::unique_ptr<NOlap::IColumnEngine>& index, TString& error) {
    THashSet<ui32> readColumnIds(read.ColumnIds.begin(), read.ColumnIds.end());
    for (auto& [id, name] : read.ProgramSourceColumns) {
        readColumnIds.insert(id);
    }

    for (ui32 colId : readColumnIds) {
        if (!PrimaryIndexStatsSchema.Columns.count(colId)) {
            error = Sprintf("Columnd id %" PRIu32 " not found", colId);
            return {};
        }
    }

    auto out = std::make_shared<NOlap::TReadStatsMetadata>(tabletId);

    out->ReadColumnIds.assign(readColumnIds.begin(), readColumnIds.end());
    out->ResultColumnIds = read.ColumnIds;
    out->Program = std::move(read.Program);

    if (!index) {
        return out;
    }

    ui64 fromPathId = 1;
    ui64 toPathId = Max<ui64>();

    if (read.GreaterPredicate && read.GreaterPredicate->Good()) {
        auto from = read.GreaterPredicate->Batch->column(0);
        if (from) {
            fromPathId = static_cast<arrow::UInt64Array&>(*from).Value(0);
        }
        out->GreaterPredicate = read.GreaterPredicate;
    }

    if (read.LessPredicate && read.LessPredicate->Good()) {
        auto to = read.LessPredicate->Batch->column(0);
        if (to) {
            toPathId = static_cast<arrow::UInt64Array&>(*to).Value(0);
        }
        out->LessPredicate = read.LessPredicate;
    }

    const auto& stats = index->GetStats();
    if (read.TableName.EndsWith(NOlap::TIndexInfo::TABLE_INDEX_STATS_TABLE)) {
        if (fromPathId <= read.PathId && toPathId >= read.PathId && stats.count(read.PathId)) {
            out->IndexStats[read.PathId] = std::make_shared<NOlap::TColumnEngineStats>(*stats.at(read.PathId));
        }
    } else if (read.TableName.EndsWith(NOlap::TIndexInfo::STORE_INDEX_STATS_TABLE)) {
        auto it = stats.lower_bound(fromPathId);
        auto itEnd = stats.upper_bound(toPathId);
        for (; it != itEnd; ++it) {
            out->IndexStats[it->first] = std::make_shared<NOlap::TColumnEngineStats>(*it->second);
        }
    }
    return out;
}

std::shared_ptr<NOlap::TReadMetadataBase> TTxScan::CreateReadMetadata(const TActorContext& ctx, TReadDescription& read,
    bool indexStats, bool isReverse, ui64 itemsLimit)
{
    std::shared_ptr<NOlap::TReadMetadataBase> metadata;
    if (indexStats) {
        metadata = PrepareStatsReadMetadata(Self->TabletID(), read, Self->PrimaryIndex, ErrorDescription);
    } else {
        metadata = PrepareReadMetadata(ctx, read, Self->InsertTable, Self->PrimaryIndex, ErrorDescription);
    }

    if (!metadata) {
        return {};
    }

    if (isReverse) {
        metadata->SetDescSorting();
    }

    if (itemsLimit) {
        metadata->Limit = itemsLimit;
    }

    return metadata;
}


bool TTxScan::Execute(TTransactionContext& txc, const TActorContext& ctx) {
    Y_UNUSED(txc);

    auto& record = Ev->Get()->Record;
    const auto& snapshot = record.GetSnapshot();
    const auto scanId = record.GetScanId();
    const ui64 txId = record.GetTxId();

    LOG_S_DEBUG("TTxScan prepare txId: " << txId << " scanId: " << scanId << " at tablet " << Self->TabletID());

    ui64 itemsLimit = record.HasItemsLimit() ? record.GetItemsLimit() : 0;

    TReadDescription read;
    read.PlanStep = snapshot.GetStep();
    read.TxId = snapshot.GetTxId();
    read.PathId = record.GetLocalPathId();
    read.ReadNothing = Self->PathsToDrop.count(read.PathId);
    read.TableName = record.GetTablePath();
    bool isIndexStats = read.TableName.EndsWith(NOlap::TIndexInfo::STORE_INDEX_STATS_TABLE) ||
        read.TableName.EndsWith(NOlap::TIndexInfo::TABLE_INDEX_STATS_TABLE);
    read.ColumnIds.assign(record.GetColumnTags().begin(), record.GetColumnTags().end());

    // TODO: move this to CreateReadMetadata?
    if (read.ColumnIds.empty()) {
        // "SELECT COUNT(*)" requests empty column list but we need non-empty list for PrepareReadMetadata.
        // So we add first PK column to the request.
        if (!isIndexStats) {
            read.ColumnIds.push_back(Self->PrimaryIndex->GetIndexInfo().GetPKFirstColumnId());
        } else {
            read.ColumnIds.push_back(PrimaryIndexStatsSchema.KeyColumns.front());
        }
    }

    bool parseResult;

    if (!isIndexStats) {
        TIndexColumnResolver columnResolver(Self->PrimaryIndex->GetIndexInfo());
        parseResult = ParseProgram(ctx, record.GetOlapProgramType(), record.GetOlapProgram(), read, columnResolver);
    } else {
        TStatsColumnResolver columnResolver;
        parseResult = ParseProgram(ctx, record.GetOlapProgramType(), record.GetOlapProgram(), read, columnResolver);
    }

    if (!parseResult) {
        return true;
    }

    if (!record.RangesSize()) {
        auto range = CreateReadMetadata(ctx, read, isIndexStats, record.GetReverse(), itemsLimit);
        if (range) {
            if (!isIndexStats) {
                Self->MapExternBlobs(ctx, static_cast<NOlap::TReadMetadata&>(*range));
            }
            ReadMetadataRanges = {range};
        }
        return true;
    }

    ReadMetadataRanges.reserve(record.RangesSize());

    auto ydbKey = isIndexStats ?
        NOlap::GetColumns(PrimaryIndexStatsSchema, PrimaryIndexStatsSchema.KeyColumns) :
        Self->PrimaryIndex->GetIndexInfo().GetPK();

    for (auto& range: record.GetRanges()) {
        FillPredicatesFromRange(read, range, ydbKey, Self->TabletID());
        auto newRange = CreateReadMetadata(ctx, read, isIndexStats, record.GetReverse(), itemsLimit);
        if (!newRange) {
            ReadMetadataRanges.clear();
            return true;
        }
        if (!isIndexStats) {
            Self->MapExternBlobs(ctx, static_cast<NOlap::TReadMetadata&>(*newRange));
        }
        ReadMetadataRanges.emplace_back(newRange);
    }

    if (record.GetReverse()) {
        std::reverse(ReadMetadataRanges.begin(), ReadMetadataRanges.end());
    }

    return true;
}

template <typename T>
struct TContainerPrinter {
    const T& Ref;

    TContainerPrinter(const T& ref)
        : Ref(ref)
    {}

    friend IOutputStream& operator << (IOutputStream& out, const TContainerPrinter& cont) {
        for (auto& ptr : cont.Ref) {
            out << *ptr << " ";
        }
        return out;
    }
};

void TTxScan::Complete(const TActorContext& ctx) {
    auto& request = Ev->Get()->Record;
    auto scanComputeActor = Ev->Sender;
    const auto& snapshot = request.GetSnapshot();
    const auto scanId = request.GetScanId();
    const ui64 txId = request.GetTxId();
    const ui32 scanGen = request.GetGeneration();
    TString table = request.GetTablePath();
    auto dataFormat = request.GetDataFormat();
    TDuration timeout = TDuration::MilliSeconds(request.GetTimeoutMs());

    if (scanGen > 1) {
        Self->IncCounter(COUNTER_SCAN_RESTARTED);
    }

    TStringStream detailedInfo;
    if (IS_LOG_PRIORITY_ENABLED(ctx, NActors::NLog::PRI_TRACE, NKikimrServices::TX_COLUMNSHARD)) {
        detailedInfo << " read metadata: (" << TContainerPrinter(ReadMetadataRanges) << ")"
            << " req: " << request;
    }

    if (ReadMetadataRanges.empty()) {
        LOG_S_DEBUG("TTxScan failed "
                << " txId: " << txId
                << " scanId: " << scanId
                << " gen: " << scanGen
                << " table: " << table
                << " snapshot: " << snapshot
                << " timeout: " << timeout
                << detailedInfo.Str()
                << " at tablet " << Self->TabletID());

        Y_VERIFY(ErrorDescription);
        auto ev = MakeHolder<TEvKqpCompute::TEvScanError>(scanGen);

        ev->Record.SetStatus(Ydb::StatusIds::BAD_REQUEST);
        auto issue = NYql::YqlIssue({}, NYql::TIssuesIds::KIKIMR_BAD_REQUEST, TStringBuilder()
            << "Table " << table << " (shard " << Self->TabletID() << ") scan failed, reason: " << ErrorDescription);
        NYql::IssueToMessage(issue, ev->Record.MutableIssues()->Add());

        ctx.Send(scanComputeActor, ev.Release());
        return;
    }

    ui64 requestCookie = Self->InFlightReadsTracker.AddInFlightRequest(ReadMetadataRanges, *Self->BlobManager);
    auto statsDelta = Self->InFlightReadsTracker.GetSelectStatsDelta();

    Self->IncCounter(COUNTER_READ_INDEX_GRANULES, statsDelta.Granules);
    Self->IncCounter(COUNTER_READ_INDEX_PORTIONS, statsDelta.Portions);
    Self->IncCounter(COUNTER_READ_INDEX_BLOBS, statsDelta.Blobs);
    Self->IncCounter(COUNTER_READ_INDEX_ROWS, statsDelta.Rows);
    Self->IncCounter(COUNTER_READ_INDEX_BYTES, statsDelta.Bytes);

    auto scanActor = ctx.Register(new TColumnShardScan(Self->SelfId(), scanComputeActor,
        scanId, txId, scanGen, requestCookie, table, timeout, std::move(ReadMetadataRanges), dataFormat));

    LOG_S_DEBUG("TTxScan starting " << scanActor
                << " txId: " << txId
                << " scanId: " << scanId
                << " gen: " << scanGen
                << " table: " << table
                << " snapshot: " << snapshot
                << " timeout: " << timeout
                << detailedInfo.Str()
                << " at tablet " << Self->TabletID());
}


void TColumnShard::Handle(TEvColumnShard::TEvScan::TPtr& ev, const TActorContext& ctx) {
    auto& record = ev->Get()->Record;
    ui64 txId = record.GetTxId();
    const auto& scanId = record.GetScanId();
    const auto& snapshot = record.GetSnapshot();

    TRowVersion readVersion(snapshot.GetStep(), snapshot.GetTxId());
    TRowVersion maxReadVersion = GetMaxReadVersion();

    LOG_S_DEBUG("EvScan txId: " << txId
        << " scanId: " << scanId
        << " version: " << readVersion
        << " readable: " << maxReadVersion
        << " at tablet " << TabletID());

    if (maxReadVersion < readVersion) {
        WaitingScans.emplace(readVersion, std::move(ev));
        WaitPlanStep(readVersion.Step);
        return;
    }

    ScanTxInFlight.insert({txId, TAppData::TimeProvider->Now()});
    SetCounter(COUNTER_SCAN_IN_FLY, ScanTxInFlight.size());
    Execute(new TTxScan(this, ev), ctx);
}

}
