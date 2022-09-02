#include <library/cpp/testing/unittest/registar.h>
#include "column_engine_logs.h"
#include "predicate.h"


namespace NKikimr {

using namespace NOlap;
namespace NTypeIds = NScheme::NTypeIds;
using TTypeId = NScheme::TTypeId;

namespace {

class TTestDbWrapper : public IDbWrapper {
public:
    struct TIndex {
        THashMap<ui64, TVector<TGranuleRecord>> Granules; // pathId -> granule
        THashMap<ui64, TVector<TColumnRecord>> Columns; // granule -> columns
        THashMap<ui32, ui64> Counters;
    };

    void Insert(const TInsertedData& data) override {
        Inserted.emplace(TWriteId{data.WriteTxId}, data);
    }

    void Commit(const TInsertedData& data) override {
        Committed[data.PathId].emplace(data);
    }

    void Abort(const TInsertedData& data) override {
        Aborted.emplace(TWriteId{data.WriteTxId}, data);
    }

    void EraseInserted(const TInsertedData& data) override {
        Inserted.erase(TWriteId{data.WriteTxId});
    }

    void EraseCommitted(const TInsertedData& data) override {
        Committed[data.PathId].erase(data);
    }

    void EraseAborted(const TInsertedData& data) override {
        Aborted.erase(TWriteId{data.WriteTxId});
    }

    bool Load(THashMap<TWriteId, TInsertedData>& inserted,
              THashMap<ui64, TSet<TInsertedData>>& committed,
              THashMap<TWriteId, TInsertedData>& aborted,
              const TInstant&) override {
        inserted = Inserted;
        committed = Committed;
        aborted = Aborted;
        return true;
    }

    void WriteGranule(ui32 index, const TGranuleRecord& row) override {
        auto& granules = Indices[index].Granules[row.PathId];

        bool replaced = false;
        for (auto& rec : granules) {
            if (rec == row) {
                rec = row;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            granules.push_back(row);
        }
    }

    void EraseGranule(ui32 index, const TGranuleRecord& row) override {
        auto& pathGranules = Indices[index].Granules[row.PathId];

        TVector<TGranuleRecord> filtered;
        filtered.reserve(pathGranules.size());
        for (const TGranuleRecord& rec : pathGranules) {
            if (rec.Granule != row.Granule) {
                filtered.push_back(rec);
            }
        }
        pathGranules.swap(filtered);
    }

    bool LoadGranules(ui32 index, std::function<void(TGranuleRecord&&)> callback) override {
        auto& granules = Indices[index].Granules;
        for (auto& [pathId, vec] : granules) {
            for (auto& rec : vec) {
                TGranuleRecord tmp = rec;
                callback(std::move(rec));
            }
        }
        return true;
    }

    void WriteColumn(ui32 index, const TColumnRecord& row) override {
        auto& columns = Indices[index].Columns[row.Granule];

        bool replaced = false;
        for (auto& rec : columns) {
            if (rec == row) {
                rec = row;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            columns.push_back(row);
        }
    }

    void EraseColumn(ui32 index, const TColumnRecord& row) override {
        auto& columns = Indices[index].Columns[row.Granule];

        TVector<TColumnRecord> filtered;
        filtered.reserve(columns.size());
        for (auto& rec : columns) {
            if (rec != row) {
                filtered.push_back(rec);
            }
        }
        columns.swap(filtered);
    }

    bool LoadColumns(ui32 index, std::function<void(TColumnRecord&&)> callback) override {
        auto& columns = Indices[index].Columns;
        for (auto& [granule, vec] : columns) {
            for (auto& rec : vec) {
                TColumnRecord tmp = rec;
                callback(std::move(rec));
            }
        }
        return true;
    }

    void WriteCounter(ui32 index, ui32 counterId, ui64 value) override {
        auto& counters = Indices[index].Counters;
        counters[counterId] = value;
    }

    bool LoadCounters(ui32 index, std::function<void(ui32 id, ui64 value)> callback) override {
        auto& counters = Indices[index].Counters;
        for (auto& [id, value] : counters) {
            callback(id, value);
        }
        return true;
    }

private:
    THashMap<TWriteId, TInsertedData> Inserted;
    THashMap<ui64, TSet<TInsertedData>> Committed;
    THashMap<TWriteId, TInsertedData> Aborted;
    THashMap<ui32, TIndex> Indices;
};

static const TVector<std::pair<TString, TTypeId>> testColumns = {
    // PK
    {"timestamp", NTypeIds::Timestamp },
    {"resource_type", NTypeIds::Utf8 },
    {"resource_id", NTypeIds::Utf8 },
    {"uid", NTypeIds::Utf8 },
    //
    {"message", NTypeIds::Utf8 }
};

static const TVector<std::pair<TString, TTypeId>> testKey = {
    {"timestamp", NTypeIds::Timestamp },
    {"resource_type", NTypeIds::Utf8 },
    {"resource_id", NTypeIds::Utf8 },
    {"uid", NTypeIds::Utf8 }
};

TIndexInfo TestTableInfo() {
    TIndexInfo indexInfo("", 0);

    for (ui32 i = 0; i < testColumns.size(); ++i) {
        ui32 id = i + 1;
        auto& name = testColumns[i].first;
        auto& type = testColumns[i].second;

        indexInfo.Columns[id] = NTable::TColumn(name, id, type);
        indexInfo.ColumnNames[name] = id;
    }

    for (const auto& [keyName, keyType] : testKey) {
        indexInfo.KeyColumns.push_back(indexInfo.ColumnNames[keyName]);
    }

    indexInfo.AddTtlColumn(testColumns[0].first);

    return indexInfo;
}

class TBuilder {
public:
    struct TRow {
        ui64 Timestamp;
        std::string ResourceType;
        std::string ResourceId;
        std::string Uid;
        std::string Message;
    };

    TBuilder()
        : Schema(NArrow::MakeArrowSchema(testColumns))
    {
        auto status = arrow::RecordBatchBuilder::Make(Schema, arrow::default_memory_pool(), &BatchBuilder);
        Y_VERIFY(status.ok());
    }

    bool AddRow(const TRow& row) {
        bool ok = true;
        ok = ok && BatchBuilder->GetFieldAs<arrow::TypeTraits<arrow::TimestampType>::BuilderType>(0)->Append(row.Timestamp).ok();
        ok = ok && BatchBuilder->GetFieldAs<arrow::TypeTraits<arrow::StringType>::BuilderType>(1)->Append(row.ResourceType).ok();
        ok = ok && BatchBuilder->GetFieldAs<arrow::TypeTraits<arrow::StringType>::BuilderType>(2)->Append(row.ResourceId).ok();
        ok = ok && BatchBuilder->GetFieldAs<arrow::TypeTraits<arrow::StringType>::BuilderType>(3)->Append(row.Uid).ok();
        ok = ok && BatchBuilder->GetFieldAs<arrow::TypeTraits<arrow::StringType>::BuilderType>(4)->Append(row.Message).ok();
        return ok;
    }

    std::shared_ptr<arrow::RecordBatch> Finish() {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto status = BatchBuilder->Flush(&batch);
        Y_VERIFY(status.ok());
        return batch;
    }

    static NOlap::TTiersInfo MakeTtl(TInstant border) {
        return NOlap::TTiersInfo(testColumns[0].first, border);
    }

private:
    std::shared_ptr<arrow::Schema> Schema = NArrow::MakeArrowSchema(testColumns);
    std::unique_ptr<arrow::RecordBatchBuilder> BatchBuilder;
};

TBlobRange MakeBlobRange(ui32 step, ui32 blobSize) {
    // tabletId, generation, step, channel, blobSize, cookie
    return TBlobRange(TUnifiedBlobId(11111, TLogoBlobID(100500, 42, step, 3, blobSize, 0)), 0, blobSize);
}

TString MakeTestBlob(ui64 start = 0, ui64 end = 100) {
    TBuilder builder;
    for (ui64 ts = start; ts < end; ++ts) {
        TString str = ToString(ts);
        builder.AddRow({ts, str, str, str, str});
    }
    auto batch = builder.Finish();
    return NArrow::SerializeBatchNoCompression(batch);
}

void AddIdsToBlobs(const TVector<TString>& srcBlobs, TVector<TPortionInfo>& portions,
                   THashMap<TBlobRange, TString>& blobs, ui32& step) {
    ui32 pos = 0;
    for (auto& portion : portions) {
        for (auto& rec : portion.Records) {
            rec.BlobRange = MakeBlobRange(++step, srcBlobs[pos].size());
            //UNIT_ASSERT(rec.Valid());
            blobs[rec.BlobRange] = srcBlobs[pos];
            ++pos;
        }
    }
}

TCompactionLimits TestLimits() {
    TCompactionLimits limits;
    limits.GranuleBlobSplitSize = 1024;
    limits.GranuleExpectedSize = 400 * 1024;
    limits.GranuleOverloadSize = 800 * 1024;
    return limits;
}

bool Insert(TColumnEngineForLogs& engine, TTestDbWrapper& db, TSnapshot snap,
            TVector<TInsertedData>&& dataToIndex, THashMap<TBlobRange, TString>& blobs, ui32& step) {
    std::shared_ptr<TColumnEngineChanges> changes = engine.StartInsert(std::move(dataToIndex));
    if (!changes) {
        return false;
    }

    changes->Blobs.insert(blobs.begin(), blobs.end());

    TVector<TString> newBlobs = TColumnEngineForLogs::IndexBlobs(engine.GetIndexInfo(), changes);

    UNIT_ASSERT_VALUES_EQUAL(changes->AppendedPortions.size(), 1);
    UNIT_ASSERT_VALUES_EQUAL(newBlobs.size(), testColumns.size() + 2); // add 2 columns: planStep, txId

    AddIdsToBlobs(newBlobs, changes->AppendedPortions, blobs, step);
    return engine.ApplyChanges(db, changes, snap);
}

bool Insert(TTestDbWrapper& db, TSnapshot snap,
            TVector<TInsertedData>&& dataToIndex, THashMap<TBlobRange, TString>& blobs, ui32& step) {
    TColumnEngineForLogs engine(TestTableInfo(), 0, TestLimits());
    engine.Load(db);

    return Insert(engine, db, snap, std::move(dataToIndex), blobs, step);
}

struct TExpected {
    ui32 SrcPortions;
    ui32 NewPortions;
    ui32 NewGranules;
};

bool Compact(TColumnEngineForLogs& engine, TTestDbWrapper& db, TSnapshot snap, THashMap<TBlobRange, TString>&& blobs, ui32& step,
             const TExpected& expected) {
    auto compactionInfo = engine.Compact();
    UNIT_ASSERT_VALUES_EQUAL(compactionInfo->Granules.size(), 1);
    UNIT_ASSERT(!compactionInfo->InGranule);

    std::shared_ptr<TColumnEngineChanges> changes = engine.StartCompaction(std::move(compactionInfo), {0, 0});
    UNIT_ASSERT_VALUES_EQUAL(changes->SwitchedPortions.size(), expected.SrcPortions);
    changes->SetBlobs(std::move(blobs));

    TVector<TString> newBlobs = TColumnEngineForLogs::CompactBlobs(engine.GetIndexInfo(), changes);

    UNIT_ASSERT_VALUES_EQUAL(changes->AppendedPortions.size(), expected.NewPortions);
    AddIdsToBlobs(newBlobs, changes->AppendedPortions, changes->Blobs, step);

    auto logsChanges = std::static_pointer_cast<TColumnEngineForLogs::TChanges>(changes);
    UNIT_ASSERT_VALUES_EQUAL(logsChanges->TmpGranuleIds.size(), expected.NewGranules);

    return engine.ApplyChanges(db, changes, snap);
}

bool Compact(TTestDbWrapper& db, TSnapshot snap, THashMap<TBlobRange, TString>&& blobs, ui32& step,
             const TExpected& expected) {
    TColumnEngineForLogs engine(TestTableInfo(), 0, TestLimits());
    engine.Load(db);
    return Compact(engine, db, snap, std::move(blobs), step, expected);
}

bool Cleanup(TColumnEngineForLogs& engine, TTestDbWrapper& db, TSnapshot snap, ui32 expectedToDrop) {
    THashSet<ui64> pathsToDrop;
    std::shared_ptr<TColumnEngineChanges> changes = engine.StartCleanup(snap, pathsToDrop);
    UNIT_ASSERT(changes);
    UNIT_ASSERT_VALUES_EQUAL(changes->PortionsToDrop.size(), expectedToDrop);

    return engine.ApplyChanges(db, changes, snap);
}

bool Ttl(TColumnEngineForLogs& engine, TTestDbWrapper& db,
         const THashMap<ui64, NOlap::TTiersInfo>& pathTtls, ui32 expectedToDrop) {
    std::shared_ptr<TColumnEngineChanges> changes = engine.StartTtl(pathTtls);
    UNIT_ASSERT(changes);
    UNIT_ASSERT_VALUES_EQUAL(changes->PortionsToDrop.size(), expectedToDrop);

    return engine.ApplyChanges(db, changes, changes->ApplySnapshot);
}

std::shared_ptr<TPredicate> MakePredicate(int64_t ts, NArrow::EOperation op) {
    auto p = std::make_shared<TPredicate>();
    p->Operation = op;

    auto type = arrow::timestamp(arrow::TimeUnit::MICRO);
    auto res = arrow::MakeArrayFromScalar(arrow::TimestampScalar(ts, type), 1);

    std::vector<std::shared_ptr<arrow::Field>> fields = { std::make_shared<arrow::Field>("timestamp", type) };
    p->Batch = arrow::RecordBatch::Make(std::make_shared<arrow::Schema>(fields), 1, {*res});
    return p;
}

}


Y_UNIT_TEST_SUITE(TColumnEngineTestLogs) {
    Y_UNIT_TEST(IndexWriteLoadRead) {
        TTestDbWrapper db;

        TVector<ui64> paths = {1, 2};

        TString testBlob = MakeTestBlob();

        TVector<TBlobRange> blobRanges;
        blobRanges.push_back(MakeBlobRange(1, testBlob.size()));
        blobRanges.push_back(MakeBlobRange(2, testBlob.size()));

        // PlanStep, TxId, PathId, DedupId, BlobId, Data, [Metadata]
        TInstant writeTime = TInstant::Now();
        TVector<TInsertedData> dataToIndex = {
            {1, 2, paths[0], "", blobRanges[0].BlobId, "", writeTime},
            {2, 1, paths[0], "", blobRanges[1].BlobId, "", writeTime}
        };

        // write

        ui32 step = 1000;
        THashMap<TBlobRange, TString> blobs;
        blobs[blobRanges[0]] = testBlob;
        blobs[blobRanges[1]] = testBlob;
        Insert(db, {1, 2}, std::move(dataToIndex), blobs, step);

        // load

        TColumnEngineForLogs engine(TestTableInfo(), 0);
        engine.Load(db);

        // selects

        const TIndexInfo& indexInfo = engine.GetIndexInfo();
        THashSet<ui32> oneColumnId = { indexInfo.GetColumnId(testColumns[0].first) };
        THashSet<ui32> columnIds;
        for (auto& [column, typeId] : testColumns) {
            columnIds.insert(indexInfo.GetColumnId(column));
        }

        { // select from snap before insert
            ui64 planStep = 1;
            ui64 txId = 0;
            auto selectInfo = engine.Select(paths[0], {planStep, txId}, columnIds, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 0);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 0);
        }

        { // select from snap after insert (greater txId)
            ui64 planStep = 1;
            ui64 txId = 2;
            auto selectInfo = engine.Select(paths[0], {planStep, txId}, columnIds, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions[0].NumRecords(), columnIds.size());
        }

        { // select from snap after insert (greater planStep)
            ui64 planStep = 2;
            ui64 txId = 1;
            auto selectInfo = engine.Select(paths[0], {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions[0].NumRecords(), 1);
        }

        { // select another pathId
            ui64 planStep = 2;
            ui64 txId = 1;
            auto selectInfo = engine.Select(paths[1], {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 0);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 0);
        }
    }

    Y_UNIT_TEST(IndexReadWithPredicates) {
        TTestDbWrapper db;

        ui64 pathId = 1;
        ui32 step = 1000;

        // insert
        ui64 planStep = 1;

        THashMap<TBlobRange, TString> blobs;
        ui64 numRows = 1000;
        ui64 rowPos = 0;
        for (ui64 txId = 1; txId <= 20; ++txId, rowPos += numRows) {
            TString testBlob = MakeTestBlob(rowPos, rowPos + numRows);
            auto blobRange = MakeBlobRange(++step, testBlob.size());
            blobs[blobRange] = testBlob;

            // PlanStep, TxId, PathId, DedupId, BlobId, Data, [Metadata]
            TVector<TInsertedData> dataToIndex;
            dataToIndex.push_back(
                TInsertedData{planStep, txId, pathId, "", blobRange.BlobId, "", TInstant::Now()});

            bool ok = Insert(db, {planStep, txId}, std::move(dataToIndex), blobs, step);
            UNIT_ASSERT(ok);
        }

        // compact
        planStep = 2;

        bool ok = Compact(db, TSnapshot{planStep, 1}, std::move(blobs), step, {20, 4, 4});
        UNIT_ASSERT(ok);

        // load

        TColumnEngineForLogs engine(TestTableInfo(), 0, TestLimits());
        engine.Load(db);

        // read

        // TODO: read old snapshot

        planStep = 3;

        const TIndexInfo& indexInfo = engine.GetIndexInfo();
        THashSet<ui32> oneColumnId = { indexInfo.GetColumnId(testColumns[0].first) };

        { // full scan
            ui64 txId = 1;
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 4);
        }

        // predicates

        {
            ui64 txId = 1;
            std::shared_ptr<TPredicate> gt10k = MakePredicate(10000, NArrow::EOperation::Greater);
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, gt10k, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 2);
        }

        {
            ui64 txId = 1;
            std::shared_ptr<TPredicate> lt10k = MakePredicate(9999, NArrow::EOperation::Less); // TODO: better border checks
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, 0, lt10k);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 2);
        }
    }

    Y_UNIT_TEST(IndexWriteOverload) {
        TTestDbWrapper db;

        ui64 pathId = 1;
        ui32 step = 1000;

        // inserts
        ui64 planStep = 1;

        TColumnEngineForLogs engine(TestTableInfo(), 0, TestLimits());
        engine.Load(db);

        THashMap<TBlobRange, TString> blobs;
        ui64 numRows = 1000;
        ui64 rowPos = 0;
        bool overload = false;
        for (ui64 txId = 1; txId <= 100; ++txId, rowPos += numRows) {
            TString testBlob = MakeTestBlob(rowPos, rowPos + numRows);
            auto blobRange = MakeBlobRange(++step, testBlob.size());
            blobs[blobRange] = testBlob;

            // PlanStep, TxId, PathId, DedupId, BlobId, Data, [Metadata]
            TVector<TInsertedData> dataToIndex;
            dataToIndex.push_back(
                TInsertedData{planStep, txId, pathId, "", blobRange.BlobId, "", TInstant::Now()});

            bool ok = Insert(engine, db, {planStep, txId}, std::move(dataToIndex), blobs, step);
            // first overload returns ok: it's a postcondition
            if (!overload) {
                UNIT_ASSERT(ok);
            } else {
                UNIT_ASSERT(!ok);
                break;
            }
            overload = engine.GetOverloadedGranules(pathId);
        }
        UNIT_ASSERT(overload);

        { // check it's overloaded after reload
            TColumnEngineForLogs tmpEngine(TestTableInfo(), 0, TestLimits());
            tmpEngine.Load(db);
            UNIT_ASSERT(tmpEngine.GetOverloadedGranules(pathId));
        }

        // compact
        planStep = 2;

        bool ok = Compact(engine, db, TSnapshot{planStep, 1}, std::move(blobs), step, {23, 5, 5});
        UNIT_ASSERT(ok);

        // success write after compaction
        planStep = 3;

        for (ui64 txId = 1; txId <= 2; ++txId, rowPos += numRows) {
            TString testBlob = MakeTestBlob(rowPos, rowPos + numRows);
            auto blobRange = MakeBlobRange(++step, testBlob.size());
            blobs[blobRange] = testBlob;

            // PlanStep, TxId, PathId, DedupId, BlobId, Data, [Metadata]
            TVector<TInsertedData> dataToIndex;
            dataToIndex.push_back(
                TInsertedData{planStep, txId, pathId, "", blobRange.BlobId, "", TInstant::Now()});

            bool ok = Insert(engine, db, {planStep, txId}, std::move(dataToIndex), blobs, step);
            bool overload = engine.GetOverloadedGranules(pathId);
            UNIT_ASSERT(ok);
            UNIT_ASSERT(!overload);
        }

        { // check it's not overloaded after reload
            TColumnEngineForLogs tmpEngine(TestTableInfo(), 0, TestLimits());
            tmpEngine.Load(db);
            UNIT_ASSERT(!tmpEngine.GetOverloadedGranules(pathId));
        }
    }

    Y_UNIT_TEST(IndexTtl) {
        TTestDbWrapper db;

        ui64 pathId = 1;
        ui32 step = 1000;

        // insert
        ui64 planStep = 1;

        THashMap<TBlobRange, TString> blobs;
        ui64 numRows = 1000;
        ui64 rowPos = 0;
        for (ui64 txId = 1; txId <= 20; ++txId, rowPos += numRows) {
            TString testBlob = MakeTestBlob(rowPos, rowPos + numRows);
            auto blobRange = MakeBlobRange(++step, testBlob.size());
            blobs[blobRange] = testBlob;

            // PlanStep, TxId, PathId, DedupId, BlobId, Data, [Metadata]
            TVector<TInsertedData> dataToIndex;
            dataToIndex.push_back(
                TInsertedData{planStep, txId, pathId, "", blobRange.BlobId, "", TInstant::Now()});

            bool ok = Insert(db, {planStep, txId}, std::move(dataToIndex), blobs, step);
            UNIT_ASSERT(ok);
        }

        // compact
        planStep = 2;

        bool ok = Compact(db, TSnapshot{planStep, 1}, std::move(blobs), step, {20, 4, 4});
        UNIT_ASSERT(ok);

        // load

        TColumnEngineForLogs engine(TestTableInfo(), 0, TestLimits());
        engine.Load(db);

        // read
        planStep = 3;

        const TIndexInfo& indexInfo = engine.GetIndexInfo();
        THashSet<ui32> oneColumnId = { indexInfo.GetColumnId(testColumns[0].first) };

        { // full scan
            ui64 txId = 1;
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 4);
        }

        // Cleanup
        Cleanup(engine, db, TSnapshot{planStep, 1}, 20);

        { // full scan
            ui64 txId = 1;
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 4);
        }

        // TTL
        THashMap<ui64, NOlap::TTiersInfo> pathTtls;
        pathTtls.emplace(pathId, TBuilder::MakeTtl(TInstant::MicroSeconds(10000)));
        Ttl(engine, db, pathTtls, 2);

        // read + load + read

        { // full scan
            ui64 txId = 1;
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 2);
        }

        // load
        engine.Load(db);
        UNIT_ASSERT_VALUES_EQUAL(engine.GetTotalStats().EmptyGranules, 1);

        { // full scan
            ui64 txId = 1;
            auto selectInfo = engine.Select(pathId, {planStep, txId}, oneColumnId, {}, {});
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Portions.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(selectInfo->Granules.size(), 2);
        }
    }
}

}
