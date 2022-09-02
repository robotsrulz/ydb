#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/mind/hive/hive.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

////////////////////////////////////////////////////////////////////////////////

class TConfigureParts: public TSubOperationState {
private:
    const TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TCreateFileStore::TConfigureParts"
            << " operationId#" << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {
            TEvHive::TEvCreateTabletReply::EventType
        });
    }

    bool HandleReply(
        TEvFileStore::TEvUpdateConfigResponse::TPtr& ev,
        TOperationContext& context) override
    {
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " HandleReply TEvUpdateConfigResponse"
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateFileStore);
        Y_VERIFY(txState->State == TTxState::ConfigureParts);

        auto tabletId = TTabletId(ev->Get()->Record.GetOrigin());
        auto status = ev->Get()->Record.GetStatus();

        // Schemeshard never sends invalid or outdated configs
        Y_VERIFY_S(status == NKikimrFileStore::OK || status == NKikimrFileStore::ERROR_UPDATE_IN_PROGRESS,
            "Unexpected error in UpdateConfigResponse"
            << ", status: " << NKikimrFileStore::EStatus_Name(status)
            << ", tx: " << OperationId
            << ", tablet: " << tabletId
            << ", at schemeshard: " << ssId);

        if (status == NKikimrFileStore::ERROR_UPDATE_IN_PROGRESS) {
            LOG_ERROR_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " Reconfiguration is in progress. We'll try to finish it later."
                << " tx: " << OperationId
                << " tablet: " << tabletId);
            return false;
        }

        auto idx = context.SS->MustGetShardIdx(tabletId);
        txState->ShardsInProgress.erase(idx);

        context.OnComplete.UnbindMsgFromPipe(OperationId, tabletId, idx);

        if (txState->ShardsInProgress.empty()) {
            NIceDb::TNiceDb db(context.GetDB());
            context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
            context.OnComplete.ActivateTx(OperationId);
            return true;
        }

        return false;
    }

    bool ProgressState(TOperationContext& context) override {
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " ProgressState"
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateFileStore);
        Y_VERIFY(!txState->Shards.empty());

        txState->ClearShardsInProgress();

        auto fs = context.SS->FileStoreInfos[txState->TargetPathId];
        Y_VERIFY_S(fs, "FileStore info is null. PathId: " << txState->TargetPathId);

        Y_VERIFY(txState->Shards.size() == 1);
        for (const auto& shard: txState->Shards) {
            Y_VERIFY(shard.TabletType == ETabletType::FileStore);
            auto shardIdx = shard.Idx;
            auto tabletId = context.SS->ShardInfos[shardIdx].TabletID;

            fs->IndexShardIdx = shardIdx;
            fs->IndexTabletId = tabletId;

            TAutoPtr<TEvFileStore::TEvUpdateConfig> event(new TEvFileStore::TEvUpdateConfig());
            event->Record.SetTxId(ui64(OperationId.GetTxId()));
            event->Record.MutableConfig()->CopyFrom(fs->Config);
            event->Record.MutableConfig()->SetVersion(fs->Version);

            context.OnComplete.BindMsgToPipe(OperationId, tabletId, shardIdx, event.Release());

            // Wait for results from this shard
            txState->ShardsInProgress.insert(shardIdx);
        }

        return false;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TPropose: public TSubOperationState {
private:
    const TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TCreateFileStore::TPropose"
            << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {
            TEvHive::TEvCreateTabletReply::EventType,
            TEvFileStore::TEvUpdateConfigResponse::EventType
        });
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const auto step = TStepId(ev->Get()->StepId);
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " HandleReply TEvOperationPlan"
            << ", step: " << step
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        if (!txState) {
            return false;
        }

        Y_VERIFY(txState->TxType == TTxState::TxCreateFileStore);
        TPathId pathId = txState->TargetPathId;

        TPathElement::TPtr path = context.SS->PathsById.at(pathId);
        path->StepCreated = step;

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->PersistCreateStep(db, pathId, step);

        auto parentDir = context.SS->PathsById.at(path->ParentPathId);
        ++parentDir->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentDir);
        context.SS->ClearDescribePathCaches(parentDir);
        context.OnComplete.PublishToSchemeBoard(OperationId, parentDir->PathId);

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " ProgressState"
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateFileStore);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TCreateFileStore: public TSubOperation {
private:
    const TOperationId OperationId;
    const TTxTransaction Transaction;

    TTxState::ETxState State = TTxState::Invalid;

public:
    TCreateFileStore(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TCreateFileStore(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(
        const TString& owner,
        TOperationContext& context) override;

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TCreateFileStore");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            "TCreateFileStore AbortUnsafe"
            << ", opId: " << OperationId
            << ", forceDropId: " << forceDropTxId
            << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

private:
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
            return MakeHolder<TCreateParts>(OperationId);
        case TTxState::ConfigureParts:
            return MakeHolder<TConfigureParts>(OperationId);
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

    TFileStoreInfo::TPtr CreateFileStoreInfo(
        const NKikimrSchemeOp::TFileStoreDescription& op,
        TEvSchemeShard::EStatus& status,
        TString& errStr);

    TTxState& PrepareChanges(
        TOperationId operationId,
        TPathElement::TPtr parentDir,
        TPathElement::TPtr fsPath,
        TFileStoreInfo::TPtr fs,
        const TString& acl,
        const TChannelsBindings& tabletChannels,
        TOperationContext& context);
};

////////////////////////////////////////////////////////////////////////////////

THolder<TProposeResponse> TCreateFileStore::Propose(
    const TString& owner,
    TOperationContext& context)
{
    const auto ssId = context.SS->SelfTabletId();

    const auto acceptExisted = !Transaction.GetFailOnExist();
    const auto& operation = Transaction.GetCreateFileStore();
    const TString& parentPathStr = Transaction.GetWorkingDir();
    const TString& name = Transaction.GetCreateFileStore().GetName();
    const ui64 shardsToCreate = 1;

    LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
        "TCreateFileStore Propose"
        << ", path: " << parentPathStr << "/" << name
        << ", opId: " << OperationId
        << ", at schemeshard: " << ssId);

    auto status = NKikimrScheme::StatusAccepted;
    auto result = MakeHolder<TProposeResponse>(
        status,
        ui64(OperationId.GetTxId()),
        ui64(ssId));

    auto parentPath = NSchemeShard::TPath::Resolve(parentPathStr, context.SS);
    {
        auto checks = parentPath.Check();
        checks
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .NotUnderDeleting()
            .IsCommonSensePath()
            .IsLikeDirectory();

        if (!checks) {
            TString explain = TStringBuilder()
                << "parent path fail checks"
                << ", path: " << parentPath.PathString();

            auto status = checks.GetStatus(&explain);
            result->SetError(status, explain);
            return result;
        }
    }

    const TString acl = Transaction.GetModifyACL().GetDiffACL();

    auto dstPath = parentPath.Child(name);
    {
        auto checks = dstPath.Check();
        checks.IsAtLocalSchemeShard();
        if (dstPath.IsResolved()) {
            checks
                .IsResolved()
                .NotUnderDeleting()
                .FailOnExist(TPathElement::EPathType::EPathTypeFileStore, acceptExisted);
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
                .ShardsLimit(shardsToCreate)
                .PathShardsLimit(shardsToCreate)
                .IsValidACL(acl);
        }

        if (!checks) {
            TString explain = TStringBuilder()
                << "dst path fail checks"
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

    const auto& ecps = operation.GetConfig().GetExplicitChannelProfiles();
    if (ecps.empty() || ui32(ecps.size()) > NHive::MAX_TABLET_CHANNELS) {
        auto errStr = Sprintf("Wrong number of channels %u , should be [1 .. %lu]",
            ecps.size(), NHive::MAX_TABLET_CHANNELS);

        result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
        return result;
    }

    TVector<TStringBuf> storePoolKinds(Reserve(ecps.size()));
    for (const auto& ecp : ecps) {
        storePoolKinds.push_back(ecp.GetPoolKind());
    }

    TChannelsBindings storeChannelBindings;
    const auto storeChannelsResolved = context.SS->ResolveChannelsByPoolKinds(
        storePoolKinds,
        dstPath.GetPathIdForDomain(),
        storeChannelBindings
    );

    if (!storeChannelsResolved) {
        result->SetError(NKikimrScheme::StatusInvalidParameter,
                         "Unable to construct channel binding for filestore with the storage pool");
        return result;
    }

    context.SS->SetNfsChannelsParams(ecps, storeChannelBindings);

    TString errStr;
    if (!context.SS->CheckApplyIf(Transaction, errStr)) {
        result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
        return result;
    }

    auto fs = CreateFileStoreInfo(operation, status, errStr);
    if (!fs) {
        result->SetError(status, errStr);
        return result;
    }
    if (!context.SS->CheckInFlightLimit(TTxState::TxCreateFileStore, errStr)) {
        result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
        return result;
    }

    dstPath.MaterializeLeaf(owner);
    result->SetPathId(dstPath.Base()->PathId.LocalPathId);

    context.SS->TabletCounters->Simple()[COUNTER_FILESTORE_COUNT].Add(1);

    const TTxState& txState = PrepareChanges(
        OperationId,
        parentPath.Base(),
        dstPath.Base(),
        fs,
        acl,
        storeChannelBindings,
        context);

    NIceDb::TNiceDb db(context.GetDB());
    ++parentPath.Base()->DirAlterVersion;
    context.SS->PersistPathDirAlterVersion(db, parentPath.Base());
    context.SS->ClearDescribePathCaches(parentPath.Base());
    context.OnComplete.PublishToSchemeBoard(OperationId, parentPath.Base()->PathId);

    context.SS->ClearDescribePathCaches(dstPath.Base());
    context.OnComplete.PublishToSchemeBoard(OperationId, dstPath.Base()->PathId);

    dstPath.DomainInfo()->IncPathsInside();
    dstPath.DomainInfo()->AddInternalShards(txState);
    dstPath.Base()->IncShardsInside(shardsToCreate);
    parentPath.Base()->IncAliveChildren();

    State = NextState();
    SetState(SelectStateFunc(State));
    return result;
}

TFileStoreInfo::TPtr TCreateFileStore::CreateFileStoreInfo(
    const NKikimrSchemeOp::TFileStoreDescription& op,
    TEvSchemeShard::EStatus& status,
    TString& errStr)
{
    TFileStoreInfo::TPtr fs = new TFileStoreInfo();

    const auto& config = op.GetConfig();
    if (!config.HasBlockSize()) {
        status = NKikimrScheme::StatusSchemeError;
        errStr = "Block size is required";
        return nullptr;
    }

    if (config.HasVersion()) {
        status = NKikimrScheme::StatusSchemeError;
        errStr = "Setting version is not allowed";
        return nullptr;
    }

    fs->Version = 1;
    fs->Config.CopyFrom(op.GetConfig());

    return fs;
}

TTxState& TCreateFileStore::PrepareChanges(
    TOperationId operationId,
    TPathElement::TPtr parentDir,
    TPathElement::TPtr fsPath,
    TFileStoreInfo::TPtr fs,
    const TString& acl,
    const TChannelsBindings& tabletChannels,
    TOperationContext& context)
{
    NIceDb::TNiceDb db(context.GetDB());

    fsPath->CreateTxId = operationId.GetTxId();
    fsPath->LastTxId = operationId.GetTxId();
    fsPath->PathState = TPathElement::EPathState::EPathStateCreate;
    fsPath->PathType = TPathElement::EPathType::EPathTypeFileStore;
    TPathId pathId = fsPath->PathId;

    TTxState& txState = context.SS->CreateTx(operationId, TTxState::TxCreateFileStore, pathId);

    auto shardIdx = context.SS->RegisterShardInfo(
        TShardInfo::FileStoreInfo(operationId.GetTxId(), pathId)
            .WithBindedChannels(tabletChannels));
    context.SS->TabletCounters->Simple()[COUNTER_FILESTORE_SHARD_COUNT].Add(1);
    txState.Shards.emplace_back(shardIdx, ETabletType::FileStore, TTxState::CreateParts);
    fs->IndexShardIdx = shardIdx;

    if (parentDir->HasActiveChanges()) {
        TTxId parentTxId = parentDir->PlannedToCreate() ? parentDir->CreateTxId : parentDir->LastTxId;
        context.OnComplete.Dependence(parentTxId, operationId.GetTxId());
    }

    context.SS->ChangeTxState(db, operationId, TTxState::CreateParts);
    context.OnComplete.ActivateTx(operationId);

    context.SS->PersistPath(db, fsPath->PathId);
    if (!acl.empty()) {
        fsPath->ApplyACL(acl);
        context.SS->PersistACL(db, fsPath);
    }

    context.SS->FileStoreInfos[pathId] = fs;
    context.SS->PersistFileStoreInfo(db, pathId, fs);
    context.SS->IncrementPathDbRefCount(pathId);

    context.SS->PersistTxState(db, operationId);
    context.SS->PersistUpdateNextPathId(db);
    context.SS->PersistUpdateNextShardIdx(db);

    for (const auto& shard: txState.Shards) {
        Y_VERIFY(shard.Operation == TTxState::CreateParts);
        context.SS->PersistChannelsBinding(db, shard.Idx, context.SS->ShardInfos[shard.Idx].BindedChannels);
        context.SS->PersistShardMapping(db, shard.Idx, InvalidTabletId, pathId, operationId.GetTxId(), shard.TabletType);
    }

    return txState;
}

}   // namespace

namespace NKikimr {
namespace NSchemeShard {

////////////////////////////////////////////////////////////////////////////////

ISubOperationBase::TPtr CreateNewFileStore(TOperationId id, const TTxTransaction& tx) {
    return new TCreateFileStore(id, tx);
}

ISubOperationBase::TPtr CreateNewFileStore(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TCreateFileStore(id, state);
}

}   // namespace NSchemeShard
}   // namespace NKikimr
