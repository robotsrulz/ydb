#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TAlterUserAttrs: public ISubOperationBase {
    const TOperationId OperationId;
    const TTxTransaction Transaction;

public:
    TAlterUserAttrs(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TAlterUserAttrs(TOperationId id)
        : OperationId(id)
    {
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto& userAttrsPatch = Transaction.GetAlterUserAttributes();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = userAttrsPatch.GetPathName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TAlterUserAttrs Propose"
                         << ", path: " << parentPathStr << "/" << name
                         << ", operationId: " << OperationId
                         << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId));

        if (!Transaction.HasAlterUserAttributes()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "UserAttributes are not present");
            return result;
        }

        if (!userAttrsPatch.HasPathName()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "Name is not present in UserAttributes");
            return result;
        }

        TPath path = TPath::Resolve(parentPathStr, context.SS).Dive(name);
        {
            TPath::TChecker checks = path.Check();
            checks.NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderOperation()
                .IsCommonSensePath(); //forbid alter user attrs at impl index tables and indexes

            if (!checks) {
                TString explain = TStringBuilder() << "path fail checks"
                                                   << ", path: " << path.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        TString errStr;

        TUserAttributes::TPtr alterData = path.Base()->UserAttrs->CreateNextVersion();
        if (!alterData->ApplyPatch(EUserAttributesOp::AlterUserAttrs, userAttrsPatch, errStr) ||
            !alterData->CheckLimits(errStr))
        {
            result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
            return result;
        }

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxAlterUserAttributes, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        NIceDb::TNiceDb db(context.GetDB());

        Y_VERIFY(!context.SS->FindTx(OperationId));
        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxAlterUserAttributes, path.Base()->PathId);

        path.Base()->PathState = NKikimrSchemeOp::EPathStateAlter;
        path.Base()->LastTxId = OperationId.GetTxId();
        path.Base()->UserAttrs->AlterData = alterData;
        context.SS->PersistAlterUserAttributes(db, path.Base()->PathId);

        txState.State = TTxState::Propose;
        context.SS->PersistTxState(db, OperationId);

        context.OnComplete.ActivateTx(OperationId);
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TAlterUserAttrs");
    }

    void ProgressState(TOperationContext& context) override {
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TAlterUserAttrs ProgressState"
                       << ", opId: " << OperationId
                       << ", at schemeshard: " << context.SS->TabletID());

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
    }

    void HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const TStepId step = TStepId(ev->Get()->StepId);
        const TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TAlterUserAttrs HandleReply TEvOperationPlan"
                       << ", opId: " << OperationId
                       << ", stepId:" << step
                       << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        if (txState->State != TTxState::Propose) {
            LOG_WARN_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "Duplicate PlanStep opId#" << OperationId
                           << " at schemeshard: " << ssId
                           << " txState is in state#" << TTxState::StateName(txState->State));
            return;
        }

        Y_VERIFY(txState->TxType == TTxState::TxAlterUserAttributes);

        TPathId pathId = txState->TargetPathId;
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);
        context.OnComplete.ReleasePathState(OperationId, pathId, TPathElement::EPathState::EPathStateNoChanges);

        NIceDb::TNiceDb db(context.GetDB());

        Y_VERIFY(path->UserAttrs);
        Y_VERIFY(path->UserAttrs->AlterData);
        Y_VERIFY(path->UserAttrs->AlterVersion < path->UserAttrs->AlterData->AlterVersion);
        context.SS->ApplyAndPersistUserAttrs(db, path->PathId);

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.OnComplete.UpdateTenants({pathId});

        context.OnComplete.DoneOperation(OperationId);
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TAlterUserAttrs AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateAlterUserAttrs(TOperationId id, const TTxTransaction& tx) {
    return new TAlterUserAttrs(id, tx);
}

ISubOperationBase::TPtr CreateAlterUserAttrs(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state == TTxState::Invalid || state == TTxState::Propose);
    return new TAlterUserAttrs(id);
}

}
}
