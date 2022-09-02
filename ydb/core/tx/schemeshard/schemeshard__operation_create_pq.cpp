#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/engine/mkql_proto.h>
#include <ydb/core/persqueue/config/config.h>
#include <ydb/core/mind/hive/hive.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

TPersQueueGroupInfo::TPtr CreatePersQueueGroup(TOperationContext& context,
                                               const NKikimrSchemeOp::TPersQueueGroupDescription& op,
                                               TEvSchemeShard::EStatus& status, TString& errStr)
{
    TPersQueueGroupInfo::TPtr pqGroupInfo = new TPersQueueGroupInfo;

    ui32 partitionCount = 0;
    if (op.HasTotalGroupCount()) {
        partitionCount = op.GetTotalGroupCount();
    }

    ui32 partsPerTablet = TSchemeShard::DefaultPQTabletPartitionsCount;
    if (op.HasPartitionPerTablet()) {
        partsPerTablet = op.GetPartitionPerTablet();
    }

    if (op.PartitionsToDeleteSize() > 0) {
        status = NKikimrScheme::StatusSchemeError;
        errStr = Sprintf("trying to delete partitions from not created PQGroup");
        return nullptr;
    }

    if (op.PartitionsToAddSize()) {
        errStr = Sprintf("creating topic with providing of partitions count is forbidden");
        return nullptr;
    }

    if (partitionCount == 0 || partitionCount > TSchemeShard::MaxPQGroupPartitionsCount) {
        status = NKikimrScheme::StatusInvalidParameter;
        errStr = Sprintf("Invalid total partition count specified: %u", partitionCount);
        return nullptr;
    }

    if (!op.HasPQTabletConfig()) {
        status = NKikimrScheme::StatusSchemeError;
        errStr = Sprintf("No PQTabletConfig specified");
        return nullptr;
    }

    if ((ui32)op.GetPQTabletConfig().GetPartitionConfig().GetWriteSpeedInBytesPerSecond() > TSchemeShard::MaxPQWriteSpeedPerPartition) {
        status = NKikimrScheme::StatusInvalidParameter;
        errStr = TStringBuilder()
                << "Invalid write speed per second in partition specified: " << op.GetPQTabletConfig().GetPartitionConfig().GetWriteSpeedInBytesPerSecond()
                << " vs " << TSchemeShard::MaxPQWriteSpeedPerPartition;
        return nullptr;
    }

    if ((ui32)op.GetPQTabletConfig().GetPartitionConfig().GetLifetimeSeconds() > TSchemeShard::MaxPQLifetimeSeconds) {
        status = NKikimrScheme::StatusInvalidParameter;
        errStr = TStringBuilder()
                << "Invalid retention period specified: " << op.GetPQTabletConfig().GetPartitionConfig().GetLifetimeSeconds()
                << " vs " << TSchemeShard::MaxPQLifetimeSeconds;
        return nullptr;
    }

    if (op.GetPQTabletConfig().PartitionKeySchemaSize()) {
        if (op.PartitionBoundariesSize() != (partitionCount - 1)) {
            status = NKikimrScheme::StatusInvalidParameter;
            errStr = Sprintf("Partition count and partition boundaries size mismatch: %lu, %u",
                op.PartitionBoundariesSize(), partitionCount);
            return nullptr;
        }

        TString error;
        if (!pqGroupInfo->FillKeySchema(op.GetPQTabletConfig(), error)) {
            status = NKikimrScheme::StatusSchemeError;
            errStr = Sprintf("Invalid key schema: %s", error.data());
            return nullptr;
        }
    } else {
        if (op.PartitionBoundariesSize()) {
            status = NKikimrScheme::StatusInvalidParameter;
            errStr = "Missing key schema with specified partition boundaries";
            return nullptr;
        }
    }

    TString prevBound;
    for (ui32 i = 0; i < partitionCount; ++i) {
        using TKeyRange = TPQShardInfo::TKeyRange;
        TMaybe<TKeyRange> keyRange;

        if (op.PartitionBoundariesSize()) {
            keyRange.ConstructInPlace();

            if (i) {
                keyRange->FromBound = prevBound;
            }

            if (i != (partitionCount - 1)) {
                TVector<TCell> cells;
                TString error;
                if (!NMiniKQL::CellsFromTuple(nullptr, op.GetPartitionBoundaries(i), pqGroupInfo->KeySchema, false, cells, error)) {
                    status = NKikimrScheme::StatusSchemeError;
                    errStr = Sprintf("Invalid partition boundary at position: %u, error: %s", i, error.data());
                    return nullptr;
                }

                cells.resize(pqGroupInfo->KeySchema.size()); // Extend with NULLs
                keyRange->ToBound = TSerializedCellVec::Serialize(cells);
                prevBound = *keyRange->ToBound;
            }
        }

        pqGroupInfo->PartitionsToAdd.emplace(i, i + 1, keyRange);
    }

    if (partsPerTablet == 0 || partsPerTablet > TSchemeShard::MaxPQTabletPartitionsCount) {
        status = NKikimrScheme::StatusSchemeError;
        errStr = Sprintf("Invalid partition per tablet count specified: %u", partsPerTablet);
        return nullptr;
    }

    pqGroupInfo->NextPartitionId = partitionCount;
    pqGroupInfo->MaxPartsPerTablet = partsPerTablet;

    pqGroupInfo->TotalGroupCount = partitionCount;
    pqGroupInfo->TotalPartitionCount = partitionCount;

    ui32 tabletCount = pqGroupInfo->ExpectedShardCount();
    if (tabletCount > TSchemeShard::MaxPQGroupTabletsCount) {
        status = NKikimrScheme::StatusSchemeError;
        errStr = Sprintf("Invalid tablet count specified: %u", tabletCount);
        return nullptr;
    }

    NKikimrPQ::TPQTabletConfig tabletConfig = op.GetPQTabletConfig();
    tabletConfig.ClearPartitionIds();
    tabletConfig.ClearPartitions();

    if (!CheckPersQueueConfig(tabletConfig, false, &errStr)) {
        status = NKikimrScheme::StatusSchemeError;
        return nullptr;
    }

    const TPathElement::TPtr dbRootEl = context.SS->PathsById.at(context.SS->RootPathId());
    if (dbRootEl->UserAttrs->Attrs.contains("cloud_id")) {
        auto cloudId = dbRootEl->UserAttrs->Attrs.at("cloud_id");
        tabletConfig.SetYcCloudId(cloudId);
    }
    if (dbRootEl->UserAttrs->Attrs.contains("folder_id")) {
        auto folderId = dbRootEl->UserAttrs->Attrs.at("folder_id");
        tabletConfig.SetYcFolderId(folderId);
    }
    if (dbRootEl->UserAttrs->Attrs.contains("database_id")) {
        auto databaseId = dbRootEl->UserAttrs->Attrs.at("database_id");
        tabletConfig.SetYdbDatabaseId(databaseId);
    }
    const TString databasePath = TPath::Init(context.SS->RootPathId(), context.SS).PathString();
    tabletConfig.SetYdbDatabasePath(databasePath);

    Y_PROTOBUF_SUPPRESS_NODISCARD tabletConfig.SerializeToString(&pqGroupInfo->TabletConfig);

    if (op.HasBootstrapConfig()) {
        Y_PROTOBUF_SUPPRESS_NODISCARD op.GetBootstrapConfig().SerializeToString(&pqGroupInfo->BootstrapConfig);
    }

    return pqGroupInfo;
}

void ApplySharding(TTxId txId,
                   TPathId pathId,
                   TPersQueueGroupInfo::TPtr pqGroup,
                   TTxState& txState,
                   const TChannelsBindings& rbBindedChannels,
                   const TChannelsBindings& pqBindedChannels,
                   TSchemeShard* ss) {
    pqGroup->AlterVersion = 0;
    TShardInfo shardInfo = TShardInfo::PersQShardInfo(txId, pathId);
    shardInfo.BindedChannels = pqBindedChannels;
    Y_VERIFY(pqGroup->TotalGroupCount == pqGroup->PartitionsToAdd.size());
    const ui64 count = pqGroup->ExpectedShardCount();
    txState.Shards.reserve(count + 1);
    const auto startShardIdx = ss->ReserveShardIdxs(count + 1);
    for (ui64 i = 0; i < count; ++i) {
        const auto idx = ss->NextShardIdx(startShardIdx, i);
        ss->RegisterShardInfo(idx, shardInfo);
        txState.Shards.emplace_back(idx, ETabletType::PersQueue, TTxState::CreateParts);

        TPQShardInfo::TPtr pqShard = new TPQShardInfo();
        pqShard->PQInfos.reserve(pqGroup->MaxPartsPerTablet);
        pqGroup->Shards[idx] = pqShard;
    }

    const auto idx = ss->NextShardIdx(startShardIdx, count);
    ss->RegisterShardInfo(idx,
        TShardInfo::PQBalancerShardInfo(txId, pathId)
            .WithBindedChannels(rbBindedChannels));
    txState.Shards.emplace_back(idx, ETabletType::PersQueueReadBalancer, TTxState::CreateParts);
    pqGroup->BalancerShardIdx = idx;

    auto it = pqGroup->PartitionsToAdd.begin();
    for (ui32 pqId = 0; pqId < pqGroup->TotalGroupCount; ++pqId, ++it) {
        auto idx = ss->NextShardIdx(startShardIdx, pqId / pqGroup->MaxPartsPerTablet);
        TPQShardInfo::TPtr pqShard = pqGroup->Shards[idx];

        TPQShardInfo::TPersQueueInfo pqInfo;
        pqInfo.PqId = it->PartitionId;
        pqInfo.GroupId = it->GroupId;
        pqInfo.KeyRange = it->KeyRange;
        pqInfo.AlterVersion = 1;
        pqShard->PQInfos.push_back(pqInfo);
    }
}


class TCreatePQ: public TSubOperation {
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

    TTxState::ETxState NextState() {
        return TTxState::CreateParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return TTxState::ConfigureParts;
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
        return TTxState::Invalid;
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return THolder(new TCreateParts(OperationId));
        case TTxState::ConfigureParts:
            return THolder(new NPQState::TConfigureParts(OperationId));
        case TTxState::Propose:
            return THolder(new NPQState::TPropose(OperationId));
        case TTxState::Done:
            return THolder(new TDone(OperationId));
        default:
            return nullptr;
        }
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

public:
    TCreatePQ(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TCreatePQ(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto acceptExisted = !Transaction.GetFailOnExist();
        const auto& createDEscription = Transaction.GetCreatePersQueueGroup();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = createDEscription.GetName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreatePQ Propose"
                         << ", path: " << parentPathStr << "/" << name
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        TEvSchemeShard::EStatus status = NKikimrScheme::StatusAccepted;
        auto result = MakeHolder<TProposeResponse>(status, ui64(OperationId.GetTxId()), ui64(ssId));

        NSchemeShard::TPath parentPath = NSchemeShard::TPath::Resolve(parentPathStr, context.SS);
        {
            NSchemeShard::TPath::TChecker checks = parentPath.Check();
            checks
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting();

            if (checks) {
                if (parentPath.Base()->IsCdcStream()) {
                    checks
                        .IsUnderCreating(NKikimrScheme::StatusNameConflict)
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else {
                    checks
                        .IsCommonSensePath()
                        .IsLikeDirectory();
                }
            }

            if (!checks) {
                TString explain = TStringBuilder() << "parent path fail checks"
                                                   << ", path: " << parentPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        const TString acl = Transaction.GetModifyACL().GetDiffACL();

        NSchemeShard::TPath dstPath = parentPath.Child(name);
        {
            NSchemeShard::TPath::TChecker checks = dstPath.Check();
            checks.IsAtLocalSchemeShard();
            if (dstPath.IsResolved()) {
                checks
                    .IsResolved()
                    .NotUnderDeleting()
                    .FailOnExist(TPathElement::EPathType::EPathTypePersQueueGroup, acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .IsValidLeafName()
                    .DepthLimit()
                    .PathsLimit()
                    .DirChildrenLimit()
                    .IsValidACL(acl);
            }

            if (!checks) {
                TString explain = TStringBuilder() << "dst path fail checks"
                                                   << ", path: " << dstPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (dstPath.IsResolved()) {
                    result->SetPathCreateTxId(ui64(dstPath.Base()->CreateTxId));
                    result->SetPathId(dstPath.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        TString errStr;

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        TPersQueueGroupInfo::TPtr pqGroup = CreatePersQueueGroup(
            context, createDEscription, status, errStr);

        if (!pqGroup.Get()) {
            result->SetError(status, errStr);
            return result;
        }

        const ui64 shardsToCreate = pqGroup->ExpectedShardCount() + 1;
        const ui64 partitionsToCreate = pqGroup->TotalPartitionCount;

        auto tabletConfig = pqGroup->TabletConfig;
        NKikimrPQ::TPQTabletConfig config;
        Y_VERIFY(!tabletConfig.empty());

        bool parseOk = ParseFromStringNoSizeLimit(config, tabletConfig);
        Y_VERIFY(parseOk);

        const ui64 throughput = ((ui64)partitionsToCreate) *
                                config.GetPartitionConfig().GetWriteSpeedInBytesPerSecond();
        const ui64 storage = [&config, &throughput]() {
            if (config.GetPartitionConfig().HasStorageLimitBytes()) {
                return config.GetPartitionConfig().GetStorageLimitBytes();
            } else {
                return throughput * config.GetPartitionConfig().GetLifetimeSeconds();
            }
        }();

        {
            NSchemeShard::TPath::TChecker checks = dstPath.Check();
            checks
                .ShardsLimit(shardsToCreate)
                .PathShardsLimit(shardsToCreate)
                .PQPartitionsLimit(partitionsToCreate)
                .PQReservedStorageLimit(storage);

            if (!checks) {
                TString explain = TStringBuilder() << "dst path fail checks"
                                                   << ", path: " << dstPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (dstPath.IsResolved()) {
                    result->SetPathCreateTxId(ui64(dstPath.Base()->CreateTxId));
                    result->SetPathId(dstPath.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        // This profile id is only used for pq read balancer tablet when
        // explicit channel profiles are specified. Read balancer tablet is
        // a tablet with local db which doesn't use extra channels in any way.
        const ui32 tabletProfileId = 0;
        TChannelsBindings tabletChannelsBinding;
        if (!context.SS->ResolvePqChannels(tabletProfileId, dstPath.GetPathIdForDomain(), tabletChannelsBinding)) {
            result->SetError(NKikimrScheme::StatusInvalidParameter,
                             "Unable to construct channel binding for PQ with the storage pool");
            return result;
        }

        // This channel bindings are for PersQueue shards. They either use
        // explicit channel profiles, or reuse channel profile above.
        const auto& partConfig = createDEscription.GetPQTabletConfig().GetPartitionConfig();
        TChannelsBindings pqChannelsBinding;
        if (partConfig.ExplicitChannelProfilesSize() > 0) {
            const auto& ecps = partConfig.GetExplicitChannelProfiles();

            if (ecps.size() < 3 || ui32(ecps.size()) > NHive::MAX_TABLET_CHANNELS) {
                auto errStr = Sprintf("ExplicitChannelProfiles has %u channels, should be [3 .. %lu]",
                                    ecps.size(),
                                    NHive::MAX_TABLET_CHANNELS);
                result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
                return result;
            }

            TVector<TStringBuf> partitionPoolKinds;
            partitionPoolKinds.reserve(ecps.size());
            for (const auto& ecp : ecps) {
                partitionPoolKinds.push_back(ecp.GetPoolKind());
            }

            const auto resolved = context.SS->ResolveChannelsByPoolKinds(
                partitionPoolKinds,
                dstPath.GetPathIdForDomain(),
                pqChannelsBinding);
            if (!resolved) {
                result->SetError(NKikimrScheme::StatusInvalidParameter,
                                "Unable to construct channel binding for PersQueue with the storage pool");
                return result;
            }

            context.SS->SetPqChannelsParams(ecps, pqChannelsBinding);
        } else {
            pqChannelsBinding = tabletChannelsBinding;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxCreatePQGroup, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        dstPath.MaterializeLeaf(owner);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);

        context.SS->TabletCounters->Simple()[COUNTER_PQ_GROUP_COUNT].Add(1);

        TPathId pathId = dstPath.Base()->PathId;

        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCreatePQGroup, pathId);

        ApplySharding(OperationId.GetTxId(), pathId, pqGroup, txState, tabletChannelsBinding, pqChannelsBinding, context.SS);

        NIceDb::TNiceDb db(context.GetDB());

        for (auto& shard : pqGroup->Shards) {
            auto shardIdx = shard.first;
            for (const auto& pqInfo : shard.second->PQInfos) {
                context.SS->PersistPersQueue(db, pathId, shardIdx, pqInfo);
            }
        }

        TPersQueueGroupInfo::TPtr emptyGroup = new TPersQueueGroupInfo;
        emptyGroup->Shards.swap(pqGroup->Shards);

        context.SS->PersQueueGroups[pathId] = emptyGroup;
        context.SS->PersQueueGroups[pathId]->AlterData = pqGroup;
        context.SS->IncrementPathDbRefCount(pathId);

        context.SS->PersistPersQueueGroup(db, pathId, emptyGroup);
        context.SS->PersistAddPersQueueGroupAlter(db, pathId, pqGroup);

        for (auto shard : txState.Shards) {
            Y_VERIFY(shard.Operation == TTxState::CreateParts);
            context.SS->PersistShardMapping(db, shard.Idx, InvalidTabletId, pathId, OperationId.GetTxId(), shard.TabletType);
            context.SS->PersistChannelsBinding(db, shard.Idx, tabletChannelsBinding);
        }
        Y_VERIFY(txState.Shards.size() == shardsToCreate);
        context.SS->TabletCounters->Simple()[COUNTER_PQ_SHARD_COUNT].Add(shardsToCreate-1);
        context.SS->TabletCounters->Simple()[COUNTER_PQ_RB_SHARD_COUNT].Add(1);

        dstPath.Base()->CreateTxId = OperationId.GetTxId();
        dstPath.Base()->LastTxId = OperationId.GetTxId();
        dstPath.Base()->PathState = TPathElement::EPathState::EPathStateCreate;
        dstPath.Base()->PathType = TPathElement::EPathType::EPathTypePersQueueGroup;

        if (parentPath.Base()->HasActiveChanges()) {
            TTxId parentTxId = parentPath.Base()->PlannedToCreate() ? parentPath.Base()->CreateTxId : parentPath.Base()->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }

        context.SS->ChangeTxState(db, OperationId, TTxState::CreateParts);
        context.OnComplete.ActivateTx(OperationId);

        context.SS->PersistTxState(db, OperationId);

        context.SS->PersistPath(db, dstPath.Base()->PathId);

        if (!acl.empty()) {
            dstPath.Base()->ApplyACL(acl);
            context.SS->PersistACL(db, dstPath.Base());
        }

        context.SS->PersistUpdateNextPathId(db);
        context.SS->PersistUpdateNextShardIdx(db);

        ++parentPath.Base()->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath.Base());
        context.SS->ClearDescribePathCaches(parentPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, parentPath.Base()->PathId);

        context.SS->ClearDescribePathCaches(dstPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, dstPath.Base()->PathId);

        dstPath.DomainInfo()->IncPathsInside();
        dstPath.DomainInfo()->AddInternalShards(txState);
        dstPath.DomainInfo()->IncPQPartitionsInside(partitionsToCreate);
        dstPath.DomainInfo()->IncPQReservedStorage(storage);

        context.SS->TabletCounters->Simple()[COUNTER_STREAM_RESERVED_THROUGHPUT].Add(throughput);
        context.SS->TabletCounters->Simple()[COUNTER_STREAM_RESERVED_STORAGE].Add(storage);

        context.SS->TabletCounters->Simple()[COUNTER_STREAM_SHARDS_COUNT].Add(partitionsToCreate);

        dstPath.Base()->IncShardsInside(shardsToCreate);
        parentPath.Base()->IncAliveChildren();

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TCreatePQ");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreatePQ AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateNewPQ(TOperationId id, const TTxTransaction& tx) {
    return new TCreatePQ(id, tx);
}

ISubOperationBase::TPtr CreateNewPQ(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TCreatePQ(id, state);
}

}
}
