#pragma once
#include "defs.h"
#include "column_engine.h"
#include "predicate.h"

namespace NKikimr::NColumnShard {
class TScanIteratorBase;
}

namespace NKikimr::NOlap {

struct TReadStats {
    TInstant BeginTimestamp;
    ui32 SelectedIndex{0};
    ui64 IndexGranules{0};
    ui64 IndexPortions{0};
    ui64 IndexBatches{0};
    ui64 CommittedBatches{0};
    ui32 UsedColumns{0};
    ui64 DataBytes{0};

    TReadStats(ui32 indexNo)
        : BeginTimestamp(TInstant::Now())
        , SelectedIndex(indexNo)
    {}

    TDuration Duration() {
        return TInstant::Now() - BeginTimestamp;
    }
};

// Holds all metedata that is needed to perform read/scan
struct TReadMetadataBase {
    using TConstPtr = std::shared_ptr<const TReadMetadataBase>;

    enum class ESorting {
        NONE = 0,
        ASC,
        DESC,
    };

    virtual ~TReadMetadataBase() = default;

    std::shared_ptr<NOlap::TPredicate> LessPredicate;
    std::shared_ptr<NOlap::TPredicate> GreaterPredicate;
    std::shared_ptr<arrow::Schema> BlobSchema;
    std::shared_ptr<arrow::Schema> LoadSchema; // ResultSchema + required for intermediate operations
    std::shared_ptr<arrow::Schema> ResultSchema; // TODO: add Program modifications
    std::vector<std::shared_ptr<NArrow::TProgramStep>> Program;
    std::shared_ptr<const THashMap<TUnifiedBlobId, TUnifiedBlobId>> ExternBlobs; // DS -> S3 map TODO: move out of base
    ESorting Sorting{ESorting::ASC}; // Sorting inside returned batches
    ui64 Limit{0}; // TODO

    bool IsAscSorted() const { return Sorting == ESorting::ASC; }
    bool IsDescSorted() const { return Sorting == ESorting::DESC; }
    bool IsSorted() const { return IsAscSorted() || IsDescSorted(); }
    void SetDescSorting() { Sorting = ESorting::DESC; }

    virtual TVector<std::pair<TString, NScheme::TTypeId>> GetResultYqlSchema() const = 0;
    virtual TVector<std::pair<TString, NScheme::TTypeId>> GetKeyYqlSchema() const = 0;
    virtual std::unique_ptr<NColumnShard::TScanIteratorBase> StartScan() const = 0;
    virtual void Dump(IOutputStream& out) const { Y_UNUSED(out); };

    bool HasProgram() const {
        return !Program.empty();
    }

    // TODO:  can this only be done for base class?
    friend IOutputStream& operator << (IOutputStream& out, const TReadMetadataBase& meta) {
        meta.Dump(out);
        return out;
    }
};

// Holds all metadata that is needed to perform read/scan
struct TReadMetadata : public TReadMetadataBase, public std::enable_shared_from_this<TReadMetadata> {
    using TConstPtr = std::shared_ptr<const TReadMetadata>;

    TIndexInfo IndexInfo;
    ui64 PlanStep = 0;
    ui64 TxId = 0;
    std::shared_ptr<TSelectInfo> SelectInfo;
    std::vector<TCommittedBlob> CommittedBlobs;
    std::shared_ptr<TReadStats> ReadStats;

    TReadMetadata(const TIndexInfo& info)
        : IndexInfo(info)
        , ReadStats(std::make_shared<TReadStats>(info.GetId()))
    {}

    bool Empty() const {
        Y_VERIFY(SelectInfo);
        return SelectInfo->Portions.empty() && CommittedBlobs.empty();
    }

    std::shared_ptr<arrow::Schema> GetSortingKey() const {
        return IndexInfo.GetSortingKey();
    }

    std::shared_ptr<arrow::Schema> GetReplaceKey() const {
        return IndexInfo.GetReplaceKey();
    }

    TVector<std::pair<TString, NScheme::TTypeId>> GetResultYqlSchema() const override {
        TVector<NTable::TTag> columnIds;
        columnIds.reserve(ResultSchema->num_fields());
        for (const auto& field: ResultSchema->fields()) {
            TString name = TStringBuilder() << field->name();
            columnIds.emplace_back(IndexInfo.GetColumnId(name));
        }
        return IndexInfo.GetColumns(columnIds);
    }

    TVector<std::pair<TString, NScheme::TTypeId>> GetKeyYqlSchema() const override {
        return IndexInfo.GetPK();
    }

    size_t NumIndexedRecords() const {
        Y_VERIFY(SelectInfo);
        return SelectInfo->NumRecords();
    }

    size_t NumIndexedBlobs() const {
        Y_VERIFY(SelectInfo);
        return SelectInfo->Stats().Blobs;
    }

    std::unique_ptr<NColumnShard::TScanIteratorBase> StartScan() const override;

    void Dump(IOutputStream& out) const override {
        out << "columns: " << (LoadSchema ? LoadSchema->num_fields() : 0)
            << " index records: " << NumIndexedRecords()
            << " index blobs: " << NumIndexedBlobs()
            << " committed blobs: " << CommittedBlobs.size()
            << " with program steps: " << Program.size()
            << (Sorting == ESorting::NONE ? " not" : (Sorting == ESorting::ASC ? " asc" : " desc"))
            << " sorted, at snapshot: " << PlanStep << ":" << TxId;
        if (GreaterPredicate) {
            out << " from{" << *GreaterPredicate << "}";
        }
        if (LessPredicate) {
            out << " to{" << *LessPredicate << "}";
        }
        if (SelectInfo) {
            out << ", " << *SelectInfo;
        }
    }

    friend IOutputStream& operator << (IOutputStream& out, const TReadMetadata& meta) {
        meta.Dump(out);
        return out;
    }
};

struct TReadStatsMetadata : public TReadMetadataBase, public std::enable_shared_from_this<TReadStatsMetadata> {
    using TConstPtr = std::shared_ptr<const TReadStatsMetadata>;

    const ui64 TabletId;
    TVector<ui32> ReadColumnIds;
    TVector<ui32> ResultColumnIds;
    THashMap<ui64, std::shared_ptr<NOlap::TColumnEngineStats>> IndexStats;

    explicit TReadStatsMetadata(ui64 tabletId)
        : TabletId(tabletId)
    {}

    TVector<std::pair<TString, NScheme::TTypeId>> GetResultYqlSchema() const override;

    TVector<std::pair<TString, NScheme::TTypeId>> GetKeyYqlSchema() const override;

    std::unique_ptr<NColumnShard::TScanIteratorBase> StartScan() const override;
};

// Represents a batch of rows produced by ASC or DESC scan with applied filters and partial aggregation
struct TPartialReadResult {
    std::shared_ptr<arrow::RecordBatch> ResultBatch;

    // This 1-row batch contains the last key that was read while producing the ResultBatch.
    // NOTE: it might be different from the Key of last row in ResulBatch in case of filtering/aggregation/limit
    std::shared_ptr<arrow::RecordBatch> LastReadKey;
};

class TIndexedReadData {
public:
    TIndexedReadData(NOlap::TReadMetadata::TConstPtr readMetadata)
        : ReadMetadata(readMetadata)
    {
        Y_VERIFY(ReadMetadata->SelectInfo);
    }

    /// @returns blobId -> granule map. Granules could be read independently
    THashMap<TBlobRange, ui64> InitRead(ui32 numNotIndexed, bool inGranulesOrder = false);

    /// @returns batches and corresponding last keys in correct order (i.e. sorted by by PK)
    TVector<TPartialReadResult> GetReadyResults(const int64_t maxRowsInBatch);

    void AddNotIndexed(ui32 batchNo, TString serializedBach, ui64 planStep, ui64 txId) {
        Y_VERIFY(batchNo < NotIndexed.size());
        if (!NotIndexed[batchNo]) {
            ++ReadyNotIndexed;
        }
        NotIndexed[batchNo] = MakeNotIndexedBatch(serializedBach, planStep, txId);
    }

    void AddIndexed(const TBlobRange& blobRange, const TString& column);
    size_t NumPortions() const { return PortionBatch.size(); }
    bool HasIndexRead() const { return WaitIndexed.size() || Indexed.size(); }

private:
    NOlap::TReadMetadata::TConstPtr ReadMetadata;
    ui32 FirstIndexedBatch{0};
    THashMap<TBlobRange, TString> Data;
    std::vector<std::shared_ptr<arrow::RecordBatch>> NotIndexed;
    THashMap<ui32, std::shared_ptr<arrow::RecordBatch>> Indexed;
    THashMap<ui32, THashSet<TBlobRange>> WaitIndexed;
    THashMap<TBlobRange, ui32> IndexedBlobs; // blobId -> batchNo
    ui32 ReadyNotIndexed{0};
    THashMap<ui64, std::shared_ptr<arrow::RecordBatch>> OutNotIndexed; // granule -> not indexed to append
    THashMap<ui64, TMap<ui64, std::shared_ptr<arrow::RecordBatch>>> ReadyGranules; // granule -> portions
    THashMap<ui64, ui32> PortionBatch; // portion -> batch
    TVector<ui64> BatchPortion; // batch -> portion
    THashMap<ui64, ui64> PortionGranule; // portion -> granule
    THashMap<ui64, ui32> GranuleWaits; // granule -> num portions to wait
    TDeque<ui64> GranulesOutOrder;
    TMap<ui64, ui64> TsGranules; // ts (key) -> granule
    THashSet<ui64> PortionsWithSelfDups;
    std::shared_ptr<NArrow::TSortDescription> SortReplaceDescription;

    const TIndexInfo& IndexInfo() const {
        return ReadMetadata->IndexInfo;
    }

    const TPortionInfo& Portion(ui32 batchNo) const {
        Y_VERIFY(batchNo >= FirstIndexedBatch);
        return ReadMetadata->SelectInfo->Portions[batchNo - FirstIndexedBatch];
    }

    ui64 BatchGranule(ui32 batchNo) const {
        Y_VERIFY(batchNo < BatchPortion.size());
        ui64 portion = BatchPortion[batchNo];
        Y_VERIFY(PortionGranule.count(portion));
        return PortionGranule.find(portion)->second;
    }

    std::shared_ptr<arrow::RecordBatch> MakeNotIndexedBatch(const TString& blob, ui64 planStep, ui64 txId) const;
    std::shared_ptr<arrow::RecordBatch> AssembleIndexedBatch(ui32 batchNo);
    void UpdateGranuleWaits(ui32 batchNo);
    THashMap<ui64, std::shared_ptr<arrow::RecordBatch>> SplitByGranules(
        std::vector<std::shared_ptr<arrow::RecordBatch>>&& batches) const;
    TVector<std::vector<std::shared_ptr<arrow::RecordBatch>>> ReadyToOut();
    TVector<TPartialReadResult> MakeResult(
        TVector<std::vector<std::shared_ptr<arrow::RecordBatch>>>&& granules,
        const int64_t maxRowsInBatch) const;
};

}
