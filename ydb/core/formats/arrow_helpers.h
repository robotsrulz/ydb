#pragma once
#include "switch_type.h"
#include <ydb/core/formats/factory.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/type_traits.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/ipc/writer.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/util/compression.h>

namespace NKikimr::NArrow {

// Arrow inrernally keeps references to Buffer objects with the data
// This helper class implements arrow::Buffer over TString that owns
// the actual memory
class TBufferOverString : public arrow::Buffer {
    TString Str;
public:
    explicit TBufferOverString(TString str)
        : arrow::Buffer((const unsigned char*)str.data(), str.size())
        , Str(str)
    {
        Y_VERIFY(data() == (const unsigned char*)Str.data());
    }
};

std::shared_ptr<arrow::DataType> GetArrowType(NScheme::TTypeId typeId);

template <typename T>
inline bool ArrayEqualValue(const std::shared_ptr<arrow::Array>& x, const std::shared_ptr<arrow::Array>& y) {
    auto& arrX = static_cast<const T&>(*x);
    auto& arrY = static_cast<const T&>(*y);
    for (int i = 0; i < x->length(); ++i) {
        if (arrX.Value(i) != arrY.Value(i)) {
            return false;
        }
    }
    return true;
}

template <typename T>
inline bool ArrayEqualView(const std::shared_ptr<arrow::Array>& x, const std::shared_ptr<arrow::Array>& y) {
    auto& arrX = static_cast<const T&>(*x);
    auto& arrY = static_cast<const T&>(*y);
    for (int i = 0; i < x->length(); ++i) {
        if (arrX.GetView(i) != arrY.GetView(i)) {
            return false;
        }
    }
    return true;
}

struct TSortDescription;

std::vector<std::shared_ptr<arrow::Field>> MakeArrowFields(const TVector<std::pair<TString, NScheme::TTypeId>>& columns);
std::shared_ptr<arrow::Schema> MakeArrowSchema(const TVector<std::pair<TString,  NScheme::TTypeId>>& columns);

TString SerializeSchema(const arrow::Schema& schema);
std::shared_ptr<arrow::Schema> DeserializeSchema(const TString& str);

TString SerializeBatch(const std::shared_ptr<arrow::RecordBatch>& batch, const arrow::ipc::IpcWriteOptions& options);
TString SerializeBatchNoCompression(const std::shared_ptr<arrow::RecordBatch>& batch);

std::shared_ptr<arrow::RecordBatch> DeserializeBatch(const TString& blob,
                                                     const std::shared_ptr<arrow::Schema>& schema);
std::shared_ptr<arrow::RecordBatch> MakeEmptyBatch(const std::shared_ptr<arrow::Schema>& schema);

std::shared_ptr<arrow::RecordBatch> ExtractColumns(const std::shared_ptr<arrow::RecordBatch>& srcBatch,
                                                   const std::vector<TString>& columnNames);
std::shared_ptr<arrow::RecordBatch> ExtractColumns(const std::shared_ptr<arrow::RecordBatch>& srcBatch,
                                                   const std::shared_ptr<arrow::Schema>& dstSchema,
                                                   bool addNotExisted = false);
std::shared_ptr<arrow::Table> CombineInTable(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);
std::shared_ptr<arrow::RecordBatch> ToBatch(const std::shared_ptr<arrow::Table>& combinedTable);
std::shared_ptr<arrow::RecordBatch> CombineBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);
std::shared_ptr<arrow::RecordBatch> CombineSortedBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                                         const std::shared_ptr<TSortDescription>& description);
std::vector<std::shared_ptr<arrow::RecordBatch>> MergeSortedBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                                                    const std::shared_ptr<TSortDescription>& description,
                                                                    size_t maxBatchRows, ui64 limit = 0);
std::vector<std::shared_ptr<arrow::RecordBatch>> ShardingSplit(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                               const std::vector<ui32>& sharding,
                                                               ui32 numShards);

std::vector<std::unique_ptr<arrow::ArrayBuilder>> MakeBuilders(const std::shared_ptr<arrow::Schema>& schema,
                                                               size_t reserve = 0);
std::vector<std::shared_ptr<arrow::Array>> Finish(std::vector<std::unique_ptr<arrow::ArrayBuilder>>&& builders);

std::shared_ptr<arrow::UInt64Array> MakeUI64Array(ui64 value, i64 size);
std::shared_ptr<arrow::UInt64Array> MakePermutation(int size, bool reverse = false);
std::shared_ptr<arrow::BooleanArray> MakeFilter(const std::vector<bool>& bits);
std::vector<bool> CombineFilters(std::vector<bool>&& f1, std::vector<bool>&& f2);
TVector<TString> ColumnNames(const std::shared_ptr<arrow::Schema>& schema);
// Return size in bytes including size of bitmap mask
ui64 GetBatchDataSize(const std::shared_ptr<arrow::RecordBatch>& batch);
// Return size in bytes *not* including size of bitmap mask
ui64 GetArrayDataSize(const std::shared_ptr<arrow::Array>& column);

enum class ECompareType {
    LESS = 1,
    LESS_OR_EQUAL,
    GREATER,
    GREATER_OR_EQUAL,
};

// It makes a filter using composite predicate. You need MakeFilter() + arrow::Filter() to apply it to Datum.
std::vector<bool> MakePredicateFilter(const arrow::Datum& datum, const arrow::Datum& border, ECompareType compareType);
std::shared_ptr<arrow::UInt64Array> MakeSortPermutation(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                        const std::shared_ptr<arrow::Schema>& sortingKey);
std::shared_ptr<arrow::RecordBatch> SortBatch(const std::shared_ptr<arrow::RecordBatch>& batch,
                                              const std::shared_ptr<arrow::Schema>& sortingKey);
bool IsSorted(const std::shared_ptr<arrow::RecordBatch>& batch,
              const std::shared_ptr<arrow::Schema>& sortingKey,
              bool desc = false);
bool IsSortedAndUnique(const std::shared_ptr<arrow::RecordBatch>& batch,
                       const std::shared_ptr<arrow::Schema>& sortingKey,
                       bool desc = false);

template <typename TArr>
std::shared_ptr<TArr> GetTypedColumn(const std::shared_ptr<arrow::RecordBatch>& batch, int pos) {
    auto array = batch->column(pos);
    Y_VERIFY(array);
    //Y_VERIFY(array->type_id() == arrow::Type::TIMESTAMP); // TODO
    auto column = std::static_pointer_cast<TArr>(array);
    Y_VERIFY(column);
    return column;
}

template <typename TArr>
std::shared_ptr<TArr> GetTypedColumn(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& columnName) {
    auto array = batch->GetColumnByName(columnName);
    Y_VERIFY(array);
    //Y_VERIFY(array->type_id() == arrow::Type::TIMESTAMP); // TODO
    auto column = std::static_pointer_cast<TArr>(array);
    Y_VERIFY(column);
    return column;
}

std::pair<int, int> FindMinMaxPosition(const std::shared_ptr<arrow::Array>& column);
std::shared_ptr<arrow::Scalar> GetScalar(const std::shared_ptr<arrow::Array>& array, int position);
bool ScalarLess(const arrow::Scalar& x, const arrow::Scalar& y);

class IRowWriter;

// Converts an arrow batch into YDB rows feeding them IRowWriter one by one
class TArrowToYdbConverter {
private:
    TVector<std::pair<TString, NScheme::TTypeId>> YdbSchema; // Destination schema (allow shrink and reorder)
    IRowWriter& RowWriter;

    template <typename TArray>
    TCell MakeCellFromValue(const std::shared_ptr<arrow::Array>& column, i64 row) {
        auto array = std::static_pointer_cast<TArray>(column);
        return TCell::Make(array->Value(row));
    }

    template <typename TArray>
    TCell MakeCellFromView(const std::shared_ptr<arrow::Array>& column, i64 row) {
        auto array = std::static_pointer_cast<TArray>(column);
        auto data = array->GetView(row);
        return TCell(data.data(), data.size());
    }

    template <typename TArrayType>
    TCell MakeCell(const std::shared_ptr<arrow::Array>& column, i64 row) {
        return MakeCellFromValue<TArrayType>(column, row);
    }

    template <>
    TCell MakeCell<arrow::BinaryArray>(const std::shared_ptr<arrow::Array>& column, i64 row) {
        return MakeCellFromView<arrow::BinaryArray>(column, row);
    }

    template <>
    TCell MakeCell<arrow::StringArray>(const std::shared_ptr<arrow::Array>& column, i64 row) {
        return MakeCellFromView<arrow::StringArray>(column, row);
    }

    template <>
    TCell MakeCell<arrow::Decimal128Array>(const std::shared_ptr<arrow::Array>& column, i64 row) {
        return MakeCellFromView<arrow::Decimal128Array>(column, row);
    }

public:
    static bool NeedDataConversion(const NScheme::TTypeId& colType) {
        switch (colType) {
            case NScheme::NTypeIds::DyNumber:
            case NScheme::NTypeIds::JsonDocument:
            case NScheme::NTypeIds::Decimal:
                return true;
            default:
                break;
        }
        return false;
    }

    TArrowToYdbConverter(const TVector<std::pair<TString, NScheme::TTypeId>>& ydbSchema, IRowWriter& rowWriter)
        : YdbSchema(ydbSchema)
        , RowWriter(rowWriter)
    {}

    bool Process(const arrow::RecordBatch& batch, TString& errorMessage);
};

// Interface to accept rows that are read form arrow batch
class IRowWriter {
public:
    virtual ~IRowWriter() {}

    // NOTE: This method must copy cells data to its own strorage
    virtual void AddRow(const TConstArrayRef<TCell> &cells) = 0;
};

std::shared_ptr<arrow::RecordBatch> ConvertColumns(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                   const THashMap<TString, NScheme::TTypeId>& columnsToConvert);

inline bool HasNulls(const std::shared_ptr<arrow::Array>& column) {
    return column->null_bitmap_data();
}

bool ArrayScalarsEqual(const std::shared_ptr<arrow::Array>& lhs, const std::shared_ptr<arrow::Array>& rhs);
std::shared_ptr<arrow::Array> NumVecToArray(const std::shared_ptr<arrow::DataType>& type,
                                            const std::vector<double>& vec);
std::shared_ptr<arrow::Array> BoolVecToArray(const std::vector<bool>& vec);

}
