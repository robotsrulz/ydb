#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_path_element.h"

#include "schemeshard_impl.h"

#include <ydb/core/base/path.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>

namespace NKikimr {
namespace NSchemeShard {

TVector<ISubOperationBase::TPtr> CreateConsistentMoveTable(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_VERIFY(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpMoveTable);

    TVector<ISubOperationBase::TPtr> result;

    const auto& moving = tx.GetMoveTable();
    const auto& srcStr = moving.GetSrcPath();
    const auto& dstStr = moving.GetDstPath();

    {
        TString errStr;
        if (!context.SS->CheckApplyIf(tx, errStr)) {
            return {CreateReject(nextId, NKikimrScheme::EStatus::StatusPreconditionFailed, errStr)};
        }
    }

    TPath srcPath = TPath::Resolve(srcStr, context.SS);
    {
        TPath::TChecker checks = srcPath.Check();
        checks.IsResolved()
              .NotDeleted()
              .IsTable()
              .IsCommonSensePath();

        if (!checks) {
            TStringBuilder explain = TStringBuilder() << "src path fail checks"
                                           << ", path: " << srcStr;
            auto status = checks.GetStatus(&explain);
            return {CreateReject(nextId, status, explain)};
        }
    }

    {
        TStringBuilder explain = TStringBuilder() << "fail checks";

        if (!context.SS->CheckLocks(srcPath.Base()->PathId, tx, explain)) {
            return {CreateReject(nextId, NKikimrScheme::StatusMultipleModifications, explain)};
        }
    }

    TPath dstPath = TPath::Resolve(dstStr, context.SS);

    result.push_back(CreateMoveTable(TOperationId(nextId.GetTxId(),
                                               nextId.GetSubTxId() + result.size()),
                                     MoveTableTask(srcPath, dstPath)));

    for (auto& child: srcPath.Base()->GetChildren()) {
        auto name = child.first;

        TPath srcChildPath = srcPath.Child(name);
        if (srcChildPath.IsDeleted()) {
            continue;
        }

        if (srcChildPath.IsCdcStream()) {
            return {CreateReject(nextId, NKikimrScheme::StatusPreconditionFailed, "Cannot move table with cdc streams")};
        }

        TPath dstIndexPath = dstPath.Child(name);

        Y_VERIFY(srcChildPath.Base()->PathId == child.second);
        Y_VERIFY_S(srcChildPath.Base()->GetChildren().size() == 1,
                   srcChildPath.PathString() << " has children " << srcChildPath.Base()->GetChildren().size());

        result.push_back(CreateMoveTableIndex(TOperationId(nextId.GetTxId(),
                                                              nextId.GetSubTxId() + result.size()),
                                                 MoveTableIndexTask(srcChildPath, dstIndexPath)));

        TString srcImplTableName = srcChildPath.Base()->GetChildren().begin()->first;
        TPath srcImplTable = srcChildPath.Child(srcImplTableName);
        if (srcImplTable.IsDeleted()) {
            continue;
        }
        Y_VERIFY(srcImplTable.Base()->PathId == srcChildPath.Base()->GetChildren().begin()->second);

        TPath dstImplTable = dstIndexPath.Child(srcImplTableName);

        result.push_back(CreateMoveTable(TOperationId(nextId.GetTxId(),
                                                      nextId.GetSubTxId() + result.size()),
                                         MoveTableTask(srcImplTable, dstImplTable)));
    }

    return result;
}

}
}
