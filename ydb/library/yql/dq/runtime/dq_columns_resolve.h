#pragma once

#include <ydb/library/yql/utils/yql_panic.h>
#include <ydb/library/yql/public/udf/udf_data_type.h>
#include <ydb/library/yql/minikql/mkql_node.h>
// #include <ydb/library/yql/dq/proto/dq_tasks.pb.h>

namespace NYql::NDq {

struct TColumnInfo {
    TString Name;
    ui32 Index;
    NKikimr::NMiniKQL::TType* Type;

    TColumnInfo(TString name, ui32 index, NKikimr::NMiniKQL::TType* type) : Name(name), Index(index), Type(type) {};

    NUdf::TDataTypeId GetTypeId() const {
        YQL_ENSURE(Type->GetKind() == NKikimr::NMiniKQL::TType::EKind::Data);
        return static_cast<NKikimr::NMiniKQL::TDataType&>(*Type).GetSchemeType();
    }
};

struct TSortColumnInfo : public TColumnInfo {
    bool Ascending;

    TSortColumnInfo(const TColumnInfo&& colInfo)
        : TColumnInfo(std::move(colInfo))
        , Ascending(false)
    {}
};

TMaybe<TColumnInfo> FindColumnInfo(const NKikimr::NMiniKQL::TType* type, TStringBuf column);
TColumnInfo GetColumnInfo(const NKikimr::NMiniKQL::TType* type, TStringBuf column);

template<typename TList>
void GetColumnsInfo(const NKikimr::NMiniKQL::TType* type, const TList& columns,
    TVector<NKikimr::NMiniKQL::TType*>& columnTypes, TVector<ui32>& columnIndices)
{
    columnTypes.clear();
    columnIndices.clear();

    columnTypes.reserve(columns.size());
    columnIndices.reserve(columns.size());

    for (auto& column : columns) {
        auto columnInfo = GetColumnInfo(type, column);
        columnTypes.push_back(columnInfo.Type);
        columnIndices.push_back(columnInfo.Index);
    }
}

template<typename TList>
void GetColumnsInfo(const NKikimr::NMiniKQL::TType* type, const TList& protoSortCols,
    TVector<TSortColumnInfo>& sortCols)
{
    sortCols.clear();
    sortCols.reserve(protoSortCols.size());

    for (const auto& protoSortCol : protoSortCols) {
        TSortColumnInfo colInfo = static_cast<TSortColumnInfo>(GetColumnInfo(type, protoSortCol.GetColumn()));
        colInfo.Ascending = protoSortCol.GetAscending();
        sortCols.emplace_back(std::move(colInfo));
    }
}

}
