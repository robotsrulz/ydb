#include "column_engine_logs.h"
#include "indexed_read_data.h"
#include "filter.h"
#include <ydb/core/formats/one_batch_input_stream.h>
#include <ydb/core/formats/merging_sorted_input_stream.h>

namespace NKikimr::NOlap {

std::shared_ptr<arrow::TimestampArray> GetTimestampColumn(const TIndexInfo& indexInfo,
                                                          const std::shared_ptr<arrow::RecordBatch>& batch) {
    TString columnName = indexInfo.GetPK()[0].first;
    std::string tsColumnName(columnName.data(), columnName.size());
    return NArrow::GetTypedColumn<arrow::TimestampArray>(batch, tsColumnName);
}

namespace {

arrow::ipc::IpcWriteOptions WriteOptions(const TCompression& compression) {
    auto& codec = compression.Codec;

    arrow::ipc::IpcWriteOptions options(arrow::ipc::IpcWriteOptions::Defaults());
    Y_VERIFY(arrow::util::Codec::IsAvailable(codec));
    arrow::Result<std::unique_ptr<arrow::util::Codec>> resCodec;
    if (compression.Level) {
        resCodec = arrow::util::Codec::Create(codec, *compression.Level);
        if (!resCodec.ok()) {
            resCodec = arrow::util::Codec::Create(codec);
        }
    } else {
        resCodec = arrow::util::Codec::Create(codec);
    }
    Y_VERIFY(resCodec.ok());

    options.codec.reset((*resCodec).release());
    options.use_threads = false;
    return options;
}

ui64 ExtractTimestamp(const std::shared_ptr<TPredicate>& pkPredicate, const std::shared_ptr<arrow::Schema>& key) {
    if (pkPredicate) {
        Y_VERIFY(pkPredicate->Good());
        Y_VERIFY(key->num_fields() == 1);
        Y_VERIFY(key->field(0)->Equals(pkPredicate->Batch->schema()->field(0)));

        auto array = NArrow::GetTypedColumn<arrow::TimestampArray>(pkPredicate->Batch, 0);
        Y_VERIFY(array && array->length() == 1);
        return array->Value(0);
    }
    return 0;
}

// Although source batches are ordered only by PK (sorting key) resulting pathBatches are ordered by extended key.
// They have const snapshot columns that do not break sorting inside batch.
std::shared_ptr<arrow::RecordBatch> AddSpecials(const TIndexInfo& indexInfo, const TInsertedData& inserted,
                                                const TString& data)
{
    Y_VERIFY(!data.empty(), "Blob data not present");

    auto batch = NArrow::DeserializeBatch(data, indexInfo.ArrowSchema());
    Y_VERIFY(batch);

    batch = TIndexInfo::AddSpecialColumns(batch, inserted.PlanStep(), inserted.TxId());
    Y_VERIFY(batch);

    return NArrow::ExtractColumns(batch, indexInfo.ArrowSchemaWithSpecials());
}

bool UpdateEvictedPortion(TPortionInfo& portionInfo, const TIndexInfo& indexInfo, const TString& tierName,
                          const THashMap<TBlobRange, TString>& srcBlobs,
                          TVector<TColumnRecord>& evictedRecords, TVector<TString>& newBlobs)
{
    Y_VERIFY(portionInfo.TierName != tierName);

    auto compression = indexInfo.GetTierCompression(tierName);
    if (!compression) {
        // Noting to recompress. We have no other kinds of evictions yet. Return.
        portionInfo.TierName = tierName;
        return true;
    }

    auto schema = indexInfo.ArrowSchemaWithSpecials();
    auto batch = portionInfo.AssembleInBatch(indexInfo, schema, srcBlobs);
    auto writeOptions = WriteOptions(*compression);

    TPortionInfo undo = portionInfo;
    size_t undoSize = newBlobs.size();

    std::vector<TString> blobs;
    for (auto& rec : portionInfo.Records) {
        auto colName = indexInfo.GetColumnName(rec.ColumnId);
        std::string name(colName.data(), colName.size());
        auto field = schema->GetFieldByName(name);

        auto blob = TPortionInfo::SerializeColumn(batch->GetColumnByName(name), field, writeOptions);
        if (blob.size() >= TPortionInfo::BLOB_BYTES_LIMIT) {
            portionInfo = undo;
            newBlobs.resize(undoSize);
            return false;
        }
        newBlobs.emplace_back(std::move(blob));
        rec.BlobRange = TBlobRange{};
    }

    for (auto& rec : undo.Records) {
        evictedRecords.emplace_back(std::move(rec));
    }

    portionInfo.AddMetadata(indexInfo, batch, tierName);
    return true;
}

TVector<TPortionInfo> MakeAppendedPortions(const TIndexInfo& indexInfo,
                                           std::shared_ptr<arrow::RecordBatch> batch,
                                           ui64 granule,
                                           const TSnapshot& minSnapshot,
                                           TVector<TString>& blobs) {
    Y_VERIFY(batch->num_rows());
    auto schema = indexInfo.ArrowSchemaWithSpecials();
    TVector<TPortionInfo> out;

    TString tierName = indexInfo.GetTierName(0);
    auto compression = indexInfo.GetTierCompression(0);
    if (!compression) {
        compression = indexInfo.GetDefaultCompression();
    }
    auto writeOptions = WriteOptions(*compression);

    std::shared_ptr<arrow::RecordBatch> portionBatch = batch;
    for (i32 pos = 0; pos < batch->num_rows();) {
        Y_VERIFY(portionBatch->num_rows());

        TPortionInfo portionInfo;
        portionInfo.Records.reserve(schema->num_fields());
        TVector<TString> portionBlobs;
        portionBlobs.reserve(schema->num_fields());

        // Serialize portion's columns into blobs

        bool ok = true;
        for (auto& field : schema->fields()) {
            const auto& name = field->name();
            ui32 columnId = indexInfo.GetColumnId(TString(name.data(), name.size()));

            /// @warnign records are not valid cause of empty BlobId and zero Portion
            TColumnRecord record = TColumnRecord::Make(granule, columnId, minSnapshot, 0);
            auto blob = portionInfo.AddOneChunkColumn(portionBatch->GetColumnByName(name), field, std::move(record),
                                                      writeOptions);
            if (!blob.size()) {
                ok = false;
                break;
            }

            // TODO: combine small columns in one blob
            portionBlobs.emplace_back(std::move(blob));
        }

        if (ok) {
            portionInfo.AddMetadata(indexInfo, portionBatch, tierName);
            out.emplace_back(std::move(portionInfo));
            for (auto& blob : portionBlobs) {
                blobs.push_back(blob);
            }
            pos += portionBatch->num_rows();
            if (pos < batch->num_rows()) {
                portionBatch = batch->Slice(pos);
            }
        } else {
            i64 halfLen = portionBatch->num_rows() / 2;
            Y_VERIFY(halfLen);
            portionBatch = batch->Slice(pos, halfLen);
        }
    }

    return out;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> PortionsToBatches(const TIndexInfo& indexInfo,
                                                                   const TVector<TPortionInfo>& portions,
                                                                   const THashMap<TBlobRange, TString>& blobs,
                                                                   bool insertedOnly = false) {
    // TODO: schema changes
    const std::shared_ptr<arrow::Schema> schema = indexInfo.ArrowSchemaWithSpecials();

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    batches.reserve(portions.size());

    for (auto& portionInfo : portions) {
        auto batch = portionInfo.AssembleInBatch(indexInfo, schema, blobs);
        if (!insertedOnly || portionInfo.IsInserted()) {
            batches.push_back(batch);
        }
    }
    return batches;
}

bool InitInGranuleMerge(TVector<TPortionInfo>& portions, const TCompactionLimits& limits, const TSnapshot& snap,
                        TMap<ui64, ui64>& borders) {
    ui64 oldTimePlanStep = snap.PlanStep - TDuration::Seconds(limits.InGranuleCompactSeconds).MilliSeconds();
    ui32 insertedCount = 0;
    ui32 insertedNew = 0;

    THashSet<ui64> filtered;
    THashSet<ui64> goodCompacted;
    {
        TMap<ui64, TVector<const TPortionInfo*>> points;

        for (auto& portionInfo : portions) {
            if (portionInfo.IsInserted()) {
                ++insertedCount;
                if (portionInfo.Snapshot().PlanStep > oldTimePlanStep) {
                    ++insertedNew;
                }
            } else if (portionInfo.BlobsSizes().second >= limits.GoodBlobSize) {
                goodCompacted.insert(portionInfo.Portion());
            }

            auto start = portionInfo.PkStart();
            auto end = portionInfo.PkEnd();
            Y_VERIFY(start && end);
            ui64 min = static_cast<const arrow::TimestampScalar&>(*start).value;
            ui64 max = static_cast<const arrow::TimestampScalar&>(*end).value;

            points[min].push_back(&portionInfo); // insert start
            points[max].push_back(nullptr); // insert end
        }

        ui32 bucketCounter = 0;
        int sum = 0;
        const TPortionInfo* lastPortion{};
        for (auto& [key, vec] : points) {
            for (auto& portionInfo : vec) {
                if (portionInfo) {
                    ++sum;
                    lastPortion = portionInfo;
                    ++bucketCounter;
                } else {
                    --sum;
                }
            }

            if (!sum) { // count(start) == count(end), start new range
                if (bucketCounter == 1) {
                    Y_VERIFY(lastPortion);

                    // We do not want to merge big compacted portions with inserted ones if there's no intersections.
                    ui64 maxBlobSize = lastPortion->BlobsSizes().second;
                    if (!lastPortion->IsInserted() && maxBlobSize >= limits.GoodBlobSize) {
                        filtered.insert(lastPortion->Portion());
                    }
                }
                bucketCounter = 0;
            }
        }
    }

    Y_VERIFY(insertedCount);
    // Trigger compaction if we have lots of inserted or if all inserted are old enough
    if (insertedNew && insertedCount < limits.InGranuleCompactInserts) {
        return false;
    }

    // Nothing to filter. Leave portions as is, no borders needed.
    if (filtered.empty() && goodCompacted.empty()) {
        return true;
    }

    // It's a map for SliceIntoGranules(). We use fake granule ids here to slice batch with borders.
    // We could merge inserted portions alltogether and slice result with filtered borders to prevent intersections.
    borders[0] = 0;

    TVector<TPortionInfo> tmp;
    tmp.reserve(portions.size());
    for (auto& portionInfo : portions) {
        // Prevent merge of compacted portions with no intersections
        if (filtered.count(portionInfo.Portion())) {
            auto start = portionInfo.PkStart();
            Y_VERIFY(start);
            ui64 ts = static_cast<const arrow::TimestampScalar&>(*start).value;
            borders[ts] = 0;
            // No need to add its end
        } else {
            // Merge good compacted portion with intersections but prevent its unneeded growth
            if (goodCompacted.count(portionInfo.Portion())) {
                // Add "first after end" border but do not add start: allow to merge with older or intersected data.
                // Do not add start to prevent [good] [small] [good] portions pattern.
                auto end = portionInfo.PkEnd();
                Y_VERIFY(end);
                ui64 ts = static_cast<const arrow::TimestampScalar&>(*end).value + 1;
                borders[ts] = 0;
            }

            tmp.emplace_back(std::move(portionInfo));
        }
    }
    tmp.swap(portions);

    if (borders.size() == 1) {
        Y_VERIFY(borders.begin()->first == 0);
        borders.clear();
    }

    ui32 counter = 0;
    for (auto& [ts, id] : borders) {
        id = ++counter;
    }
    return true;
}

TVector<const TPortionInfo*> GetActualPortions(const THashMap<ui64, TPortionInfo>& portions) {
    TVector<const TPortionInfo*> out;
    out.reserve(portions.size());
    for (auto& [portion, portionInfo] : portions) {
        if (portionInfo.IsActive()) {
            out.emplace_back(&portionInfo);
        }
    }
    return out;
}

template <typename T>
inline THashMap<ui64, std::shared_ptr<arrow::RecordBatch>> SliceIntoGranulesImpl(
    const std::shared_ptr<arrow::RecordBatch>& batch, const T& tsGranules, const TIndexInfo& indexInfo)
{
    THashMap<ui64, std::shared_ptr<arrow::RecordBatch>> out;

    if (tsGranules.size() == 1) {
        Y_VERIFY(tsGranules.begin()->first == 0);
        ui64 granule = tsGranules.begin()->second;
        out.emplace(granule, batch);
    } else {
        std::shared_ptr<arrow::TimestampArray> keyColumn = GetTimestampColumn(indexInfo, batch);
        Y_VERIFY(keyColumn && keyColumn->length() > 0);

        TVector<i64> borders;
        borders.reserve(tsGranules.size());
        for (auto& [ts, granule] : tsGranules) {
            borders.push_back(ts);
        }

        ui32 i = 0;
        int offset = 0;
        for (auto& [ts, granule] : tsGranules) {
            int end = keyColumn->length();
            if (i < borders.size() - 1) {
                i64 border = borders[i + 1];
                const auto* ptr = keyColumn->raw_values();
                end = std::lower_bound(ptr + offset, ptr + keyColumn->length(), border) - ptr;
            }

            int size = end - offset;
            if (size) {
                Y_VERIFY(size > 0);
                Y_VERIFY(!out.count(granule));
                out.emplace(granule, batch->Slice(offset, size));
            }

            offset = end;
            ++i;
        }
    }
    return out;
}

}

THashMap<ui64, std::shared_ptr<arrow::RecordBatch>>
SliceIntoGranules(const std::shared_ptr<arrow::RecordBatch>& batch, const TMap<ui64, ui64>& tsGranules,
                  const TIndexInfo& indexInfo)
{
    return SliceIntoGranulesImpl(batch, tsGranules, indexInfo);
}

THashMap<ui64, std::shared_ptr<arrow::RecordBatch>>
SliceIntoGranules(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<std::pair<ui64, ui64>>& tsGranules,
                  const TIndexInfo& indexInfo)
{
    return SliceIntoGranulesImpl(batch, tsGranules, indexInfo);
}

TColumnEngineForLogs::TColumnEngineForLogs(TIndexInfo&& info, ui64 tabletId, const TCompactionLimits& limits)
    : IndexInfo(info)
    , Limits(limits)
    , TabletId(tabletId)
    , LastPortion(0)
    , LastGranule(0)
{
    /// @note Setting replace and sorting key to PK we are able to:
    /// * apply REPLACE by MergeSort
    /// * apply PK predicate before REPLACE
    IndexInfo.SetAllKeys(IndexInfo.GetPK(), {0});

    ui32 indexId = IndexInfo.GetId();
    GranulesTable = std::make_shared<TGranulesTable>(indexId);
    ColumnsTable = std::make_shared<TColumnsTable>(indexId);
    CountersTable = std::make_shared<TCountersTable>(indexId);
}

ui64 TColumnEngineForLogs::MemoryUsage() const {
    ui64 numPortions = Counters.Inserted.Portions +
        Counters.Compacted.Portions +
        Counters.SplitCompacted.Portions +
        Counters.Inactive.Portions +
        Counters.Evicted.Portions;

    return Counters.Granules * (sizeof(TGranuleMeta) + sizeof(ui64)) +
        numPortions * (sizeof(TPortionInfo) + sizeof(ui64)) +
        Counters.ColumnRecords * sizeof(TColumnRecord) +
        Counters.ColumnMetadataBytes;
}

const TMap<ui64, std::shared_ptr<TColumnEngineStats>>& TColumnEngineForLogs::GetStats() const {
    return PathStats;
}

const TColumnEngineStats& TColumnEngineForLogs::GetTotalStats() {
    Counters.Tables = PathGranules.size();
    Counters.Granules = Granules.size();
    Counters.EmptyGranules = EmptyGranules.size();
    Counters.OverloadedGranules = 0;
    for (auto& [pathId, set] : PathsGranulesOverloaded) {
        Counters.OverloadedGranules += set.size();
    }

    return Counters;
}

void TColumnEngineForLogs::UpdatePortionStats(const TPortionInfo& portionInfo, bool isErase, bool isLoad) {
    UpdatePortionStats(Counters, portionInfo, isErase, isLoad);

    ui64 granule = portionInfo.Granule();
    Y_VERIFY(granule);
    Y_VERIFY(Granules.count(granule));
    ui64 pathId = Granules[granule]->PathId();
    Y_VERIFY(pathId);
    if (!PathStats.count(pathId)) {
        auto& stats = PathStats[pathId];
        stats = std::make_shared<TColumnEngineStats>();
        stats->Tables = 1;
    }
    UpdatePortionStats(*PathStats[pathId], portionInfo, isErase, isLoad);
}

void TColumnEngineForLogs::UpdatePortionStats(TColumnEngineStats& engineStats, const TPortionInfo& portionInfo,
                                              bool isErase, bool isLoad) const {
    ui64 columnRecords = portionInfo.Records.size();
    ui64 metadataBytes = 0;
    THashSet<TUnifiedBlobId> blobs;
    for (auto& rec : portionInfo.Records) {
        metadataBytes += rec.Metadata.size();
        blobs.insert(rec.BlobRange.BlobId);
    }

    ui32 rows = portionInfo.NumRows();
    ui64 rawBytes = portionInfo.RawBytesSum();
    ui64 bytes = 0;
    for (auto& blobId : blobs) {
        bytes += blobId.BlobSize();
    }

    TColumnEngineStats::TPortionsStats* srcStats = nullptr;
    switch (portionInfo.Meta.Produced) {
        case NOlap::TPortionMeta::UNSPECIFIED:
            Y_VERIFY(false); // unexpected
        case NOlap::TPortionMeta::INSERTED:
            srcStats = &engineStats.Inserted;
            break;
        case NOlap::TPortionMeta::COMPACTED:
            srcStats = &engineStats.Compacted;
            break;
        case NOlap::TPortionMeta::SPLIT_COMPACTED:
            srcStats = &engineStats.SplitCompacted;
            break;
        case NOlap::TPortionMeta::INACTIVE:
            srcStats = &engineStats.Inactive;
            break;
        case NOlap::TPortionMeta::EVICTED:
            srcStats = &engineStats.Evicted;
            break;
    }
    Y_VERIFY(srcStats);
    auto* stats = portionInfo.IsActive() ? srcStats : &engineStats.Inactive;

    if (isErase) { // PortionsToDrop
        engineStats.ColumnRecords -= columnRecords;
        engineStats.ColumnMetadataBytes -= metadataBytes;

        --stats->Portions;
        stats->Blobs -= blobs.size();
        stats->Rows -= rows;
        stats->Bytes -= bytes;
        stats->RawBytes -= rawBytes;
    } else if (isLoad || portionInfo.IsActive()) { // AppendedPortions
        engineStats.ColumnRecords += columnRecords;
        engineStats.ColumnMetadataBytes += metadataBytes;

        ++stats->Portions;
        stats->Blobs += blobs.size();
        stats->Rows += rows;
        stats->Bytes += bytes;
        stats->RawBytes += rawBytes;
    } else { // SwitchedPortions
        --srcStats->Portions;
        srcStats->Blobs -= blobs.size();
        srcStats->Rows -= rows;
        srcStats->Bytes -= bytes;
        srcStats->RawBytes -= rawBytes;

        ++stats->Portions;
        stats->Blobs += blobs.size();
        stats->Rows += rows;
        stats->Bytes += bytes;
        stats->RawBytes += rawBytes;
    }
}

void TColumnEngineForLogs::UpdateDefaultSchema(const TSnapshot& snapshot, TIndexInfo&& info) {
    // TODO(chertus): use step/txId for keeping older schema versions for older snapshots
    Y_UNUSED(snapshot);
    IndexInfo = std::move(info);
    // copied from constructor above
    IndexInfo.SetAllKeys(IndexInfo.GetPK(), {0});
}

bool TColumnEngineForLogs::Load(IDbWrapper& db, const THashSet<ui64>& pathsToDrop) {
    ClearIndex();

    if (!LoadGranules(db)) {
        return false;
    }
    if (!LoadColumns(db)) {
        return false;
    }
    if (!LoadCounters(db)) {
        return false;
    }

#if 0 // Clear index data
    for (auto& [granule, meta] : Granules) {
        for (auto& [portion, portionInfo] : meta->Portions) {
            for (auto& rec : portionInfo.Records) {
                ColumnsTable->Erase(db, rec);
            }
        }
        GranulesTable->Erase(db, meta->Record);
    }
    CountersTable->Write(db, LAST_PORTION, 0);
    CountersTable->Write(db, LAST_GRANULE, 0);
    CountersTable->Write(db, LAST_PLAN_STEP, 0);
    CountersTable->Write(db, LAST_TX_ID, 0);
    return true;
#endif

    THashSet<ui64> emptyGranulePaths;
    for (auto& [granule, spg] : Granules) {
        if (spg->Empty()) {
            EmptyGranules.insert(granule);
            emptyGranulePaths.insert(spg->PathId());
        } else {
            CompactionGranules.insert(granule);
            CleanupGranules.insert(granule);
        }
        for (auto& [_, portionInfo] : spg->Portions) {
            UpdatePortionStats(portionInfo, false, true);
        }
    }

    // Cleanup empty granules
    for (auto& pathId : emptyGranulePaths) {
        for (auto& emptyGranules : EmptyGranuleTracks(pathId)) {
            // keep first one => megre, keep nothing => drop.
            bool keepFirst = !pathsToDrop.count(pathId);
            for (auto& [ts, granule] : emptyGranules) {
                if (keepFirst) {
                    keepFirst = false;
                    continue;
                }

                Y_VERIFY(Granules.count(granule));
                auto spg = Granules[granule];
                Y_VERIFY(spg);
                GranulesTable->Erase(db, spg->Record);
                EraseGranule(pathId, granule, ts);
            }
        }
    }

    UpdateOverloaded(Granules);

    Y_VERIFY(!(LastPortion >> 63), "near to int overflow");
    Y_VERIFY(!(LastGranule >> 63), "near to int overflow");
    return true;
}

bool TColumnEngineForLogs::LoadGranules(IDbWrapper& db) {
    auto callback = [&](TGranuleRecord&& rec) {
        bool ok = SetGranule(rec, true);
        Y_VERIFY(ok);
    };

    return GranulesTable->Load(db, callback);
}

bool TColumnEngineForLogs::LoadColumns(IDbWrapper& db) {
    auto callback = [&](TColumnRecord&& row) {
        AddColumnRecord(row);
    };

    return ColumnsTable->Load(db, callback);
}

bool TColumnEngineForLogs::LoadCounters(IDbWrapper& db) {
    auto callback = [&](ui32 id, ui64 value) {
        switch (id) {
        case LAST_PORTION:
            LastPortion = value;
            break;
        case LAST_GRANULE:
            LastGranule = value;
            break;
        case LAST_PLAN_STEP:
            LastSnapshot.PlanStep = value;
            break;
        case LAST_TX_ID:
            LastSnapshot.TxId = value;
            break;
        }
    };

    return CountersTable->Load(db, callback);
}

std::shared_ptr<TColumnEngineChanges> TColumnEngineForLogs::StartInsert(TVector<TInsertedData>&& dataToIndex) {
    Y_VERIFY(dataToIndex.size());

    auto changes = std::make_shared<TChanges>(std::move(dataToIndex), Limits);
    ui32 reserveGranules = 0;

    changes->InitSnapshot = LastSnapshot;

    for (auto& data : changes->DataToIndex) {
        ui64 pathId = data.PathId;
        if (changes->PathToGranule.count(pathId)) {
            continue;
        }

        if (PathGranules.count(pathId)) {
            if (PathsGranulesOverloaded.count(pathId)) {
                return {};
            }

            // TODO: cache PathToGranule for hot pathIds
            const auto& src = PathGranules[pathId];
            auto& dst = changes->PathToGranule[pathId];
            dst.reserve(src.size());
            for (const auto& [ts, granule] : src) {
                dst.emplace_back(std::make_pair(ts, granule));
            }
        } else {
            // It could reserve more then needed in case of the same pathId in DataToIndex
            ++reserveGranules;
        }
    }

    if (reserveGranules) {
        changes->FirstGranuleId = LastGranule + 1;
        changes->ReservedGranuleIds = reserveGranules;
        LastGranule += reserveGranules;
    }

    return changes;
}

std::shared_ptr<TColumnEngineChanges> TColumnEngineForLogs::StartCompaction(std::unique_ptr<TCompactionInfo>&& info,
                                                                            const TSnapshot& outdatedSnapshot) {
    Y_VERIFY(info);

    auto changes = std::make_shared<TChanges>(std::move(info), Limits);
    changes->InitSnapshot = LastSnapshot;

    Y_VERIFY(changes->CompactionInfo->Granules.size() == 1);

    ui64 granule = *changes->CompactionInfo->Granules.begin();
    auto& portions = changes->SwitchedPortions;
    {
        auto spg = Granules.find(granule)->second;
        Y_VERIFY(spg);

        auto actualPortions = GetActualPortions(spg->Portions);
        Y_VERIFY(!actualPortions.empty());
        portions.reserve(actualPortions.size());

        for (auto* portionInfo : actualPortions) {
            Y_VERIFY(!portionInfo->Empty());
            Y_VERIFY(portionInfo->Granule() == granule);
            portions.push_back(*portionInfo);
        }
    }

    Y_VERIFY(Granules.count(granule));
    auto& spg = Granules[granule];
    Y_VERIFY(spg);
    ui64 pathId = spg->Record.PathId;
    Y_VERIFY(PathGranules.count(pathId));

    for (const auto& [ts, pathGranule] : PathGranules[pathId]) {
        if (pathGranule == granule) {
            changes->SrcGranule = {pathId, granule, ts};
            break;
        }
    }
    Y_VERIFY(changes->SrcGranule.Good());

    if (changes->CompactionInfo->InGranule) {
        TSnapshot completedSnap = (outdatedSnapshot < LastSnapshot) ? LastSnapshot : outdatedSnapshot;
        if (!InitInGranuleMerge(portions, Limits, completedSnap, changes->MergeBorders)) {
            return {};
        }
    } else {
        GranulesInSplit.insert(granule);
    }

    Y_VERIFY(!portions.empty());
    return changes;
}

std::shared_ptr<TColumnEngineChanges> TColumnEngineForLogs::StartCleanup(const TSnapshot& snapshot,
                                                                         THashSet<ui64>& pathsToDrop) {
    auto changes = std::make_shared<TChanges>(snapshot, Limits);

    // Add all portions from dropped paths
    THashSet<ui64> dropPortions;
    THashSet<ui64> activePathsToDrop;
    for (ui64 pathId : pathsToDrop) {
        if (!PathGranules.count(pathId)) {
            continue;
        }

        for (const auto& [_, granule]: PathGranules[pathId]) {
            Y_VERIFY(Granules.count(granule));
            auto spg = Granules[granule];
            Y_VERIFY(spg);
            for (auto& [portion, info] : spg->Portions) {
                changes->PortionsToDrop.push_back(info);
                dropPortions.insert(portion);
                activePathsToDrop.insert(pathId);
            }
        }
    }
    pathsToDrop.swap(activePathsToDrop);

    // Add stale portions of alive paths
    THashSet<ui64> activeCleanupGranules;
    for (ui64 granule : CleanupGranules) {
        auto spg = Granules.find(granule)->second;
        Y_VERIFY(spg);
        for (auto& [portion, info] : spg->Portions) {
            if (dropPortions.count(portion)) {
                continue;
            }

            if (!info.IsActive()) {
                activeCleanupGranules.insert(granule);
                if (info.XSnapshot() < snapshot) {
                    changes->PortionsToDrop.push_back(info);
                }
            }
        }
    }
    CleanupGranules.swap(activeCleanupGranules);

    return changes;
}

std::shared_ptr<TColumnEngineChanges> TColumnEngineForLogs::StartTtl(const THashMap<ui64, TTiersInfo>& pathTtls,
                                                                     ui64 maxEvictBytes) {
    if (pathTtls.empty()) {
        return {};
    }

    TSnapshot fakeSnapshot = {1, 1}; // TODO: better snapshot
    auto changes = std::make_shared<TChanges>(TColumnEngineChanges::TTL, fakeSnapshot);
    ui64 evicttionSize = 0;
    bool allowEviction = true;
    ui64 dropBlobs = 0;
    bool allowDrop = true;

    for (auto& [pathId, ttl] : pathTtls) {
        if (!PathGranules.count(pathId)) {
            continue; // It's not an error: allow TTL over multiple shards with different pathIds presented
        }

        if (!IndexInfo.AllowTtlOverColumn(ttl.Column)) {
            continue;
        }

        Y_VERIFY(!ttl.TierBorders.empty());

        ui32 ttlColumnId = IndexInfo.GetColumnId(ttl.Column);
        for (const auto& [ts, granule] : PathGranules[pathId]) {
            auto spg = Granules[granule];
            Y_VERIFY(spg);

            for (auto& [portion, info] : spg->Portions) {
                if (!info.IsActive()) {
                    continue;
                }

                allowEviction = (evicttionSize <= maxEvictBytes);
                allowDrop = (dropBlobs <= TCompactionLimits::MAX_BLOBS_TO_DELETE);

                if (auto max = info.MaxValue(ttlColumnId)) {
                    bool keep = false;
                    for (auto& border : ttl.TierBorders) {
                        if (NArrow::ScalarLess(*border.ToTimestamp(), *max)) {
                            keep = true;
                            if (allowEviction && info.TierName != border.TierName) {
                                evicttionSize += info.BlobsSizes().first;
                                changes->PortionsToEvict.emplace_back(info, border.TierName);
                            }
                            break;
                        }
                    }
                    if (!keep && allowDrop) {
                        Y_VERIFY(!NArrow::ScalarLess(*ttl.TierBorders.back().ToTimestamp(), *max));
                        dropBlobs += info.NumRecords();
                        changes->PortionsToDrop.push_back(info);
                    }
                }
            }
        }
    }

    if (changes->PortionsToDrop.empty() &&
        changes->PortionsToEvict.empty()) {
        return {};
    }

    if (!allowEviction || !allowDrop) {
        changes->NeedRepeat = true;
    }
    return changes;
}

TVector<TVector<std::pair<ui64, ui64>>> TColumnEngineForLogs::EmptyGranuleTracks(ui64 pathId) const {
    Y_VERIFY(PathGranules.count(pathId));
    const auto& pathGranules = PathGranules.find(pathId)->second;

    TVector<TVector<std::pair<ui64, ui64>>> emptyGranules;
    ui64 emptyStart = 0;
    for (const auto& [ts, granule]: pathGranules) {
        Y_VERIFY(Granules.count(granule));
        auto spg = Granules.find(granule)->second;
        Y_VERIFY(spg);

        if (spg->Empty()) {
            if (!emptyStart) {
                emptyGranules.push_back({});
                emptyStart = granule;
            }
            emptyGranules.back().emplace_back(ts, granule);
        } else if (emptyStart) {
            emptyStart = 0;
        }
    }

    return emptyGranules;
}

void TColumnEngineForLogs::UpdateOverloaded(const THashMap<ui64, std::shared_ptr<TGranuleMeta>>& granules) {
    for (auto [granule, spg] : granules) {
        if (!spg) {
            spg = Granules[granule];
        }
        Y_VERIFY(spg);
        ui64 pathId = spg->Record.PathId;

        ui64 size = 0;
        for (auto& [portion, portionInfo] : spg->Portions) {
            if (portionInfo.IsActive()) {
                size += portionInfo.BlobsSizes().first;
            }
        }

        if (size >= Limits.GranuleOverloadSize) {
            PathsGranulesOverloaded.emplace(pathId, granule);
        } else if (PathsGranulesOverloaded.count(pathId)) {
            auto& granules = PathsGranulesOverloaded[pathId];
            granules.erase(granule);
            if (granules.empty()) {
                PathsGranulesOverloaded.erase(pathId);
            }
        }
    }
}

bool TColumnEngineForLogs::ApplyChanges(IDbWrapper& db, std::shared_ptr<TColumnEngineChanges> indexChanges,
                                        const TSnapshot& snapshot) {
    auto changes = std::static_pointer_cast<TChanges>(indexChanges);
    if (changes->ApplySnapshot.Valid()) {
        Y_VERIFY(changes->ApplySnapshot == snapshot);
    }

    // Update tmp granules with real ids
    auto granuleRemap = changes->TmpToNewGranules(LastGranule);
    ui64 portion = LastPortion;
    for (auto& portionInfo : changes->AppendedPortions) {
        portionInfo.UpdateRecords(++portion, granuleRemap, snapshot);

        TPortionMeta::EProduced produced = TPortionMeta::INSERTED;
        // If it's a split compaction with moves appended portions are INSERTED (could have overlaps with others)
        if (changes->IsCompaction() && changes->PortionsToMove.empty()) {
            Y_VERIFY(changes->CompactionInfo);
            produced = changes->CompactionInfo->InGranule ?
                TPortionMeta::COMPACTED : TPortionMeta::SPLIT_COMPACTED;
        }

        portionInfo.UpdateRecordsMeta(produced);
    }

    for (auto& [portionInfo, _] : changes->PortionsToEvict) {
        portionInfo.UpdateRecordsMeta(TPortionMeta::EVICTED);
    }

    for (auto& [_, id] : changes->PortionsToMove) {
        Y_VERIFY(granuleRemap.count(id));
        id = granuleRemap[id];
    }

    // Set x-snapshot to switched portions
    if (changes->IsCompaction()) {
        Y_VERIFY(changes->SrcGranule.Good());

        /// @warning set granule not in split even if tx would be aborted later
        GranulesInSplit.erase(changes->SrcGranule.Granule);

        Y_VERIFY(changes->CompactionInfo);
        for (auto& portionInfo : changes->SwitchedPortions) {
            Y_VERIFY(portionInfo.IsActive());
            portionInfo.SetStale(snapshot);
        }
    }

    if (!ApplyChanges(db, *changes, snapshot, false)) { // validate only
        return false;
    }
    bool ok = ApplyChanges(db, *changes, snapshot, true);
    Y_VERIFY(ok);

    // Save updated granules for comapaction
    if (changes->IsInsert()) {
        for (auto& portionInfo : changes->AppendedPortions) {
            CompactionGranules.insert(portionInfo.Granule());
        }
    }

    // Save updated granules for cleanup
    if (changes->IsCompaction()) {
        Y_VERIFY(changes->CompactionInfo);
        for (auto& portionInfo : changes->SwitchedPortions) {
            CleanupGranules.insert(portionInfo.Granule());
        }
    } else if (changes->IsCleanup()) {
        for (auto& portionInfo : changes->PortionsToDrop) {
            ui64 granule = portionInfo.Granule();
            auto& meta = Granules[granule];
            Y_VERIFY(meta);
            if (meta->AllActive()) {
                CleanupGranules.erase(granule);
            }
        }
    }

    // Update overloaded granules (only if tx would be applyed)
    if (changes->IsInsert() || changes->IsCompaction() || changes->IsCleanup()) {
        THashMap<ui64, std::shared_ptr<TGranuleMeta>> granules;
        if (changes->IsCleanup()) {
            for (auto& portionInfo : changes->PortionsToDrop) {
                granules[portionInfo.Granule()] = {};
            }
        } else if (changes->IsCompaction() && !changes->CompactionInfo->InGranule) {
            granules[changes->SrcGranule.Granule] = {};
        } else {
            for (auto& portionInfo : changes->AppendedPortions) {
                granules[portionInfo.Granule()] = {};
            }
        }
        UpdateOverloaded(granules);
    }
    return true;
}

bool TColumnEngineForLogs::ApplyChanges(IDbWrapper& db, const TChanges& changes, const TSnapshot& snapshot, bool apply) {
    const TVector<TPortionInfo>* switchedPortions = nullptr;
    if (changes.IsCompaction()) {
        Y_VERIFY(changes.CompactionInfo);
        switchedPortions = &changes.SwitchedPortions;

        if (changes.CompactionInfo->InGranule) {
#if 0
            if (changes.SwitchedPortions.size() <= changes.AppendedPortions.size()) {
                LOG_S_ERROR("Cannot compact granule " << changes.SrcGranule.Granule << " at tablet " << TabletId);
                return false;
            }
#endif
        } else {
            if (changes.NewGranules.empty()) {
                LOG_S_ERROR("Cannot split granule " << changes.SrcGranule.Granule << " at tablet " << TabletId);
                return false;
            }
        }
    } else if (changes.IsInsert() && !CanInsert(changes, snapshot)) {
        LOG_S_INFO("Cannot insert " << changes.AppendedPortions.size() << " portions at tablet " << TabletId);
        return false;
    }

    // Save new granules

    for (auto& [granule, p] : changes.NewGranules) {
        ui64 pathId = p.first;
        ui64 ts = p.second;
        TString key((const char*)&ts, sizeof(ts));
        TGranuleRecord rec{pathId, key, granule, snapshot};

        if (!SetGranule(rec, apply)) {
            LOG_S_ERROR("Cannot insert granule " << rec << " at tablet " << TabletId);
            return false;
        }
        if (apply) {
            GranulesTable->Write(db, rec);
        }
    }

    // Update old portions (set stale snapshot)

    if (switchedPortions) {
        for (auto& portionInfo : *switchedPortions) {
            Y_VERIFY(!portionInfo.Empty());
            Y_VERIFY(!portionInfo.IsActive());

            ui64 granule = portionInfo.Granule();
            if (!Granules.count(granule)) {
                LOG_S_ERROR("Cannot update portion " << portionInfo << " with unknown granule at tablet " << TabletId);
                return false;
            }

            ui64 granuleMinTs = ExtractKey(Granules[granule]->Record.IndexKey);

            if (!apply) { // granule vs portion minPK
                auto pkScalar = portionInfo.PkStart();
                Y_VERIFY(pkScalar);
                ui64 portionMinTs = ExtractKey(*pkScalar);
                if (granuleMinTs > portionMinTs) {
                    LOG_S_ERROR("Cannot update invalid portion " << portionInfo << " minTs: " << portionMinTs
                        << " granule minTs: " << granuleMinTs << " at tablet " << TabletId);
                    return false;
                }
            }

            if (!UpsertPortion(portionInfo, apply)) {
                LOG_S_ERROR("Cannot update portion " << portionInfo << " at tablet " << TabletId);
                return false;
            }

            if (apply) {
                for (auto& record : portionInfo.Records) {
                    ColumnsTable->Write(db, record);
                }
            }
        }
    }

    // Update evicted portions
    // There could be race between compaction and eviction. Allow compaction and disallow eviction in this case.

    for (auto& [info, _] : changes.PortionsToEvict) {
        const auto& portionInfo = info;
        Y_VERIFY(!portionInfo.Empty());
        Y_VERIFY(portionInfo.IsActive());

        ui64 granule = portionInfo.Granule();
        ui64 portion = portionInfo.Portion();
        if (!Granules.count(granule) || !Granules[granule]->Portions.count(portion)) {
            LOG_S_ERROR("Cannot evict unknown portion " << portionInfo << " at tablet " << TabletId);
            return false;
        }

        // In case of race with compaction portion could become inactive
        // TODO: evict others instead of abort eviction
        auto& oldInfo = Granules[granule]->Portions[portion];
        if (!oldInfo.IsActive()) {
            LOG_S_WARN("Cannot evict inactive portion " << oldInfo << " at tablet " << TabletId);
            return false;
        }
        Y_VERIFY(portionInfo.TierName != oldInfo.TierName);

        // TODO: update stats
        if (!UpsertPortion(portionInfo, apply, false)) {
            LOG_S_ERROR("Cannot evict portion " << portionInfo << " at tablet " << TabletId);
            return false;
        }

        if (apply) {
            for (auto& record : portionInfo.Records) {
                ColumnsTable->Write(db, record);
            }
        }
    }

    // Move portions in granules (zero-copy switch + append into new granules)

    for (auto& [info, granule] : changes.PortionsToMove) {
        const auto& portionInfo = info;
        if (!ErasePortion(portionInfo, apply, false)) {
            LOG_S_ERROR("Cannot erase moved portion " << portionInfo << " at tablet " << TabletId);
            return false;
        }
        TPortionInfo moved = portionInfo;
        moved.SetGranule(granule);
        if (!UpsertPortion(moved, apply, false)) {
            LOG_S_ERROR("Cannot insert moved portion " << moved << " at tablet " << TabletId);
            return false;
        }
        if (apply) {
            for (auto& record : portionInfo.Records) {
                ColumnsTable->Erase(db, record);
            }
            for (auto& record : moved.Records) {
                ColumnsTable->Write(db, record);
            }
        }
    }

    // Drop old portions

    for (auto& portionInfo : changes.PortionsToDrop) {
        if (!ErasePortion(portionInfo, apply)) {
            LOG_S_ERROR("Cannot erase portion " << portionInfo << " at tablet " << TabletId);
            return false;
        }
        if (apply) {
            for (auto& record : portionInfo.Records) {
                ColumnsTable->Erase(db, record);
            }
        }
    }

    // Save new portions (their column records)

    for (auto& portionInfo : changes.AppendedPortions) {
        Y_VERIFY(!portionInfo.Empty());

        if (!apply) {
            ui64 granule = portionInfo.Records[0].Granule;
            if (!Granules.count(granule) && !changes.NewGranules.count(granule)) {
                LOG_S_ERROR("Cannot write portion with unknown granule " << portionInfo << " at tablet " << TabletId);
                return false;
            }

            // granule vs portion minPK
            ui64 granuleMinTs = 0;
            if (Granules.count(granule)) {
                granuleMinTs = ExtractKey(Granules[granule]->Record.IndexKey);
            } else {
                granuleMinTs = changes.NewGranules.find(granule)->second.second;
            }
            auto pkScalar = portionInfo.PkStart();
            Y_VERIFY(pkScalar);
            ui64 portionMinTs = ExtractKey(*pkScalar);
            if (granuleMinTs > portionMinTs) {
                LOG_S_ERROR("Cannot insert invalid portion " << portionInfo << " minTs: " << portionMinTs
                    << " granule minTs: " << granuleMinTs << " at tablet " << TabletId);
                return false;
            }
        }

        if (!UpsertPortion(portionInfo, apply)) {
            LOG_S_ERROR("Cannot insert portion " << portionInfo << " at tablet " << TabletId);
            return false;
        }

        if (apply) {
            for (auto& record : portionInfo.Records) {
                ColumnsTable->Write(db, record);
            }
        }
    }

    // Write counters

    if (apply) {
        LastPortion += changes.AppendedPortions.size();
        // Insert use early granules allocation. LastGranule increased there.
        if (!changes.IsInsert()) {
            LastGranule += changes.NewGranules.size();
        }
        CountersTable->Write(db, LAST_PORTION, LastPortion);
        CountersTable->Write(db, LAST_GRANULE, LastGranule);

        if (LastSnapshot < snapshot) {
            LastSnapshot = snapshot;
            CountersTable->Write(db, LAST_PLAN_STEP, LastSnapshot.PlanStep);
            CountersTable->Write(db, LAST_TX_ID, LastSnapshot.TxId);
        }
    }
    return true;
}

bool TColumnEngineForLogs::SetGranule(const TGranuleRecord& rec, bool apply) {
    ui64 ts = ExtractKey(rec.IndexKey);

    if (!apply) {
        if (Granules.count(rec.Granule)) {
            return false;
        }

        if (PathGranules.count(rec.PathId) && PathGranules[rec.PathId].count(ts)) {
            return false;
        }
        return true;
    }

    PathGranules[rec.PathId].emplace(ts, rec.Granule);
    auto& spg = Granules[rec.Granule];
    Y_VERIFY(!spg);
    spg = std::make_shared<TGranuleMeta>(rec);
    return true; // It must return true if (apply == true)
}

void TColumnEngineForLogs::EraseGranule(ui64 pathId, ui64 granule, ui64 ts) {
    Y_VERIFY(PathGranules.count(pathId));
    Y_VERIFY(Granules.count(granule));

    Granules.erase(granule);
    EmptyGranules.erase(granule);
    PathGranules[pathId].erase(ts);
}

bool TColumnEngineForLogs::UpsertPortion(const TPortionInfo& portionInfo, bool apply, bool updateStats) {
    Y_VERIFY(portionInfo.Valid());
    ui64 granule = portionInfo.Granule();

    if (!apply) {
        for (auto& record : portionInfo.Records) {
            if (granule != record.Granule || !record.Valid()) {
                return false;
            }
        }
        return true;
    }

    ui64 portion = portionInfo.Portion();
    auto& spg = Granules[granule];
    Y_VERIFY(spg);
    if (updateStats) {
        UpdatePortionStats(portionInfo);
    }
    spg->Portions[portion] = portionInfo;
    return true; // It must return true if (apply == true)
}

bool TColumnEngineForLogs::ErasePortion(const TPortionInfo& portionInfo, bool apply, bool updateStats) {
    Y_VERIFY(!portionInfo.Empty());
    ui64 granule = portionInfo.Granule();
    ui64 portion = portionInfo.Portion();

    if (!apply) {
        if (!Granules.count(granule)) {
            return false;
        }
        return true;
    }

    auto& spg = Granules[granule];
    Y_VERIFY(spg);
    if (spg->Portions.count(portion)) {
        if (updateStats) {
            UpdatePortionStats(spg->Portions[portion], true);
        }
        spg->Portions.erase(portion);
    } else {
        LOG_S_ERROR("Erase for unknown portion " << portionInfo << " at tablet " << TabletId);
    }
    return true; // It must return true if (apply == true)
}

void TColumnEngineForLogs::AddColumnRecord(const TColumnRecord& rec) {
    Y_VERIFY(rec.Valid());
    auto& spg = Granules[rec.Granule];
#if 0
    if (!spg) {
        LOG_S_ERROR("No granule " << rec.Granule << " for record " << rec << " at tablet " << TabletId);
        Granules.erase(rec.Granule);
        return;
    }
#else
    Y_VERIFY(spg);
#endif
    auto& portionInfo = spg->Portions[rec.Portion];
    portionInfo.AddRecord(IndexInfo, rec);
}

bool TColumnEngineForLogs::CanInsert(const TChanges& changes, const TSnapshot& commitSnap) const {
    Y_UNUSED(commitSnap);
    // Does insert have granule in split?
    for (auto& portionInfo : changes.AppendedPortions) {
        Y_VERIFY(!portionInfo.Empty());
        ui64 granule = portionInfo.Granule();
        if (GranulesInSplit.count(granule)) {
            LOG_S_DEBUG("Cannot insert into splitting granule " << granule << " at tablet " << TabletId);
            return false;
        }
    }
    // Does insert have already splitted granule?
    for (const auto& [pathId, tsGranules] : changes.PathToGranule) {
        if (PathGranules.count(pathId)) {
            const auto& actualGranules = PathGranules.find(pathId)->second;
            size_t expectedSize = tsGranules.size();
            if (actualGranules.size() != expectedSize) {
                LOG_S_DEBUG("Cannot insert into splitted granules (actual: " << actualGranules.size()
                    << ", expected: " << expectedSize << ") at tablet " << TabletId);
                return false;
            }
        }
    }
    return true;
}

TMap<TSnapshot, TVector<ui64>> TColumnEngineForLogs::GetOrderedPortions(ui64 granule, const TSnapshot& snapshot) const {
    Y_VERIFY(Granules.count(granule));
    auto& spg = Granules.find(granule)->second;
    Y_VERIFY(spg);

    TMap<TSnapshot, TVector<ui64>> out;
    for (auto& [portion, portionInfo] : spg->Portions) {
        if (portionInfo.Empty()) {
            continue;
        }

        TSnapshot recSnapshot = portionInfo.Snapshot();
        TSnapshot recXSnapshot = portionInfo.XSnapshot();

        bool visible = (recSnapshot <= snapshot);
        if (recXSnapshot.PlanStep) {
            visible = visible && snapshot < recXSnapshot;
        }

        if (visible) {
            out[recSnapshot].push_back(portion);
        }
    }
    return out;
}

std::shared_ptr<TSelectInfo> TColumnEngineForLogs::Select(ui64 pathId, TSnapshot snapshot,
                                                          const THashSet<ui32>& columnIds,
                                                          std::shared_ptr<TPredicate> from,
                                                          std::shared_ptr<TPredicate> to) const {
    auto out = std::make_shared<TSelectInfo>();
    if (!PathGranules.count(pathId)) {
        return out;
    }

    auto& pathGranules = PathGranules.find(pathId)->second;
    if (pathGranules.empty()) {
        return out;
    }
    out->Granules.reserve(pathGranules.size());
    // TODO: out.Portions.reserve()

    ui64 tsFrom = ExtractTimestamp(from, GetIndexKey());
    ui64 tsTo = ExtractTimestamp(to, GetIndexKey());

    // Apply FROM
    auto it = pathGranules.begin();
    if (tsFrom) {
        it = pathGranules.upper_bound(tsFrom);
        --it;
    }
    for (; it != pathGranules.end(); ++it) {
        ui64 ts = it->first;
        ui64 granule = it->second;

        // Apply TO
        if (to && ts > tsTo) {
            break;
        }

        Y_VERIFY(Granules.count(granule));
        auto& spg = Granules.find(granule)->second;
        Y_VERIFY(spg);
        auto& portions = spg->Portions;
        bool granuleHasDataForSnaphsot = false;

        TMap<TSnapshot, TVector<ui64>> orderedPortions = GetOrderedPortions(granule, snapshot);
        for (auto& [snap, vec] : orderedPortions) {
            for (auto& portion : vec) {
                auto& portionInfo = portions.find(portion)->second;

                TPortionInfo outPortion;
                outPortion.Records.reserve(columnIds.size());

                for (auto& rec : portionInfo.Records) {
                    Y_VERIFY(rec.Valid());
                    if (columnIds.count(rec.ColumnId)) {
                        outPortion.Records.push_back(rec);
                    }
                }
                out->Portions.emplace_back(std::move(outPortion));
                granuleHasDataForSnaphsot = true;
            }
        }

        if (granuleHasDataForSnaphsot) {
            out->Granules.push_back(spg->Record);
        }
    }

    return out;
}

static bool NeedSplit(const TVector<const TPortionInfo*>& actual, const TCompactionLimits& limits, ui32& inserted) {
    if (actual.size() < 2) {
        return false;
    }

    inserted = 0;
    ui64 sumSize = 0;
    ui64 sumMaxSize = 0;
    for (auto* portionInfo : actual) {
        Y_VERIFY(portionInfo);
        auto sizes = portionInfo->BlobsSizes();
        sumSize += sizes.first;
        sumMaxSize += sizes.second;
        if (portionInfo->IsInserted()) {
            ++inserted;
        }
    }

    return sumMaxSize >= limits.GranuleBlobSplitSize
        || sumSize >= limits.GranuleOverloadSize;
}

std::unique_ptr<TCompactionInfo> TColumnEngineForLogs::Compact() {
    auto info = std::make_unique<TCompactionInfo>();
    info->InGranule = true;
    auto& out = info->Granules;

    std::vector<ui64> goodGranules;
    for (ui64 granule : CompactionGranules) {
        auto spg = Granules.find(granule)->second;
        Y_VERIFY(spg);

        // We need only actual portions here (with empty XPlanStep:XTxId)
        auto actualPortions = GetActualPortions(spg->Portions);
        if (actualPortions.empty()) {
            continue;
        }

        ui32 inserted = 0;
        bool needSplit = NeedSplit(actualPortions, Limits, inserted);
        if (needSplit) {
            if (info->InGranule) {
                info->InGranule = false;
                out.clear(); // clear in-granule candidates, we have a splitting one
            }
            out.insert(granule);
        } else if (inserted) {
            if (info->InGranule) {
                out.insert(granule);
            }
        } else {
            goodGranules.push_back(granule);
        }
    }

    for (ui64 granule : goodGranules) {
        CompactionGranules.erase(granule);
    }

    if (!out.empty()) {
        return info;
    }
    return {};
}

TVector<TString> TColumnEngineForLogs::IndexBlobs(const TIndexInfo& indexInfo,
                                                  std::shared_ptr<TColumnEngineChanges> indexChanges) {
    auto changes = std::static_pointer_cast<TChanges>(indexChanges);
    Y_VERIFY(!changes->DataToIndex.empty());
    Y_VERIFY(changes->AppendedPortions.empty());
    Y_VERIFY(indexInfo.IsSorted());

    TSnapshot& minSnapshot = changes->ApplySnapshot;
    THashMap<ui64, std::vector<std::shared_ptr<arrow::RecordBatch>>> pathBatches;
    for (auto& inserted : changes->DataToIndex) {
        TSnapshot insertSnap{inserted.PlanStep(), inserted.TxId()};
        Y_VERIFY(insertSnap.Valid());
        if (minSnapshot.IsZero() || insertSnap <= minSnapshot) {
            minSnapshot = insertSnap;
        }

        TBlobRange blobRange(inserted.BlobId, 0, inserted.BlobId.BlobSize());
        auto* blobData = changes->Blobs.FindPtr(blobRange);
        Y_VERIFY(blobData, "Data for range %s has not been read", blobRange.ToString().c_str());
        auto batch = AddSpecials(indexInfo, inserted, *blobData);
        pathBatches[inserted.PathId].push_back(batch);
        Y_VERIFY_DEBUG(NArrow::IsSorted(pathBatches[inserted.PathId].back(), indexInfo.GetReplaceKey()));
    }
    Y_VERIFY(minSnapshot.Valid());

    TVector<TString> blobs;

    for (auto& [pathId, batches] : pathBatches) {
        changes->AddPathIfNotExists(pathId);

        // We could merge data here cause tablet limits indexing data portions
        auto merged = NArrow::CombineSortedBatches(batches, indexInfo.SortDescription()); // insert: no replace
        Y_VERIFY(merged);
        Y_VERIFY_DEBUG(NArrow::IsSorted(merged, indexInfo.GetReplaceKey()));

        auto granuleBatches = SliceIntoGranules(merged, changes->PathToGranule[pathId], indexInfo);
        for (auto& [granule, batch] : granuleBatches) {
            auto portions = MakeAppendedPortions(indexInfo, batch, granule, minSnapshot, blobs);
            Y_VERIFY(portions.size() > 0);
            for (auto& portion : portions) {
                changes->AppendedPortions.emplace_back(std::move(portion));
            }
        }
    }

    Y_VERIFY(changes->PathToGranule.size() == pathBatches.size());
    return blobs;
}

static std::shared_ptr<arrow::RecordBatch> CompactInOneGranule(const TIndexInfo& indexInfo, ui64 granule,
                                                               const TVector<TPortionInfo>& portions,
                                                               const THashMap<TBlobRange, TString>& blobs) {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    batches.reserve(portions.size());

    auto schema = indexInfo.ArrowSchemaWithSpecials();
    for (auto& portionInfo : portions) {
        Y_VERIFY(!portionInfo.Empty());
        Y_VERIFY(portionInfo.Granule() == granule);

        auto batch = portionInfo.AssembleInBatch(indexInfo, schema, blobs);
        batches.push_back(batch);
    }

    auto sortedBatch = NArrow::CombineSortedBatches(batches, indexInfo.SortReplaceDescription());
    Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(sortedBatch, indexInfo.GetReplaceKey()));

    return sortedBatch;
}

static TVector<TString> CompactInGranule(const TIndexInfo& indexInfo,
                                         std::shared_ptr<TColumnEngineForLogs::TChanges> changes) {
    TVector<TString> blobs;
    auto& switchedProtions = changes->SwitchedPortions;
    Y_VERIFY(switchedProtions.size());

    ui64 granule = switchedProtions[0].Granule();
    auto batch = CompactInOneGranule(indexInfo, granule, switchedProtions, changes->Blobs);

    TVector<TPortionInfo> portions;
    if (changes->MergeBorders.size()) {
        Y_VERIFY(changes->MergeBorders.size() > 1);
        auto slices = SliceIntoGranules(batch, changes->MergeBorders, indexInfo);
        portions.reserve(slices.size());

        for (auto& [_, slice] : slices) {
            if (!slice || slice->num_rows() == 0) {
                continue;
            }
            auto tmp = MakeAppendedPortions(indexInfo, slice, granule, TSnapshot{}, blobs);
            for (auto&& portionInfo : tmp) {
                portions.emplace_back(std::move(portionInfo));
            }
        }
    } else {
        portions = MakeAppendedPortions(indexInfo, batch, granule, TSnapshot{}, blobs);
    }

    Y_VERIFY(portions.size() > 0);
    for (auto& portion : portions) {
        changes->AppendedPortions.emplace_back(std::move(portion));
    }

    return blobs;
}

/// @return vec({ts, batch}). ts0 <= ts1 <= ... <= tsN
/// @note We use ts from PK for split but there could be lots PK with the same ts.
static TVector<std::pair<ui64, std::shared_ptr<arrow::RecordBatch>>>
SliceGranuleBatches(const TIndexInfo& indexInfo, const TColumnEngineForLogs::TChanges& changes,
                    std::vector<std::shared_ptr<arrow::RecordBatch>>&& batches, i64 ts0) {
    TVector<std::pair<ui64, std::shared_ptr<arrow::RecordBatch>>> out;

    // Extract unique effective key (timestamp) and their counts
    i64 numRows = 0;
    TMap<ui64, ui32> uniqKeyCount;
    for (auto& batch : batches) {
        numRows += batch->num_rows();

        std::shared_ptr<arrow::TimestampArray> keyColumn = GetTimestampColumn(indexInfo, batch);
        Y_VERIFY(keyColumn && keyColumn->length() > 0);

        for (int pos = 0; pos < keyColumn->length(); ++pos) {
            ui64 ts = keyColumn->Value(pos);
            ++uniqKeyCount[ts];
        }
    }

    Y_VERIFY(uniqKeyCount.size());
    i64 minTs = uniqKeyCount.begin()->first;
    i64 maxTs = uniqKeyCount.rbegin()->first;
    Y_VERIFY(minTs >= ts0);

    // It's an estimation of needed count cause numRows calculated before key replaces
    ui32 numSplitInto = changes.NumSplitInto(numRows);
    ui32 rowsInGranule = numRows / numSplitInto;
    Y_VERIFY(rowsInGranule);

    // Cannot split in case of one unique key
    if (uniqKeyCount.size() == 1) {
        // We have to split big batch of same key in several portions
        auto merged = NArrow::MergeSortedBatches(batches, indexInfo.SortReplaceDescription(), rowsInGranule);
        for (auto& batch : merged) {
            Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(batch, indexInfo.GetReplaceKey()));
            out.emplace_back(ts0, batch);
        }
        return out;
    }

    // Make split borders from uniq keys
    TVector<i64> borders;
    borders.reserve(numRows / rowsInGranule);
    {
        ui32 sumRows = 0;
        for (auto& [ts, num] : uniqKeyCount) {
            if (sumRows >= rowsInGranule) {
                borders.emplace_back(ts);
                sumRows = 0;
            }
            sumRows += num;
        }
        if (borders.empty()) {
            borders.emplace_back(maxTs); // huge trailing key
        }
        Y_VERIFY(borders.size());
    }

    // Find offsets in source batches
    TVector<TVector<int>> offsets(batches.size()); // vec[batch][border] = offset
    for (size_t i = 0; i < batches.size(); ++i) {
        auto& batch = batches[i];
        auto& batchOffsets = offsets[i];
        batchOffsets.reserve(borders.size() + 1);

        std::shared_ptr<arrow::TimestampArray> keyColumn = GetTimestampColumn(indexInfo, batch);
        Y_VERIFY(keyColumn && keyColumn->length() > 0);

        batchOffsets.push_back(0);
        for (i64 border : borders) {
            const auto* start = keyColumn->raw_values() + batchOffsets.back();
            const auto* end = keyColumn->raw_values() + keyColumn->length();
            const auto* borderPos = std::lower_bound(start, end, border);
            int offset = borderPos - keyColumn->raw_values();
            Y_VERIFY(offset >= batchOffsets.back());
            batchOffsets.push_back(offset);
        }

        Y_VERIFY(batchOffsets.size() == borders.size() + 1);
    }

    // Make merge-sorted granule batch for each splitted granule
    for (ui32 granuleNo = 0; granuleNo < borders.size() + 1; ++granuleNo) {
        std::vector<std::shared_ptr<arrow::RecordBatch>> granuleBatches;
        granuleBatches.reserve(batches.size());

        // Extract granule: slice source batches with offsets
        i64 granuleNumRows = 0;
        for (size_t i = 0; i < batches.size(); ++i) {
            auto& batch = batches[i];
            auto& batchOffsets = offsets[i];

            int offset = batchOffsets[granuleNo];
            int end = batch->num_rows();
            if (granuleNo < borders.size()) {
                end = batchOffsets[granuleNo + 1];
            }
            int size = end - offset;
            Y_VERIFY(size >= 0);

            if (size) {
                auto slice = batch->Slice(offset, size);
                Y_VERIFY(slice->num_rows());
                granuleNumRows += slice->num_rows();
#if 1 // Check correctness
                std::shared_ptr<arrow::TimestampArray> keyColumn = GetTimestampColumn(indexInfo, slice);
                Y_VERIFY(keyColumn && keyColumn->length() > 0);

                i64 startKey = granuleNo ? borders[granuleNo - 1] : minTs;
                Y_VERIFY(keyColumn->Value(0) >= startKey);
                if (granuleNo < borders.size() - 1) {
                    i64 endKey = borders[granuleNo];
                    Y_VERIFY(keyColumn->Value(keyColumn->length() - 1) < endKey);
                } else {
                    Y_VERIFY(keyColumn->Value(keyColumn->length() - 1) <= maxTs);
                }
#endif
                Y_VERIFY_DEBUG(NArrow::IsSorted(slice, indexInfo.GetReplaceKey()));
                granuleBatches.emplace_back(slice);
            }
        }

        // Merge slices. We have to split a big key batches in several ones here.
        if (granuleNumRows > 4 * rowsInGranule) {
            granuleNumRows = rowsInGranule;
        }
        auto merged = NArrow::MergeSortedBatches(granuleBatches, indexInfo.SortReplaceDescription(), granuleNumRows);
        for (auto& batch : merged) {
            Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(batch, indexInfo.GetReplaceKey()));

            i64 startKey = ts0;
            if (granuleNo) {
                startKey = borders[granuleNo - 1];
            }
#if 1 // Check correctness
            std::shared_ptr<arrow::TimestampArray> keyColumn = GetTimestampColumn(indexInfo, batch);
            Y_VERIFY(keyColumn && keyColumn->length() > 0);
            Y_VERIFY(keyColumn->Value(0) >= startKey);
#endif
            out.emplace_back(startKey, batch);
        }
    }

    return out;
}

static ui64 TryMovePortions(TVector<TPortionInfo>& portions, TMap<ui64, ui64>& tsIds,
                            TVector<std::pair<TPortionInfo, ui64>>& toMove, i64 ts0) {
    std::vector<const TPortionInfo*> compacted;
    compacted.reserve(portions.size());
    std::vector<const TPortionInfo*> inserted;
    inserted.reserve(portions.size());

    for (auto& portionInfo : portions) {
        if (portionInfo.IsInserted()) {
            inserted.push_back(&portionInfo);
        } else {
            compacted.push_back(&portionInfo);
        }
    }

    if (compacted.size() < 2) {
        return 0;
    }

    std::sort(compacted.begin(), compacted.end(), [](const TPortionInfo* a, const TPortionInfo* b) {
        return NArrow::ScalarLess(*a->PkStart(), *b->PkStart());
    });

    for (size_t i = 0; i < compacted.size() - 1; ++i) {
        if (!NArrow::ScalarLess(*compacted[i]->PkEnd(), *compacted[i + 1]->PkStart())) {
            return 0;
        }
    }

    toMove.reserve(compacted.size());
    ui64 numRows = 0;
    ui32 counter = 0;
    for (auto* portionInfo : compacted) {
        ui64 ts = ts0;
        if (counter) {
            ts = static_cast<const arrow::TimestampScalar&>(*portionInfo->PkStart()).value;
        }

        ui32 rows = portionInfo->NumRows();
        Y_VERIFY(rows);
        numRows += rows;
        tsIds.emplace(ts, counter + 1);
        toMove.emplace_back(std::move(*portionInfo), counter);
        ++counter;
    }

    std::vector<TPortionInfo> out;
    out.reserve(inserted.size());
    for (auto* portionInfo : inserted) {
        out.emplace_back(std::move(*portionInfo));
    }
    portions.swap(out);

    return numRows;
}

static TVector<TString> CompactSplitGranule(const TIndexInfo& indexInfo,
                                            std::shared_ptr<TColumnEngineForLogs::TChanges> changes) {
    ui64 pathId = changes->SrcGranule.PathId;
    //ui64 granule = changes->SrcGranule.Granule;
    ui64 ts0 = changes->SrcGranule.Ts;
    TVector<TPortionInfo>& portions = changes->SwitchedPortions;

    TMap<ui64, ui64> tsIds;
    ui64 movedRows = TryMovePortions(portions, tsIds, changes->PortionsToMove, ts0);
    auto srcBatches = PortionsToBatches(indexInfo, portions, changes->Blobs, (bool)movedRows);
    Y_VERIFY(srcBatches.size() == portions.size());

    TVector<TString> blobs;

    if (movedRows) {
        Y_VERIFY(changes->PortionsToMove.size() >= 2);
        Y_VERIFY(changes->PortionsToMove.size() == tsIds.size());
        Y_VERIFY(tsIds.begin()->first == ts0);

        ui64 numRows = movedRows;
        for (auto& batch : srcBatches) {
            numRows += batch->num_rows();
        }

        // Recalculate new granules borders (if they are larger then portions)
        ui32 numSplitInto = changes->NumSplitInto(numRows);
        if (numSplitInto < tsIds.size()) {
            ui32 rowsInGranule = numRows / numSplitInto;
            Y_VERIFY(rowsInGranule);

            TMap<ui64, ui64> newTsIds;
            ui64 lastTs = tsIds.rbegin()->first;
            ui32 tmpGranule = 0;
            ui32 sumRows = 0;
            ui32 i = 0;
            for (auto& [ts, _] : tsIds) {
                if (sumRows >= rowsInGranule || (ts == lastTs && newTsIds.empty())) {
                    ++tmpGranule;
                    newTsIds.emplace(ts, tmpGranule + 1);
                    sumRows = 0;
                }

                auto& toMove = changes->PortionsToMove[i];
                sumRows += toMove.first.NumRows();
                toMove.second = tmpGranule;
                ++i;
            }

            newTsIds[ts0] = 1;
            tsIds.swap(newTsIds);
        }
        Y_VERIFY(tsIds.size() > 1);

        // Slice inserted portions with granules' borders
        THashMap<ui64, std::vector<std::shared_ptr<arrow::RecordBatch>>> idBatches;
        TVector<TPortionInfo*> toSwitch;
        toSwitch.reserve(portions.size());
        for (size_t i = 0; i < portions.size(); ++i) {
            auto& portion = portions[i];
            auto& batch = srcBatches[i];
            auto slices = SliceIntoGranules(batch, tsIds, indexInfo);

            THashSet<ui64> ids;
            for (auto& [id, slice] : slices) {
                if (slice && slice->num_rows()) {
                    ids.insert(id);
                    idBatches[id].emplace_back(std::move(slice));
                }
            }

            // Optimization: move not splitted inserted portions. Do not reappend them.
            if (ids.size() == 1) {
                ui64 id = *ids.begin();
                idBatches[id].resize(idBatches[id].size() - 1);
                ui64 tmpGranule = id - 1;
                changes->PortionsToMove.emplace_back(std::move(portion), tmpGranule);
            } else {
                toSwitch.push_back(&portion);
            }
        }

        // Update switchedPortions if we have moves
        if (toSwitch.size() != portions.size()) {
            TVector<TPortionInfo> tmp;
            tmp.reserve(toSwitch.size());
            for (auto* portionInfo : toSwitch) {
                tmp.emplace_back(std::move(*portionInfo));
            }
            portions.swap(tmp);
        }

        for (auto& [ts, id] : tsIds) {
            ui64 tmpGranule = changes->SetTmpGranule(pathId, ts);

            for (auto& batch : idBatches[id]) {
                // Cannot set snapshot here. It would be set in committing transaction in ApplyChanges().
                auto newPortions = MakeAppendedPortions(indexInfo, batch, tmpGranule, TSnapshot{}, blobs);
                Y_VERIFY(newPortions.size() > 0);
                for (auto& portion : newPortions) {
                    changes->AppendedPortions.emplace_back(std::move(portion));
                }
            }
        }
    } else {
        auto batches = SliceGranuleBatches(indexInfo, *changes, std::move(srcBatches), ts0);

        changes->SetTmpGranule(pathId, ts0);
        for (auto& [ts, batch] : batches) {
            // Tmp granule would be updated to correct value in ApplyChanges()
            ui64 tmpGranule = changes->SetTmpGranule(pathId, ts);

            // Cannot set snapshot here. It would be set in committing transaction in ApplyChanges().
            auto portions = MakeAppendedPortions(indexInfo, batch, tmpGranule, TSnapshot{}, blobs);
            Y_VERIFY(portions.size() > 0);
            for (auto& portion : portions) {
                changes->AppendedPortions.emplace_back(std::move(portion));
            }
        }
    }

    return blobs;
}

TVector<TString> TColumnEngineForLogs::CompactBlobs(const TIndexInfo& indexInfo,
                                                    std::shared_ptr<TColumnEngineChanges> changes) {
    Y_VERIFY(changes);
    Y_VERIFY(changes->CompactionInfo);
    Y_VERIFY(changes->DataToIndex.empty()); // not used
    Y_VERIFY(!changes->Blobs.empty()); // src data
    Y_VERIFY(!changes->SwitchedPortions.empty()); // src meta
    Y_VERIFY(changes->AppendedPortions.empty()); // dst meta

    auto castedChanges = std::static_pointer_cast<TChanges>(changes);
    if (castedChanges->CompactionInfo->InGranule) {
        return CompactInGranule(indexInfo, castedChanges);
    }
    return CompactSplitGranule(indexInfo, castedChanges);
}

TVector<TString> TColumnEngineForLogs::EvictBlobs(const TIndexInfo& indexInfo,
                                                  std::shared_ptr<TColumnEngineChanges> changes) {
    Y_VERIFY(changes);
    Y_VERIFY(!changes->Blobs.empty()); // src data
    Y_VERIFY(!changes->PortionsToEvict.empty()); // src meta
    Y_VERIFY(changes->EvictedRecords.empty()); // dst meta

    TVector<TString> newBlobs;
    TVector<std::pair<TPortionInfo, TString>> evicted;
    evicted.reserve(changes->PortionsToEvict.size());

    for (auto& [portionInfo, tierName] : changes->PortionsToEvict) {
        Y_VERIFY(!portionInfo.Empty());
        Y_VERIFY(portionInfo.IsActive());

        if (UpdateEvictedPortion(portionInfo, indexInfo, tierName, changes->Blobs, changes->EvictedRecords, newBlobs)) {
            Y_VERIFY(portionInfo.TierName == tierName);
            evicted.emplace_back(std::move(portionInfo), TString{});
        }
    }

    changes->PortionsToEvict.swap(evicted);
    return newBlobs;
}

}
