#pragma once
#include "defs.h"
#include <ydb/core/tablet_flat/flat_dbase_scheme.h>
#include <ydb/core/protos/tx_columnshard.pb.h>
#include <ydb/core/sys_view/common/schema.h>

namespace arrow {
    class Array;
    class Field;
    class Schema;
}

namespace NKikimr::NArrow {
    struct TSortDescription;
}

namespace NKikimr::NOlap {

void ScalarToConstant(const arrow::Scalar& scalar, NKikimrSSA::TProgram_TConstant& value);
std::shared_ptr<arrow::Scalar> ConstantToScalar(const NKikimrSSA::TProgram_TConstant& value,
                                                const std::shared_ptr<arrow::DataType>& type);

template <typename T>
static std::shared_ptr<arrow::Schema> MakeArrowSchema(const NTable::TScheme::TTableSchema::TColumns& columns, const T& ids) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(ids.size());

    for (ui32 id: ids) {
        auto it = columns.find(id);
        if (it == columns.end()) {
            return {};
        }

        const auto& column = it->second;
        std::string colName(column.Name.data(), column.Name.size());
        fields.emplace_back(std::make_shared<arrow::Field>(colName, NArrow::GetArrowType(column.PType)));
    }

    return std::make_shared<arrow::Schema>(fields);
}

inline
TVector<std::pair<TString, NScheme::TTypeId>>
GetColumns(const NTable::TScheme::TTableSchema& tableSchema, const TVector<ui32>& ids) {
    TVector<std::pair<TString, NScheme::TTypeId>> out;
    out.reserve(ids.size());
    for (ui32 id : ids) {
        Y_VERIFY(tableSchema.Columns.count(id));
        auto& column = tableSchema.Columns.find(id)->second;
        out.emplace_back(column.Name, column.PType);
    }
    return out;
}

struct TInsertedData;

struct TCompression {
    arrow::Compression::type Codec{arrow::Compression::LZ4_FRAME};
    std::optional<int> Level;
};

struct TStorageTier {
    TString Name;
    std::optional<TCompression> Compression;
};

/// Column engine index description in terms of tablet's local table.
/// We have to use YDB types for keys here.
struct TIndexInfo : public NTable::TScheme::TTableSchema {
    static constexpr const char * SPEC_COL_PLAN_STEP = "_yql_plan_step";
    static constexpr const char * SPEC_COL_TX_ID = "_yql_tx_id";
    static const TString STORE_INDEX_STATS_TABLE;
    static const TString TABLE_INDEX_STATS_TABLE;

    enum class ESpecialColumn : ui32 {
        PLAN_STEP = 0xffffff00,
        TX_ID,
    };

    TIndexInfo(const TString& name, ui32 id)
        : NTable::TScheme::TTableSchema()
        , Id(id)
        , Name(name)
    {}

    ui32 GetId() const {
        return Id;
    }

    ui32 GetColumnId(const TString& name) const {
        if (!ColumnNames.count(name)) {
            if (name == SPEC_COL_PLAN_STEP) {
                return (ui32)ESpecialColumn::PLAN_STEP;
            } else if (name == SPEC_COL_TX_ID) {
                return (ui32)ESpecialColumn::TX_ID;
            }
            Y_VERIFY(false);
        }
        return ColumnNames.find(name)->second;
    }

    TString GetColumnName(ui32 id, bool required = true) const {
        switch (ESpecialColumn(id)) {
            case ESpecialColumn::PLAN_STEP:
                return SPEC_COL_PLAN_STEP;
            case ESpecialColumn::TX_ID:
                return SPEC_COL_TX_ID;
            default:
                break;
        }

        if (!required && !Columns.count(id)) {
            return {};
        }

        Y_VERIFY(Columns.count(id));
        return Columns.find(id)->second.Name;
    }

    TVector<TString> GetColumnNames(const TVector<ui32>& ids) const {
        TVector<TString> out;
        out.reserve(ids.size());
        for (ui32 id : ids) {
            Y_VERIFY(Columns.count(id));
            out.push_back(Columns.find(id)->second.Name);
        }
        return out;
    }

    TVector<std::pair<TString, NScheme::TTypeId>> GetColumns(const TVector<ui32>& ids) const {
        return NOlap::GetColumns(*this, ids);
    }

    // Traditional Primary Key (includes uniqueness, search and sorting logic)
    TVector<std::pair<TString, NScheme::TTypeId>> GetPK() const {
        return GetColumns(KeyColumns);
    }

    static TVector<std::pair<TString, NScheme::TTypeId>> SchemaIndexStats(ui32 version = 0);
    static TVector<std::pair<TString, NScheme::TTypeId>> SchemaIndexStatsKey(ui32 version = 0);

    ui32 GetPKFirstColumnId() const {
        Y_VERIFY(KeyColumns.size());
        return KeyColumns[0];
    }

    // Sorting key: colud be less or greater then traditional PK
    // It could be empty for append-only tables. It could be greater then PK for better columns compression.
    // If sorting key includes uniqueness key as a prefix we are able to use MergeSort for REPLACE.
    const std::shared_ptr<arrow::Schema>& GetSortingKey() const { return SortingKey; }
    const std::shared_ptr<arrow::Schema>& GetReplaceKey() const { return ReplaceKey; }
    const std::shared_ptr<arrow::Schema>& GetExtendedKey() const { return ExtendedKey; }
    const std::shared_ptr<arrow::Schema>& GetIndexKey() const { return IndexKey; }

    void SetAllKeys(const TVector<TString>& columns, const TVector<int>& indexKeyPos);
    void SetAllKeys(const TVector<std::pair<TString, NScheme::TTypeId>>& columns, const TVector<int>& indexKeyPos) {
        SetAllKeys(NamesOnly(columns), indexKeyPos);
    }

    void AddTtlColumn(const TString& ttlColumn) {
        Y_VERIFY(!ttlColumn.empty());
        MinMaxIdxColumnsIds.insert(GetColumnId(ttlColumn));
    }

    TVector<TRawTypeValue> ExtractKey(const THashMap<ui32, TCell>& fields, bool allowNulls = false) const;
    std::shared_ptr<arrow::Schema> ArrowSchema() const;
    std::shared_ptr<arrow::Schema> ArrowSchemaWithSpecials() const;
    std::shared_ptr<arrow::Schema> AddColumns(std::shared_ptr<arrow::Schema> schema,
                                              const TVector<TString>& columns) const;
    static std::shared_ptr<arrow::Schema> ArrowSchemaSnapshot();
    std::shared_ptr<arrow::Schema> ArrowSchema(const TVector<ui32>& columnIds) const;
    std::shared_ptr<arrow::Schema> ArrowSchema(const TVector<TString>& columnNames) const;
    std::shared_ptr<arrow::Field> ArrowColumnField(ui32 columnId) const;
    std::shared_ptr<arrow::RecordBatch> PrepareForInsert(const TString& data, const TString& metadata,
                                                         TString& strError) const;

    const THashSet<TString>& GetRequiredColumns() const {
        return RequiredColumns;
    }

    const THashSet<ui32>& GetMinMaxIdxColumns() const {
        return MinMaxIdxColumnsIds;
    }

    bool AllowTtlOverColumn(const TString& name) const {
        auto it = ColumnNames.find(name);
        if (it == ColumnNames.end()) {
            return false;
        }
        return MinMaxIdxColumnsIds.count(it->second);
    }

    bool IsSorted() const { return SortingKey.get(); }
    bool IsReplacing() const { return ReplaceKey.get(); }

    std::shared_ptr<NArrow::TSortDescription> SortDescription() const;
    std::shared_ptr<NArrow::TSortDescription> SortReplaceDescription() const;

    static bool IsSpecialColumn(const arrow::Field& field);
    static std::shared_ptr<arrow::RecordBatch> AddSpecialColumns(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                                 ui64 platStep, ui64 txId);

    void SetDefaultCompression(const TCompression& compression) { DefaultCompression = compression; }
    const TCompression& GetDefaultCompression() const { return DefaultCompression; }

    std::optional<TCompression> GetTierCompression(ui32 tierNo) const {
        if (!Tiers.empty()) {
            Y_VERIFY(tierNo < Tiers.size());
            return Tiers[tierNo].Compression;
        }
        return {};
    }

    std::optional<TCompression> GetTierCompression(const TString& tierName) const {
        if (tierName.empty()) {
            return {};
        }
        ui32 tierNo = GetTierNumber(tierName);
        Y_VERIFY(tierNo != Max<ui32>());
        return GetTierCompression(tierNo);
    }

    TString GetTierName(ui32 tierNo) const {
        if (!Tiers.empty()) {
            Y_VERIFY(tierNo < Tiers.size());
            return Tiers[tierNo].Name;
        }
        return {};
    }

    void AddStorageTier(TStorageTier&& tier) {
        TierByName[tier.Name] = Tiers.size();
        Tiers.emplace_back(std::move(tier));
    }

    ui32 GetTierNumber(const TString& tierName) const {
        if (auto it = TierByName.find(tierName); it != TierByName.end()) {
            return it->second;
        }
        return Max<ui32>();
    }

private:
    ui32 Id;
    TString Name;
    mutable std::shared_ptr<arrow::Schema> Schema;
    mutable std::shared_ptr<arrow::Schema> SchemaWithSpecials;
    std::shared_ptr<arrow::Schema> SortingKey;
    std::shared_ptr<arrow::Schema> ReplaceKey;
    std::shared_ptr<arrow::Schema> ExtendedKey; // Extend PK with snapshot columns to allow old shapshot reads
    std::shared_ptr<arrow::Schema> IndexKey;
    THashSet<TString> RequiredColumns;
    THashSet<ui32> MinMaxIdxColumnsIds;
    TCompression DefaultCompression;
    std::vector<TStorageTier> Tiers;
    THashMap<TString, ui32> TierByName;

    void AddRequiredColumns(const TVector<TString>& columns) {
        for (auto& name: columns) {
            RequiredColumns.insert(name);
        }
    }

    static TVector<TString> NamesOnly(const TVector<std::pair<TString, NScheme::TTypeId>>& columns) {
        TVector<TString> out;
        out.reserve(columns.size());
        for (auto& [name, type] : columns) {
            out.push_back(name);
        }
        return out;
    }
};

}
