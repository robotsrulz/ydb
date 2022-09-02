#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/persqueue/config/config.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TCreateExtSubDomain: public TSubOperation {
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

    TTxState::ETxState NextState() {
        return TTxState::Propose;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
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
        case TTxState::Propose:
            return THolder(new NSubDomainState::TPropose(OperationId));
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
    TCreateExtSubDomain(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TCreateExtSubDomain(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
          , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto& settings = Transaction.GetSubDomain();
        const auto acceptExisted = !Transaction.GetFailOnExist();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = settings.GetName();

        ui64 shardsToCreate = settings.GetCoordinators() + settings.GetMediators();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateExtSubDomain Propose"
                         << ", path" << parentPathStr << "/" << name
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        TEvSchemeShard::EStatus status = NKikimrScheme::StatusAccepted;
        auto result = MakeHolder<TProposeResponse>(status, ui64(OperationId.GetTxId()), ui64(ssId));

        if (!parentPathStr) {
            result->SetError(NKikimrScheme::StatusInvalidParameter,
                             "Malformed subdomain request: no working dir");
            return result;
        }

        if (!name) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: no name");
            return result;
        }

        NSchemeShard::TPath parentPath = NSchemeShard::TPath::Resolve(parentPathStr, context.SS);
        {
            NSchemeShard::TPath::TChecker checks = parentPath.Check();
            checks
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsCommonSensePath()
                .IsLikeDirectory();

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
                    .FailOnExist(TPathElement::EPathType::EPathTypeExtSubDomain, acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .IsValidLeafName()
                    .DepthLimit()
                    .PathsLimit() //check capacity on root Domain
                    .DirChildrenLimit()
                    .PathShardsLimit(shardsToCreate)
                    .ShardsLimit(shardsToCreate) //check capacity on root Domain
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

        const bool onlyDeclaration = settings.GetTimeCastBucketsPerMediator() == 0 &&
            settings.GetPlanResolution() == 0 &&
            settings.GetCoordinators() == 0 &&
            settings.GetMediators() == 0;

        if (!onlyDeclaration) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: only declaration at creation is allowed, do not set up tables");
            return result;
        }

        TPathId resourcesDomainId;
        if (settings.HasResourcesDomainKey()) {
            const auto& resourcesDomainKey = settings.GetResourcesDomainKey();
            resourcesDomainId = TPathId(resourcesDomainKey.GetSchemeShard(), resourcesDomainKey.GetPathId());

            if (!context.SS->SubDomains.contains(resourcesDomainId)) {
                result->SetError(NKikimrScheme::StatusInvalidParameter, "Unknown resources domain key");
                return result;
            }
        }

        auto domainPathId = parentPath.GetPathIdForDomain();
        Y_VERIFY(context.SS->PathsById.contains(domainPathId));
        Y_VERIFY(context.SS->SubDomains.contains(domainPathId));
        if (domainPathId != context.SS->RootPathId()) {
            result->SetError(NKikimrScheme::StatusNameConflict, "Nested subdomains is forbidden");
            return result;
        }


        bool requestedStoragePools = !settings.GetStoragePools().empty();
        if (requestedStoragePools) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: only declaration at creation is allowed, do not set up storage");
            return result;
        }

        const auto& userAttrsDetails = Transaction.GetAlterUserAttributes();
        TUserAttributes::TPtr userAttrs = new TUserAttributes(1);

        TString errStr;

        if (!userAttrs->ApplyPatch(EUserAttributesOp::CreateExtSubDomain, userAttrsDetails, errStr) ||
            !userAttrs->CheckLimits(errStr))
        {
            result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
            return result;
        }

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxCreateExtSubDomain, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        dstPath.MaterializeLeaf(owner);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);

        TPathElement::TPtr newNode = dstPath.Base();
        newNode->CreateTxId = OperationId.GetTxId();
        newNode->LastTxId = OperationId.GetTxId();
        newNode->PathState = TPathElement::EPathState::EPathStateCreate;
        newNode->PathType = TPathElement::EPathType::EPathTypeExtSubDomain;
        newNode->UserAttrs->AlterData = userAttrs;
        newNode->DirAlterVersion = 1;

        NIceDb::TNiceDb db(context.GetDB());

        context.SS->PersistPath(db, newNode->PathId);
        context.SS->ApplyAndPersistUserAttrs(db, newNode->PathId);

        if (!acl.empty()) {
            newNode->ApplyACL(acl);
            context.SS->PersistACL(db, newNode);
        }

        context.SS->PersistUpdateNextPathId(db);

        context.SS->TabletCounters->Simple()[COUNTER_EXTSUB_DOMAIN_COUNT].Add(1);

        Y_VERIFY(!context.SS->FindTx(OperationId));
        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCreateExtSubDomain, newNode->PathId);

        TSubDomainInfo::TPtr alter = new TSubDomainInfo(1, 0, 0, resourcesDomainId ? resourcesDomainId : newNode->PathId);
        alter->SetSchemeLimits(parentPath.DomainInfo()->GetSchemeLimits()); //inherit from root

        if (resourcesDomainId) {
            TSubDomainInfo::TPtr resourcesDomain = context.SS->SubDomains.at(resourcesDomainId);
            TTabletId sharedHive = context.SS->GetGlobalHive(context.Ctx);
            if (resourcesDomain->GetTenantHiveID()) {
                sharedHive = resourcesDomain->GetTenantHiveID();
            }

            alter->SetSharedHive(sharedHive);
        }

        if (settings.HasDeclaredSchemeQuotas()) {
            alter->SetDeclaredSchemeQuotas(settings.GetDeclaredSchemeQuotas());
        }

        if (settings.HasDatabaseQuotas()) {
            alter->SetDatabaseQuotas(settings.GetDatabaseQuotas());
        }

        Y_VERIFY(!context.SS->SubDomains.contains(newNode->PathId));
        auto& subDomainInfo = context.SS->SubDomains[newNode->PathId];
        subDomainInfo = new TSubDomainInfo();
        subDomainInfo->SetAlter(alter);

        context.SS->PersistSubDomain(db, newNode->PathId, *subDomainInfo);
        context.SS->PersistSubDomainAlter(db, newNode->PathId, *alter);
        context.SS->IncrementPathDbRefCount(newNode->PathId);

        if (parentPath.Base()->HasActiveChanges()) {
            TTxId parentTxId = parentPath.Base()->PlannedToCreate() ? parentPath.Base()->CreateTxId : parentPath.Base()->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }

        txState.State = TTxState::Propose;
        context.OnComplete.ActivateTx(OperationId);

        context.SS->PersistTxState(db, OperationId);

        ++parentPath.Base()->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath.Base());
        context.SS->ClearDescribePathCaches(parentPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, parentPath.Base()->PathId);

        context.SS->ClearDescribePathCaches(newNode);
        context.OnComplete.PublishToSchemeBoard(OperationId, newNode->PathId);

        Y_VERIFY(0 == txState.Shards.size());
        parentPath.DomainInfo()->IncPathsInside();
        parentPath.Base()->IncAliveChildren();

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TCreateExtSubDomain");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateExtSubDomain AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateExtSubDomain(TOperationId id, const TTxTransaction& tx) {
    return new TCreateExtSubDomain(id, tx);
}

ISubOperationBase::TPtr CreateExtSubDomain(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TCreateExtSubDomain(id, state);
}

}
}
