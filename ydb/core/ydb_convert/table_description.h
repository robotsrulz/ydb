#pragma once

#include "table_profiles.h"

#include <ydb/library/mkql_proto/protos/minikql.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/public/api/protos/ydb_table.pb.h>

namespace NKikimr {

// out
void FillColumnDescription(Ydb::Table::DescribeTableResult& out,
    NKikimrMiniKQL::TType& splitKeyType, const NKikimrSchemeOp::TTableDescription& in);
void FillColumnDescription(Ydb::Table::CreateTableRequest& out,
    NKikimrMiniKQL::TType& splitKeyType, const NKikimrSchemeOp::TTableDescription& in);
// in
bool FillColumnDescription(NKikimrSchemeOp::TTableDescription& out,
    const google::protobuf::RepeatedPtrField<Ydb::Table::ColumnMeta>& in, Ydb::StatusIds::StatusCode& status, TString& error);
bool ExtractColumnTypeId(ui32& outTypeId, const Ydb::Type& inType, Ydb::StatusIds::StatusCode& status, TString& error);

// out
void FillTableBoundary(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in, const NKikimrMiniKQL::TType& splitKeyType);
void FillTableBoundary(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in, const NKikimrMiniKQL::TType& splitKeyType);

// out
void FillIndexDescription(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
void FillIndexDescription(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in);
// in
bool FillIndexDescription(NKikimrSchemeOp::TIndexedTableCreationConfig& out,
    const Ydb::Table::CreateTableRequest& in, Ydb::StatusIds::StatusCode& status, TString& error);

// out
void FillChangefeedDescription(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
// in
bool FillChangefeedDescription(NKikimrSchemeOp::TCdcStreamDescription& out,
    const Ydb::Table::Changefeed& in, Ydb::StatusIds::StatusCode& status, TString& error);

// out
void FillTableStats(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TPathDescription& in, bool withPartitionStatistic);

// out
void FillStorageSettings(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
void FillStorageSettings(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in);

// out
void FillColumnFamilies(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
void FillColumnFamilies(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in);

// out
void FillAttributes(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TPathDescription& in);
void FillAttributes(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TPathDescription& in);

// out
void FillPartitioningSettings(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
void FillPartitioningSettings(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in);

// in
bool CopyExplicitPartitions(NKikimrSchemeOp::TTableDescription& out,
    const Ydb::Table::ExplicitPartitions& in, Ydb::StatusIds::StatusCode& status, TString& error);

// out
void FillKeyBloomFilter(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
void FillKeyBloomFilter(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in);

// out
void FillReadReplicasSettings(Ydb::Table::DescribeTableResult& out,
    const NKikimrSchemeOp::TTableDescription& in);
void FillReadReplicasSettings(Ydb::Table::CreateTableRequest& out,
    const NKikimrSchemeOp::TTableDescription& in);

// in
bool FillTableDescription(NKikimrSchemeOp::TModifyScheme& out,
    const Ydb::Table::CreateTableRequest& in, const TTableProfiles& profiles,
    Ydb::StatusIds::StatusCode& status, TString& error);

} // namespace NKikimr
