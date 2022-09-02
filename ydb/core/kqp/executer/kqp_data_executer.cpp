#include "kqp_executer.h"
#include "kqp_executer_impl.h"
#include "kqp_locks_helper.h"
#include "kqp_partition_helper.h"
#include "kqp_planner.h"
#include "kqp_result_channel.h"
#include "kqp_table_resolver.h"
#include "kqp_tasks_validate.h"
#include "kqp_shards_resolver.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/base/wilson.h>
#include <ydb/core/client/minikql_compile/db_key_resolver.h>
#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/compute_actor/kqp_compute_actor.h>
#include <ydb/core/kqp/kqp.h>
#include <ydb/core/kqp/runtime/kqp_transport.h>
#include <ydb/core/kqp/prepare/kqp_query_plan.h>
#include <ydb/core/tx/coordinator/coordinator_impl.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/long_tx_service/public/events.h>
#include <ydb/core/tx/long_tx_service/public/lock_handle.h>
#include <ydb/core/tx/tx_proxy/proxy.h>

#include <ydb/library/yql/dq/runtime/dq_columns_resolve.h>
#include <ydb/library/yql/dq/tasks/dq_connection_builder.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>

namespace NKikimr {
namespace NKqp {

using namespace NYql;
using namespace NYql::NDq;
using namespace NLongTxService;

namespace {

static constexpr TDuration MinReattachDelay = TDuration::MilliSeconds(10);
static constexpr TDuration MaxReattachDelay = TDuration::MilliSeconds(100);
static constexpr TDuration MaxReattachDuration = TDuration::Seconds(4);
static constexpr ui32 ReplySizeLimit = 48 * 1024 * 1024; // 48 MB

class TKqpDataExecuter : public TKqpExecuterBase<TKqpDataExecuter, EExecType::Data> {
    using TBase = TKqpExecuterBase<TKqpDataExecuter, EExecType::Data>;
    using TKqpSnapshot = IKqpGateway::TKqpSnapshot;

    struct TEvPrivate {
        enum EEv {
            EvReattachToShard = EventSpaceBegin(TEvents::ES_PRIVATE),
        };

        struct TEvReattachToShard : public TEventLocal<TEvReattachToShard, EvReattachToShard> {
            const ui64 TabletId;

            explicit TEvReattachToShard(ui64 tabletId)
                : TabletId(tabletId) {}
        };
    };

    struct TReattachState {
        TDuration Delay;
        TInstant Deadline;
        ui64 Cookie = 0;
        bool Reattaching = false;

        bool ShouldReattach(TInstant now) {
            ++Cookie; // invalidate any previous cookie

            if (!Reattaching) {
                Deadline = now + MaxReattachDuration;
                Delay = TDuration::Zero();
                Reattaching = true;
                return true;
            }

            TDuration left = Deadline - now;
            if (!left) {
                Reattaching = false;
                return false;
            }

            Delay *= 2.0;
            if (Delay < MinReattachDelay) {
                Delay = MinReattachDelay;
            } else if (Delay > MaxReattachDelay) {
                Delay = MaxReattachDelay;
            }

            // Add ±10% jitter
            Delay *= 0.9 + 0.2 * TAppData::RandomProvider->GenRandReal4();
            if (Delay > left) {
                Delay = left;
            }

            return true;
        }

        void Reattached() {
            Reattaching = false;
        }
    };

    struct TShardState {
        enum class EState {
            Initial,
            Preparing,      // planned tx only
            Prepared,       // planned tx only
            Executing,
            Finished
        };

        EState State = EState::Initial;
        TSet<ui64> TaskIds;

        struct TDatashardState {
            ui64 ShardMinStep = 0;
            ui64 ShardMaxStep = 0;
            ui64 ReadSize = 0;
            bool ShardReadLocks = false;
            bool Follower = false;
        };
        TMaybe<TDatashardState> DatashardState;

        TReattachState ReattachState;
        ui32 RestartCount = 0;
        bool Restarting = false;
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_DATA_EXECUTER_ACTOR;
    }

    TKqpDataExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database, const TMaybe<TString>& userToken,
        TKqpRequestCounters::TPtr counters)
        : TBase(std::move(request), database, userToken, counters, TWilsonKqp::DataExecuter, "DataExecuter")
    {
        YQL_ENSURE(Request.IsolationLevel != NKikimrKqp::ISOLATION_LEVEL_UNDEFINED);

        if (Request.AcquireLocksTxId || Request.ValidateLocks || Request.EraseLocks) {
            YQL_ENSURE(Request.IsolationLevel == NKikimrKqp::ISOLATION_LEVEL_SERIALIZABLE);
        }

        if (Request.Snapshot.IsValid()) {
            YQL_ENSURE(Request.IsolationLevel == NKikimrKqp::ISOLATION_LEVEL_SERIALIZABLE);
        }
    }

public:
    STATEFN(WaitResolveState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvKqpExecuter::TEvTableResolveStatus, HandleResolve);
                hFunc(TEvKqp::TEvAbortExecution, HandleAbortExecution);
                hFunc(TEvents::TEvWakeup, HandleTimeout);
                default:
                    UnexpectedEvent("WaitResolveState", ev->GetTypeRewrite());
            }
        } catch (const yexception& e) {
            InternalError(e.what());
        }
        ReportEventElapsedTime();
    }

    void HandleResolve(TEvKqpExecuter::TEvTableResolveStatus::TPtr& ev) {
        auto& reply = *ev->Get();

        auto resolveDuration = TInstant::Now() - StartResolveTime;
        Counters->TxProxyMon->TxPrepareResolveHgram->Collect(resolveDuration.MicroSeconds());

        KqpTableResolverId = {};
        if (Stats) {
            Stats->ExecuterCpuTime += reply.CpuTime;
            Stats->ResolveCpuTime = reply.CpuTime;
            Stats->ResolveWallTime = resolveDuration;
        }

        if (reply.Status != Ydb::StatusIds::SUCCESS) {
            Counters->TxProxyMon->ResolveKeySetWrongRequest->Inc();
            ReplyErrorAndDie(reply.Status, reply.Issues);
            return;
        }

        if (ExecuterTableResolveSpan) {
            ExecuterTableResolveSpan.End();
        }

        Execute();
    }

private:
    STATEFN(PrepareState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvDataShard::TEvProposeTransactionResult, HandlePrepare);
                hFunc(TEvDataShard::TEvProposeTransactionRestart, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionAttachResult, HandlePrepare);
                hFunc(TEvPrivate::TEvReattachToShard, HandleExecute);
                hFunc(TEvDqCompute::TEvState, HandlePrepare); // from CA
                hFunc(TEvDqCompute::TEvChannelData, HandleExecute); // from CA
                hFunc(TEvPipeCache::TEvDeliveryProblem, HandlePrepare);
                hFunc(TEvKqp::TEvAbortExecution, HandlePrepare);
                hFunc(TEvents::TEvWakeup, HandlePrepare);
                default: {
                    CancelProposal(0);
                    UnexpectedEvent("PrepareState", ev->GetTypeRewrite());
                }
            }
        } catch (const yexception& e) {
            CancelProposal(0);
            InternalError(e.what());
        }
        ReportEventElapsedTime();
    }

    void HandlePrepare(TEvDataShard::TEvProposeTransactionResult::TPtr& ev) {
        TEvDataShard::TEvProposeTransactionResult* res = ev->Get();
        const ui64 shardId = res->GetOrigin();
        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState, "Unexpected propose result from unknown tabletId " << shardId);

        LOG_D("Got propose result, shard: " << shardId << ", status: "
            << NKikimrTxDataShard::TEvProposeTransactionResult_EStatus_Name(res->GetStatus())
            << ", error: " << res->GetError());

        if (Stats) {
            Stats->AddDatashardPrepareStats(std::move(*res->Record.MutableTxStats()));
        }

        switch (res->GetStatus()) {
            case NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED: {
                if (!ShardPrepared(*shardState, res->Record)) {
                    return CancelProposal(shardId);
                }
                return CheckPrepareCompleted();
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::COMPLETE: {
                YQL_ENSURE(false);
            }
            default: {
                CancelProposal(shardId);
                return ShardError(res->Record);
            }
        }
    }

    void HandlePrepare(TEvDataShard::TEvProposeTransactionAttachResult::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui64 tabletId = record.GetTabletId();

        auto* shardState = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(shardState, "Unknown tablet " << tabletId);

        if (ev->Cookie != shardState->ReattachState.Cookie) {
            return;
        }

        switch (shardState->State) {
            case TShardState::EState::Preparing:
            case TShardState::EState::Prepared:
                break;
            case TShardState::EState::Initial:
            case TShardState::EState::Executing:
            case TShardState::EState::Finished:
                YQL_ENSURE(false, "Unexpected shard " << tabletId << " state " << ToString(shardState->State));
        }

        if (record.GetStatus() == NKikimrProto::OK) {
            // Transaction still exists at this shard
            LOG_D("Reattached to shard " << tabletId << ", state was: " << ToString(shardState->State));
            shardState->State = TShardState::EState::Prepared;
            shardState->ReattachState.Reattached();
            return CheckPrepareCompleted();
        }

        LOG_E("Shard " << tabletId << " transaction lost during reconnect: " << record.GetStatus());

        CancelProposal(tabletId);
        ReplyTxStateUnknown(tabletId);
    }

    void HandlePrepare(TEvDqCompute::TEvState::TPtr& ev) {
        if (ev->Get()->Record.GetState() == NDqProto::COMPUTE_STATE_FAILURE) {
            CancelProposal(0);
        }
        HandleExecute(ev);
    }

    void HandlePrepare(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        TEvPipeCache::TEvDeliveryProblem* msg = ev->Get();
        auto* shardState = ShardStates.FindPtr(msg->TabletId);
        YQL_ENSURE(shardState, "EvDeliveryProblem from unknown tablet " << msg->TabletId);

        bool wasRestarting = std::exchange(shardState->Restarting, false);

        // We can only be sure tx was not prepared if initial propose was not delivered
        bool notPrepared = msg->NotDelivered && (shardState->RestartCount == 0);

        switch (shardState->State) {
            case TShardState::EState::Preparing: {
                // Disconnected while waiting for initial propose response

                LOG_I("Shard " << msg->TabletId << " propose error, notDelivered: " << msg->NotDelivered
                    << ", notPrepared: " << notPrepared << ", wasRestart: " << wasRestarting);

                if (notPrepared) {
                    CancelProposal(msg->TabletId);
                    return ReplyUnavailable(TStringBuilder() << "Could not deliver program to shard " << msg->TabletId);
                }

                CancelProposal(0);

                if (wasRestarting) {
                    // We are waiting for propose and have a restarting flag, which means shard was
                    // persisting our tx. We did not receive a reply, so we cannot be sure if it
                    // succeeded or not, but we know that it could not apply any side effects, since
                    // we don't start transaction planning until prepare phase is complete.
                    return ReplyUnavailable(TStringBuilder() << "Could not prepare program on shard " << msg->TabletId);
                }

                return ReplyTxStateUnknown(msg->TabletId);
            }

            case TShardState::EState::Prepared: {
                // Disconnected while waiting for other shards to prepare

                if ((wasRestarting || shardState->ReattachState.Reattaching) &&
                    shardState->ReattachState.ShouldReattach(TlsActivationContext->Now()))
                {
                    LOG_N("Shard " << msg->TabletId << " delivery problem (already prepared, reattaching in "
                        << shardState->ReattachState.Delay << ")");

                    Schedule(shardState->ReattachState.Delay, new TEvPrivate::TEvReattachToShard(msg->TabletId));
                    ++shardState->RestartCount;
                    return;
                }

                LOG_N("Shard " << msg->TabletId << " delivery problem (already prepared)"
                    << (msg->NotDelivered ? ", last message not delivered" : ""));

                CancelProposal(0);
                return ReplyTxStateUnknown(msg->TabletId);
            }

            case TShardState::EState::Initial:
            case TShardState::EState::Executing:
            case TShardState::EState::Finished:
                YQL_ENSURE(false, "Unexpected shard " << msg->TabletId << " state " << ToString(shardState->State));
        }
    }

    void HandlePrepare(TEvKqp::TEvAbortExecution::TPtr& ev) {
        CancelProposal(0);
        TBase::HandleAbortExecution(ev);
    }

    void HandlePrepare(TEvents::TEvWakeup::TPtr& ev) {
        CancelProposal(0);
        TBase::HandleTimeout(ev);
    }

    void CancelProposal(ui64 exceptShardId) {
        for (auto& [shardId, state] : ShardStates) {
            if (shardId != exceptShardId &&
                (state.State == TShardState::EState::Preparing || state.State == TShardState::EState::Prepared))
            {
                state.State = TShardState::EState::Finished;

                YQL_ENSURE(!state.DatashardState->Follower);

                Send(MakePipePeNodeCacheID(/* allowFollowers */ false), new TEvPipeCache::TEvForward(
                    new TEvDataShard::TEvCancelTransactionProposal(TxId), shardId, /* subscribe */ false));
            }
        }
    }

    bool ShardPrepared(TShardState& state, const NKikimrTxDataShard::TEvProposeTransactionResult& result) {
        YQL_ENSURE(state.State == TShardState::EState::Preparing);
        state.State = TShardState::EState::Prepared;

        state.DatashardState->ShardMinStep = result.GetMinStep();
        state.DatashardState->ShardMaxStep = result.GetMaxStep();
        state.DatashardState->ReadSize += result.GetReadSize();

        ui64 coordinator = 0;
        if (result.DomainCoordinatorsSize()) {
            auto domainCoordinators = TCoordinators(TVector<ui64>(result.GetDomainCoordinators().begin(),
                                                                  result.GetDomainCoordinators().end()));
            coordinator = domainCoordinators.Select(TxId);
        }

        if (coordinator && !TxCoordinator) {
            TxCoordinator = coordinator;
        }

        if (!TxCoordinator || TxCoordinator != coordinator) {
            LOG_E("Handle TEvProposeTransactionResult: unable to select coordinator. Tx canceled, actorId: " << SelfId()
                << ", previously selected coordinator: " << TxCoordinator
                << ", coordinator selected at propose result: " << coordinator);

            Counters->TxProxyMon->TxResultAborted->Inc();
            ReplyErrorAndDie(Ydb::StatusIds::CANCELLED, MakeIssue(
                NKikimrIssues::TIssuesIds::TX_DECLINED_IMPLICIT_COORDINATOR, "Unable to choose coordinator."));
            return false;
        }

        LastPrepareReply = TInstant::Now();
        if (!FirstPrepareReply) {
            FirstPrepareReply = LastPrepareReply;
        }

        return true;
    }

    void ShardError(const NKikimrTxDataShard::TEvProposeTransactionResult& result) {
        if (result.ErrorSize() != 0) {
            TStringBuilder message;
            message << NKikimrTxDataShard::TEvProposeTransactionResult_EStatus_Name(result.GetStatus()) << ": ";
            for (const auto &err : result.GetError()) {
                message << "[" << NKikimrTxDataShard::TError_EKind_Name(err.GetKind()) << "] " << err.GetReason() << "; ";
            }
            LOG_E(message);
        }

        switch (result.GetStatus()) {
            case NKikimrTxDataShard::TEvProposeTransactionResult::OVERLOADED: {
                Counters->TxProxyMon->TxResultShardOverloaded->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OVERLOADED);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::OVERLOADED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::ABORTED: {
                Counters->TxProxyMon->TxResultAborted->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_ABORTED);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::ABORTED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::TRY_LATER: {
                Counters->TxProxyMon->TxResultShardTryLater->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::RESULT_UNAVAILABLE: {
                Counters->TxProxyMon->TxResultResultUnavailable->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_RESULT_UNAVAILABLE);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNDETERMINED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::CANCELLED: {
                Counters->TxProxyMon->TxResultCancelled->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_CANCELLED);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::CANCELLED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::BAD_REQUEST: {
                Counters->TxProxyMon->TxResultCancelled->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_BAD_REQUEST);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::BAD_REQUEST, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::EXEC_ERROR: {
                Counters->TxProxyMon->TxResultExecError->Inc();
                for (auto& er : result.GetError()) {
                    if (er.GetKind() == NKikimrTxDataShard::TError::PROGRAM_ERROR) {
                        auto issue = YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED);
                        issue.AddSubIssue(new TIssue(TStringBuilder() << "Data shard error: [PROGRAM_ERROR] " << er.GetReason()));
                        return ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED, issue);
                    }
                }
                auto issue = YqlIssue({}, TIssuesIds::DEFAULT_ERROR, "Error executing transaction (ExecError): Execution failed");
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::GENERIC_ERROR, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::ERROR: {
                Counters->TxProxyMon->TxResultError->Inc();
                for (auto& er : result.GetError()) {
                    switch (er.GetKind()) {
                        case NKikimrTxDataShard::TError::SCHEME_CHANGED:
                        case NKikimrTxDataShard::TError::SCHEME_ERROR:
                            return ReplyErrorAndDie(Ydb::StatusIds::SCHEME_ERROR, YqlIssue({},
                                TIssuesIds::KIKIMR_SCHEME_MISMATCH, er.GetReason()));

                        default:
                            break;
                    }
                }
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
            }
            default: {
                Counters->TxProxyMon->TxResultFatal->Inc();
                auto issue = YqlIssue({}, TIssuesIds::DEFAULT_ERROR, "Error executing transaction: transaction failed.");
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::GENERIC_ERROR, issue);
            }
        }
    }

    void CheckPrepareCompleted() {
        for (auto& [_, state] : ShardStates) {
            if (state.State != TShardState::EState::Prepared) {
                LOG_D("Not all shards are prepared, waiting...");
                return;
            }
        }

        Counters->TxProxyMon->TxPrepareSpreadHgram->Collect((LastPrepareReply - FirstPrepareReply).MilliSeconds());

        LOG_D("All shards prepared, become ExecuteState.");
        Become(&TKqpDataExecuter::ExecuteState);
        if (ExecuterStateSpan) {
            ExecuterStateSpan.End();
            ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::DataExecuterExecuteState, ExecuterSpan.GetTraceId(), "ExecuteState", NWilson::EFlags::AUTO_END);
        }

        ExecutePlanned();
    }

    void ExecutePlanned() {
        YQL_ENSURE(TxCoordinator);

        auto ev = MakeHolder<TEvTxProxy::TEvProposeTransaction>();
        ev->Record.SetCoordinatorID(TxCoordinator);

        auto& transaction = *ev->Record.MutableTransaction();
        auto& affectedSet = *transaction.MutableAffectedSet();
        affectedSet.Reserve(static_cast<int>(ShardStates.size()));

        ui64 aggrMinStep = 0;
        ui64 aggrMaxStep = Max<ui64>();
        ui64 totalReadSize = 0;

        for (auto& [shardId, state] : ShardStates) {
            YQL_ENSURE(state.State == TShardState::EState::Prepared);
            state.State = TShardState::EState::Executing;

            YQL_ENSURE(state.DatashardState.Defined());
            YQL_ENSURE(!state.DatashardState->Follower);

            aggrMinStep = Max(aggrMinStep, state.DatashardState->ShardMinStep);
            aggrMaxStep = Min(aggrMaxStep, state.DatashardState->ShardMaxStep);
            totalReadSize += state.DatashardState->ReadSize;

            auto& item = *affectedSet.Add();
            item.SetTabletId(shardId);

            ui32 affectedFlags = 0;
            if (state.DatashardState->ShardReadLocks) {
                affectedFlags |= NFlatTxCoordinator::TTransactionProposal::TAffectedEntry::AffectedRead;
            }

            for (auto taskId : state.TaskIds) {
                auto& task = TasksGraph.GetTask(taskId);
                auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);

                if (HasReads(stageInfo)) {
                    affectedFlags |= NFlatTxCoordinator::TTransactionProposal::TAffectedEntry::AffectedRead;
                }
                if (HasWrites(stageInfo)) {
                    affectedFlags |= NFlatTxCoordinator::TTransactionProposal::TAffectedEntry::AffectedWrite;
                }
            }

            item.SetFlags(affectedFlags);
        }

        ui64 sizeLimit = RequestControls.PerRequestDataSizeLimit;
        if (Request.TotalReadSizeLimitBytes > 0) {
            sizeLimit = sizeLimit
                ? std::min(sizeLimit, Request.TotalReadSizeLimitBytes)
                : Request.TotalReadSizeLimitBytes;
        }

        if (totalReadSize > sizeLimit) {
            auto msg = TStringBuilder() << "Transaction total read size " << totalReadSize << " exceeded limit " << sizeLimit;
            LOG_N(msg);
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                YqlIssue({}, NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED, msg));
            return;
        }

        transaction.SetTxId(TxId);
        transaction.SetMinStep(aggrMinStep);
        transaction.SetMaxStep(aggrMaxStep);

        LOG_T("Execute planned transaction, coordinator: " << TxCoordinator);
        Send(MakePipePeNodeCacheID(false), new TEvPipeCache::TEvForward(ev.Release(), TxCoordinator, /* subscribe */ true));
    }

private:
    STATEFN(ExecuteState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvDataShard::TEvProposeTransactionResult, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionRestart, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionAttachResult, HandleExecute);
                hFunc(TEvPrivate::TEvReattachToShard, HandleExecute);
                hFunc(TEvPipeCache::TEvDeliveryProblem, HandleExecute);
                hFunc(TEvTxProxy::TEvProposeTransactionStatus, HandleExecute);
                hFunc(TEvDqCompute::TEvState, HandleExecute);
                hFunc(TEvDqCompute::TEvChannelData, HandleExecute);
                hFunc(TEvKqp::TEvAbortExecution, HandleAbortExecution);
                hFunc(TEvents::TEvWakeup, HandleTimeout);
                default:
                    UnexpectedEvent("ExecuteState", ev->GetTypeRewrite());
            }
        } catch (const yexception& e) {
            InternalError(e.what());
        }
        ReportEventElapsedTime();
    }

    void HandleExecute(TEvDataShard::TEvProposeTransactionResult::TPtr& ev) {
        TEvDataShard::TEvProposeTransactionResult* res = ev->Get();
        const ui64 shardId = res->GetOrigin();
        LastShard = shardId;

        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState);

        LOG_D("Got propose result, shard: " << shardId << ", status: "
            << NKikimrTxDataShard::TEvProposeTransactionResult_EStatus_Name(res->GetStatus())
            << ", error: " << res->GetError());

        if (Stats) {
            Stats->AddDatashardStats(std::move(*res->Record.MutableComputeActorStats()),
                std::move(*res->Record.MutableTxStats()));
        }

        switch (res->GetStatus()) {
            case NKikimrTxDataShard::TEvProposeTransactionResult::COMPLETE: {
                YQL_ENSURE(shardState->State == TShardState::EState::Executing);
                shardState->State = TShardState::EState::Finished;

                Counters->TxProxyMon->ResultsReceivedCount->Inc();
                Counters->TxProxyMon->ResultsReceivedSize->Add(res->GetTxResult().size());

                for (auto& lock : res->Record.GetTxLocks()) {
                    LOG_D("Shard " << shardId << " completed, store lock " << lock.ShortDebugString());
                    Locks.emplace_back(std::move(lock));
                }

                Counters->TxProxyMon->TxResultComplete->Inc();

                CheckExecutionComplete();
                return;
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::LOCKS_BROKEN: {
                LOG_D("Broken locks: " << res->Record.DebugString());

                Counters->TxProxyMon->TxResultAborted->Inc(); // TODO: dedicated counter?

                TMaybe<TString> tableName;
                if (!res->Record.GetTxLocks().empty()) {
                    auto& lock = res->Record.GetTxLocks(0);
                    auto tableId = TTableId(lock.GetSchemeShard(), lock.GetPathId());
                    auto it = FindIf(TableKeys.Get(), [tableId](const auto& x){ return x.first.HasSamePath(tableId); });
                    if (it != TableKeys.Get().end()) {
                        tableName = it->second.Path;
                    }
                }

                auto message = TStringBuilder() << "Transaction locks invalidated.";
                if (tableName) {
                    message << " Table: " << *tableName;
                }

                return ReplyErrorAndDie(Ydb::StatusIds::ABORTED,
                    YqlIssue({}, TIssuesIds::KIKIMR_LOCKS_INVALIDATED, message));
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED: {
                YQL_ENSURE(false);
            }
            default: {
                return ShardError(res->Record);
            }
        }
    }

    void HandleExecute(TEvDataShard::TEvProposeTransactionRestart::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui64 shardId = record.GetTabletId();

        auto* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState, "restart tx event from unknown tabletId: " << shardId << ", tx: " << TxId);

        LOG_D("Got transaction restart event from tabletId: " << shardId << ", state: " << ToString(shardState->State)
            << ", txPlanned: " << TxPlanned);

        switch (shardState->State) {
            case TShardState::EState::Preparing:
            case TShardState::EState::Prepared:
            case TShardState::EState::Executing: {
                shardState->Restarting = true;
                return;
            }
            case TShardState::EState::Finished: {
                return;
            }
            case TShardState::EState::Initial: {
                YQL_ENSURE(false);
            }
        }
    }

    void HandleExecute(TEvDataShard::TEvProposeTransactionAttachResult::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui64 tabletId = record.GetTabletId();

        auto* shardState = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(shardState, "Unknown tablet " << tabletId);

        if (ev->Cookie != shardState->ReattachState.Cookie) {
            return;
        }

        switch (shardState->State) {
            case TShardState::EState::Executing:
                break;
            case TShardState::EState::Initial:
            case TShardState::EState::Preparing:
            case TShardState::EState::Prepared:
            case TShardState::EState::Finished:
                return;
        }

        if (record.GetStatus() == NKikimrProto::OK) {
            // Transaction still exists at this shard
            LOG_N("Reattached to shard " << tabletId << ", state was: " << ToString(shardState->State));
            shardState->ReattachState.Reattached();

            CheckExecutionComplete();
            return;
        }

        LOG_E("Shard " << tabletId << " transaction lost during reconnect: " << record.GetStatus());

        ReplyTxStateUnknown(tabletId);
    }

    void HandleExecute(TEvPrivate::TEvReattachToShard::TPtr& ev) {
        const ui64 tabletId = ev->Get()->TabletId;
        auto* shardState = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(shardState);

        LOG_I("Reattach to shard " << tabletId);

        Send(MakePipePeNodeCacheID(UseFollowers), new TEvPipeCache::TEvForward(
            new TEvDataShard::TEvProposeTransactionAttach(tabletId, TxId),
            tabletId, /* subscribe */ true), 0, ++shardState->ReattachState.Cookie);
    }

    void HandleExecute(TEvTxProxy::TEvProposeTransactionStatus::TPtr &ev) {
        TEvTxProxy::TEvProposeTransactionStatus* res = ev->Get();
        LOG_D("Got transaction status, status: " << res->GetStatus());

        switch (res->GetStatus()) {
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusAccepted:
                Counters->TxProxyMon->ClientTxStatusAccepted->Inc();
                break;
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusProcessed:
                Counters->TxProxyMon->ClientTxStatusProcessed->Inc();
                break;
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusConfirmed:
                Counters->TxProxyMon->ClientTxStatusConfirmed->Inc();
                break;

            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusPlanned:
                Counters->TxProxyMon->ClientTxStatusPlanned->Inc();
                TxPlanned = true;
                break;

            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusOutdated:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusDeclined:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusDeclinedNoSpace:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusRestarting:
                Counters->TxProxyMon->ClientTxStatusCoordinatorDeclined->Inc();
                CancelProposal(0);
                ReplyUnavailable(TStringBuilder() << "Failed to plan transaction, status: " << res->GetStatus());
                break;

            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusUnknown:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusAborted:
                Counters->TxProxyMon->ClientTxStatusCoordinatorDeclined->Inc();
                InternalError(TStringBuilder() << "Unexpected TEvProposeTransactionStatus status: " << res->GetStatus());
                break;
        }
    }

    void HandleExecute(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        TEvPipeCache::TEvDeliveryProblem* msg = ev->Get();

        LOG_D("DeliveryProblem to shard " << msg->TabletId << ", notDelivered: " << msg->NotDelivered
            << ", txPlanned: " << TxPlanned << ", coordinator: " << TxCoordinator);

        if (msg->TabletId == TxCoordinator) {
            if (msg->NotDelivered) {
                LOG_E("Not delivered to coordinator " << msg->TabletId << ", abort execution");
                CancelProposal(0);
                return ReplyUnavailable("Delivery problem: could not plan transaction.");
            }

            if (TxPlanned) {
                // We lost pipe to coordinator, but we already know tx is planned
                return;
            }

            LOG_E("Delivery problem to coordinator " << msg->TabletId << ", abort execution");
            return ReplyTxStateUnknown(msg->TabletId);
        }

        auto* shardState = ShardStates.FindPtr(msg->TabletId);
        YQL_ENSURE(shardState, "EvDeliveryProblem from unknown shard " << msg->TabletId);

        bool wasRestarting = std::exchange(shardState->Restarting, false);

        switch (shardState->State) {
            case TShardState::EState::Prepared: // is it correct?
                LOG_E("DeliveryProblem to shard " << msg->TabletId << ", notDelivered: " << msg->NotDelivered
                    << ", txPlanned: " << TxPlanned << ", coordinator: " << TxCoordinator);
                Y_VERIFY_DEBUG(false);
                // Proceed with query processing
                [[fallthrough]];
            case TShardState::EState::Executing: {
                if ((wasRestarting || shardState->ReattachState.Reattaching) &&
                     shardState->ReattachState.ShouldReattach(TlsActivationContext->Now()))
                {
                    LOG_N("Shard " << msg->TabletId << " lost pipe while waiting for reply (reattaching in "
                        << shardState->ReattachState.Delay << ")");

                    Schedule(shardState->ReattachState.Delay, new TEvPrivate::TEvReattachToShard(msg->TabletId));
                    ++shardState->RestartCount;
                    return;
                }

                LOG_N("Shard " << msg->TabletId << " lost pipe while waiting for reply"
                    << (msg->NotDelivered ? " (last message not delivered)" : ""));

                return ReplyTxStateUnknown(msg->TabletId);
            }

            case TShardState::EState::Finished: {
                return;
            }

            case TShardState::EState::Initial:
            case TShardState::EState::Preparing:
                YQL_ENSURE(false, "Unexpected shard " << msg->TabletId << " state " << ToString(shardState->State));
        }
    }

    void HandleExecute(TEvDqCompute::TEvState::TPtr& ev) {
        TActorId computeActor = ev->Sender;
        auto& state = ev->Get()->Record;
        ui64 taskId = state.GetTaskId();

        LOG_D("Got execution state from compute actor: " << computeActor
            << ", task: " << taskId
            << ", state: " << NDqProto::EComputeState_Name((NDqProto::EComputeState) state.GetState()));

        switch (state.GetState()) {
            case NDqProto::COMPUTE_STATE_UNKNOWN: {
                YQL_ENSURE(false, "unexpected state from " << computeActor << ", task: " << taskId);
            }

            case NDqProto::COMPUTE_STATE_FAILURE: {
                ReplyErrorAndDie(DqStatusToYdbStatus(state.GetStatusCode()), state.MutableIssues());
                return;
            }

            case NDqProto::COMPUTE_STATE_EXECUTING: {
                YQL_ENSURE(PendingComputeActors.contains(computeActor));
                YQL_ENSURE(TasksGraph.GetTask(taskId).ComputeActorId == computeActor);
                break;
            }

            case NDqProto::COMPUTE_STATE_FINISHED: {
                if (Stats) {
                    Stats->AddComputeActorStats(computeActor.NodeId(), std::move(*state.MutableStats()));
                }

                if (PendingComputeActors.erase(computeActor) == 0) {
                    LOG_W("Got execution state from unknown compute actor: " << computeActor << ", task: " << taskId);
                }
            }
        }

        CheckExecutionComplete();
    }

    void HandleExecute(TEvDqCompute::TEvChannelData::TPtr& ev) {
        auto& record = ev->Get()->Record;
        auto& channelData = record.GetChannelData();

        auto& channel = TasksGraph.GetChannel(channelData.GetChannelId());
        YQL_ENSURE(channel.DstTask == 0);
        auto shardId = TasksGraph.GetTask(channel.SrcTask).Meta.ShardId;

        if (Stats) {
            Stats->ResultBytes += channelData.GetData().GetRaw().size();
            Stats->ResultRows += channelData.GetData().GetRows();
        }

        LOG_T("Got result, channelId: " << channel.Id << ", shardId: " << shardId
            << ", inputIndex: " << channel.DstInputIndex << ", from: " << ev->Sender
            << ", finished: " << channelData.GetFinished());

        YQL_ENSURE(channel.DstInputIndex < Results.size());
        if (channelData.GetData().GetRows()) {
            Results[channel.DstInputIndex].Data.emplace_back(std::move(*record.MutableChannelData()->MutableData()));
        }

        {
            LOG_T("Send ack to channelId: " << channel.Id << ", seqNo: " << record.GetSeqNo() << ", to: " << ev->Sender);

            auto ackEv = MakeHolder<TEvDqCompute::TEvChannelDataAck>();
            ackEv->Record.SetSeqNo(record.GetSeqNo());
            ackEv->Record.SetChannelId(channel.Id);
            ackEv->Record.SetFreeSpace(50_MB);
            Send(ev->Sender, ackEv.Release(), /* TODO: undelivery */ 0, /* cookie */ channel.Id);
        }
    }

    void CheckExecutionComplete() {
        ui32 notFinished = 0;
        for (auto& x : ShardStates) {
            if (x.second.State != TShardState::EState::Finished) {
                notFinished++;
                LOG_D("Datashard " << x.first << " not finished yet: " << ToString(x.second.State));
            }
        }
        if (notFinished == 0 && PendingComputeActors.empty()) {
            Finalize();
            return;
        }

        if (IsDebugLogEnabled()) {
            auto sb = TStringBuilder() << "Waiting for " << PendingComputeActors.size() << " compute actor(s) and "
                << notFinished << " datashard(s): ";
            for (auto shardId : PendingComputeActors) {
                sb << "CA " << shardId.first << ", ";
            }
            for (auto& [shardId, shardState] : ShardStates) {
                if (shardState.State != TShardState::EState::Finished) {
                    sb << "DS " << shardId << " (" << ToString(shardState.State) << "), ";
                }
            }
            LOG_D(sb);
        }
    }

private:
    void FillGeneralReadInfo(TTaskMeta& taskMeta, ui64 itemsLimit, bool reverse) {
        if (taskMeta.Reads && !taskMeta.Reads.GetRef().empty()) {
            // Validate parameters
            YQL_ENSURE(taskMeta.ReadInfo.ItemsLimit == itemsLimit);
            YQL_ENSURE(taskMeta.ReadInfo.Reverse == reverse);
            return;
        }

        taskMeta.ReadInfo.ItemsLimit = itemsLimit;
        taskMeta.ReadInfo.Reverse = reverse;
    };

    void BuildDatashardTasks(TStageInfo& stageInfo, const NMiniKQL::THolderFactory& holderFactory,
        const NMiniKQL::TTypeEnvironment& typeEnv)
    {
        THashMap<ui64, ui64> shardTasks; // shardId -> taskId

        auto getShardTask = [&](ui64 shardId) -> TTask& {
            auto it  = shardTasks.find(shardId);
            if (it != shardTasks.end()) {
                return TasksGraph.GetTask(it->second);
            }
            auto& task = TasksGraph.AddTask(stageInfo);
            task.Meta.ShardId = shardId;
            shardTasks.emplace(shardId, task.Id);
            return task;
        };

        auto& stage = GetStage(stageInfo);

        const auto& table = TableKeys.GetTable(stageInfo.Meta.TableId);
        const auto& keyTypes = table.KeyColumnTypes;;

        for (auto& op : stage.GetTableOps()) {
            Y_VERIFY_DEBUG(stageInfo.Meta.TablePath == op.GetTable().GetPath());

            auto columns = BuildKqpColumns(op, table);
            THashMap<ui64, TShardInfo> partitions;

            switch (op.GetTypeCase()) {
                case NKqpProto::TKqpPhyTableOperation::kReadRanges:
                case NKqpProto::TKqpPhyTableOperation::kReadRange:
                case NKqpProto::TKqpPhyTableOperation::kLookup: {
                    bool reverse = false;
                    ui64 itemsLimit = 0;
                    TString itemsLimitParamName;
                    NDqProto::TData itemsLimitBytes;
                    NKikimr::NMiniKQL::TType* itemsLimitType = nullptr;

                    if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kReadRanges) {
                        partitions = PrunePartitions(TableKeys, op.GetReadRanges(), stageInfo, holderFactory, typeEnv);
                        ExtractItemsLimit(stageInfo, op.GetReadRanges().GetItemsLimit(), holderFactory, typeEnv,
                            itemsLimit, itemsLimitParamName, itemsLimitBytes, itemsLimitType);
                        reverse = op.GetReadRanges().GetReverse();
                    } else if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kReadRange) {
                        partitions = PrunePartitions(TableKeys, op.GetReadRange(), stageInfo, holderFactory, typeEnv);
                        ExtractItemsLimit(stageInfo, op.GetReadRange().GetItemsLimit(), holderFactory, typeEnv,
                            itemsLimit, itemsLimitParamName, itemsLimitBytes, itemsLimitType);
                        reverse = op.GetReadRange().GetReverse();
                    } else if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kLookup) {
                        partitions = PrunePartitions(TableKeys, op.GetLookup(), stageInfo, holderFactory, typeEnv);
                    }

                    for (auto& [shardId, shardInfo] : partitions) {
                        YQL_ENSURE(!shardInfo.KeyWriteRanges);

                        auto& task = getShardTask(shardId);
                        for (auto& [name, value] : shardInfo.Params) {
                            task.Meta.Params.emplace(name, std::move(value));
                            auto typeIterator = shardInfo.ParamTypes.find(name);
                            YQL_ENSURE(typeIterator != shardInfo.ParamTypes.end());
                            auto retType = task.Meta.ParamTypes.emplace(name, typeIterator->second);
                            YQL_ENSURE(retType.second);
                        }

                        FillGeneralReadInfo(task.Meta, itemsLimit, reverse);

                        TTaskMeta::TShardReadInfo readInfo;
                        readInfo.Ranges = std::move(*shardInfo.KeyReadRanges);
                        readInfo.Columns = columns;

                        if (itemsLimitParamName) {
                            task.Meta.Params.emplace(itemsLimitParamName, itemsLimitBytes);
                            task.Meta.ParamTypes.emplace(itemsLimitParamName, itemsLimitType);
                        }

                        if (!task.Meta.Reads) {
                            task.Meta.Reads.ConstructInPlace();
                        }
                        task.Meta.Reads->emplace_back(std::move(readInfo));
                    }

                    break;
                }

                case NKqpProto::TKqpPhyTableOperation::kUpsertRows:
                case NKqpProto::TKqpPhyTableOperation::kDeleteRows: {
                    YQL_ENSURE(stage.InputsSize() <= 1, "Effect stage with multiple inputs: " << stage.GetProgramAst());

                    if (stage.InputsSize() == 1 && stage.GetInputs(0).GetTypeCase() == NKqpProto::TKqpPhyConnection::kMapShard) {
                        const auto& inputStageInfo = TasksGraph.GetStageInfo(
                            TStageId(stageInfo.Id.TxId, stage.GetInputs(0).GetStageIndex()));

                        for (ui64 inputTaskId : inputStageInfo.Tasks) {
                            auto& task = getShardTask(TasksGraph.GetTask(inputTaskId).Meta.ShardId);

                            auto& inputTask = TasksGraph.GetTask(inputTaskId);
                            YQL_ENSURE(inputTask.Meta.Reads, "" << inputTask.Meta.ToString(keyTypes, *AppData()->TypeRegistry));
                            for (auto& read : *inputTask.Meta.Reads) {
                                if (!task.Meta.Writes) {
                                    task.Meta.Writes.ConstructInPlace();
                                    task.Meta.Writes->Ranges = read.Ranges;
                                } else {
                                    task.Meta.Writes->Ranges.MergeWritePoints(TShardKeyRanges(read.Ranges), keyTypes);
                                }

                                if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kDeleteRows) {
                                    task.Meta.Writes->AddEraseOp();
                                } else {
                                    task.Meta.Writes->AddUpdateOp();
                                }

                            }

                            ShardsWithEffects.insert(task.Meta.ShardId);
                        }
                    } else {
                        auto result = (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kUpsertRows)
                            ? PruneEffectPartitions(TableKeys, op.GetUpsertRows(), stageInfo, holderFactory, typeEnv)
                            : PruneEffectPartitions(TableKeys, op.GetDeleteRows(), stageInfo, holderFactory, typeEnv);

                        for (auto& [shardId, shardInfo] : result) {
                            YQL_ENSURE(!shardInfo.KeyReadRanges);
                            YQL_ENSURE(shardInfo.KeyWriteRanges);

                            auto& task = getShardTask(shardId);
                            task.Meta.Params = std::move(shardInfo.Params);

                            if (!task.Meta.Writes) {
                                task.Meta.Writes.ConstructInPlace();
                                task.Meta.Writes->Ranges = std::move(*shardInfo.KeyWriteRanges);
                            } else {
                                task.Meta.Writes->Ranges.MergeWritePoints(std::move(*shardInfo.KeyWriteRanges), keyTypes);
                            }

                            if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kDeleteRows) {
                                task.Meta.Writes->AddEraseOp();
                            } else {
                                task.Meta.Writes->AddUpdateOp();
                            }

                            for (const auto& [name, info] : shardInfo.ColumnWrites) {
                                auto& column = table.Columns.at(name);

                                auto& taskColumnWrite = task.Meta.Writes->ColumnWrites[column.Id];
                                taskColumnWrite.Column.Id = column.Id;
                                taskColumnWrite.Column.Type = column.Type;
                                taskColumnWrite.Column.Name = name;
                                taskColumnWrite.MaxValueSizeBytes = std::max(taskColumnWrite.MaxValueSizeBytes,
                                    info.MaxValueSizeBytes);
                            }

                            ShardsWithEffects.insert(shardId);
                        }
                    }
                    break;
                }

                default: {
                    YQL_ENSURE(false, "Unexpected table operation: " << (ui32) op.GetTypeCase() << Endl
                        << this->DebugString());
                }
            }
        }

        LOG_D("Stage " << stageInfo.Id << " will be executed on " << shardTasks.size() << " shards.");

        for (auto& shardTask : shardTasks) {
            auto& task = TasksGraph.GetTask(shardTask.second);
            LOG_D("Stage " << stageInfo.Id << " create datashard task: " << shardTask.second
                << ", shard: " << shardTask.first
                << ", meta: " << task.Meta.ToString(keyTypes, *AppData()->TypeRegistry));
        }
    }

    void BuildComputeTasks(TStageInfo& stageInfo) {
        auto& stage = GetStage(stageInfo);

        ui32 partitionsCount = 1;
        for (ui32 inputIndex = 0; inputIndex < stage.InputsSize(); ++inputIndex) {
            const auto& input = stage.GetInputs(inputIndex);

            // Current assumptions:
            // 1. `Broadcast` can not be the 1st stage input unless it's a single input
            // 2. All stage's inputs, except 1st one, must be a `Broadcast` or `UnionAll`
            if (inputIndex == 0) {
                if (stage.InputsSize() > 1) {
                    YQL_ENSURE(input.GetTypeCase() != NKqpProto::TKqpPhyConnection::kBroadcast);
                }
            } else {
                switch (input.GetTypeCase()) {
                    case NKqpProto::TKqpPhyConnection::kBroadcast:
                    case NKqpProto::TKqpPhyConnection::kHashShuffle:
                    case NKqpProto::TKqpPhyConnection::kUnionAll:
                    case NKqpProto::TKqpPhyConnection::kMerge:
                        break;
                    default:
                        YQL_ENSURE(false, "Unexpected connection type: " << (ui32)input.GetTypeCase() << Endl
                            << this->DebugString());
                }
            }

            auto& originStageInfo = TasksGraph.GetStageInfo(TStageId(stageInfo.Id.TxId, input.GetStageIndex()));

            switch (input.GetTypeCase()) {
                case NKqpProto::TKqpPhyConnection::kHashShuffle: {
                    partitionsCount = std::max(partitionsCount, (ui32)originStageInfo.Tasks.size() / 2);
                    partitionsCount = std::min(partitionsCount, 24u);
                    break;
                }

                case NKqpProto::TKqpPhyConnection::kMap: {
                    partitionsCount = originStageInfo.Tasks.size();
                    break;
                }

                default: {
                    break;
                }
            }
        }

        for (ui32 i = 0; i < partitionsCount; ++i) {
            auto& task = TasksGraph.AddTask(stageInfo);
            LOG_D("Stage " << stageInfo.Id << " create compute task: " << task.Id);
        }
    }

    void ExecuteDatashardTransaction(ui64 shardId, NKikimrTxDataShard::TKqpTransaction& kqpTx,
        const TMaybe<ui64> lockTxId)
    {
        TShardState shardState;
        shardState.State = ImmediateTx ? TShardState::EState::Executing : TShardState::EState::Preparing;
        shardState.DatashardState.ConstructInPlace();
        shardState.DatashardState->Follower = UseFollowers;

        if (Deadline) {
            TDuration timeout = *Deadline - TAppData::TimeProvider->Now();
            kqpTx.MutableRuntimeSettings()->SetTimeoutMs(timeout.MilliSeconds());
        }
        kqpTx.MutableRuntimeSettings()->SetExecType(NDqProto::TComputeRuntimeSettings::DATA);
        kqpTx.MutableRuntimeSettings()->SetStatsMode(GetDqStatsModeShard(Request.StatsMode));

        kqpTx.MutableRuntimeSettings()->SetUseLLVM(false);
        kqpTx.MutableRuntimeSettings()->SetUseSpilling(false);

        NKikimrTxDataShard::TDataTransaction dataTransaction;
        dataTransaction.MutableKqpTransaction()->Swap(&kqpTx);
        dataTransaction.SetImmediate(ImmediateTx);
        dataTransaction.SetReadOnly(ReadOnlyTx);
        if (CancelAt) {
            dataTransaction.SetCancelAfterMs((*CancelAt - AppData()->TimeProvider->Now()).MilliSeconds());
        }
        if (Request.PerShardKeysSizeLimitBytes) {
            YQL_ENSURE(!ReadOnlyTx);
            dataTransaction.SetPerShardKeysSizeLimitBytes(Request.PerShardKeysSizeLimitBytes);
        }

        if (lockTxId) {
            dataTransaction.SetLockTxId(*lockTxId);
            dataTransaction.SetLockNodeId(SelfId().NodeId());
        }

        for (auto& task : dataTransaction.GetKqpTransaction().GetTasks()) {
            shardState.TaskIds.insert(task.GetId());
        }

        auto locksCount = dataTransaction.GetKqpTransaction().GetLocks().LocksSize();
        shardState.DatashardState->ShardReadLocks = locksCount > 0;

        LOG_D("Executing KQP transaction on shard: " << shardId
            << ", tasks: [" << JoinStrings(shardState.TaskIds.begin(), shardState.TaskIds.end(), ",") << "]"
            << ", lockTxId: " << lockTxId
            << ", locks: " << dataTransaction.GetKqpTransaction().GetLocks().ShortDebugString());

        TEvDataShard::TEvProposeTransaction* ev;
        if (Snapshot.IsValid() && ReadOnlyTx) {
            ev = new TEvDataShard::TEvProposeTransaction(
                NKikimrTxDataShard::TX_KIND_DATA,
                SelfId(),
                TxId,
                dataTransaction.SerializeAsString(),
                Snapshot.Step,
                Snapshot.TxId,
                ImmediateTx ? NTxDataShard::TTxFlags::Immediate : 0);
        } else {
            ev = new TEvDataShard::TEvProposeTransaction(
                NKikimrTxDataShard::TX_KIND_DATA,
                SelfId(),
                TxId,
                dataTransaction.SerializeAsString(),
                ImmediateTx ? NTxDataShard::TTxFlags::Immediate : 0);
        }

        auto traceId = ExecuterSpan.GetTraceId();

        LOG_D("ExecuteDatashardTransaction traceId.verbosity: " << std::to_string(traceId.GetVerbosity()));

        Send(MakePipePeNodeCacheID(UseFollowers), new TEvPipeCache::TEvForward(ev, shardId, true), 0, 0, std::move(traceId));

        auto result = ShardStates.emplace(shardId, std::move(shardState));
        YQL_ENSURE(result.second);
    }

    void ExecuteDataComputeTask(NDqProto::TDqTask&& taskDesc) {
        auto taskId = taskDesc.GetId();
        auto& task = TasksGraph.GetTask(taskId);

        TComputeRuntimeSettings settings;
        if (Deadline) {
            settings.Timeout = *Deadline - TAppData::TimeProvider->Now();
        }
        //settings.ExtraMemoryAllocationPool = NRm::EKqpMemoryPool::DataQuery;
        settings.ExtraMemoryAllocationPool = NRm::EKqpMemoryPool::Unspecified;
        settings.FailOnUndelivery = true;
        settings.StatsMode = GetDqStatsMode(Request.StatsMode);
        settings.UseLLVM = false;
        settings.UseSpilling = false;

        TComputeMemoryLimits limits;
        limits.ScanBufferSize = 50_MB; // for system views only
        limits.ChannelBufferSize = 50_MB;
        limits.MkqlLightProgramMemoryLimit = Request.MkqlMemoryLimit > 0 ? std::min(500_MB, Request.MkqlMemoryLimit) : 500_MB;
        limits.MkqlHeavyProgramMemoryLimit = Request.MkqlMemoryLimit > 0 ? std::min(2_GB, Request.MkqlMemoryLimit) : 2_GB;
        limits.AllocateMemoryFn = [TxId = TxId](auto /* txId */, ui64 taskId, ui64 memory) {
            LOG_E("Data query task cannot allocate additional memory during executing."
                      << " Task: " << taskId << ", memory: " << memory);
            return false;
        };

        auto computeActor = CreateKqpComputeActor(SelfId(), TxId, std::move(taskDesc), nullptr, nullptr, settings, limits, ExecuterSpan.GetTraceId());
        auto computeActorId = Register(computeActor);
        task.ComputeActorId = computeActorId;

        LOG_D("Executing task: " << taskId << " on compute actor: " << task.ComputeActorId);

        auto result = PendingComputeActors.emplace(task.ComputeActorId, TProgressStat());
        YQL_ENSURE(result.second);
    }

    void Execute() {
        NWilson::TSpan prepareTasksSpan(TWilsonKqp::DataExecuterPrepateTasks, ExecuterStateSpan.GetTraceId(), "PrepateTasks", NWilson::EFlags::AUTO_END);
        LWTRACK(KqpDataExecuterStartExecute, ResponseEv->Orbit, TxId);
        RequestControls.Reqister(TlsActivationContext->AsActorContext());

        ReadOnlyTx = true;

        auto& funcRegistry = *AppData()->FunctionRegistry;
        NMiniKQL::TScopedAlloc alloc(TAlignedPagePoolCounters(), funcRegistry.SupportsSizedAllocators());
        NMiniKQL::TTypeEnvironment typeEnv(alloc);

        NMiniKQL::TMemoryUsageInfo memInfo("PrepareTasks");
        NMiniKQL::THolderFactory holderFactory(alloc.Ref(), memInfo, &funcRegistry);

        for (ui32 txIdx = 0; txIdx < Request.Transactions.size(); ++txIdx) {
            auto& tx = Request.Transactions[txIdx];

            for (ui32 stageIdx = 0; stageIdx < tx.Body->StagesSize(); ++stageIdx) {
                auto& stage = tx.Body->GetStages(stageIdx);
                auto& stageInfo = TasksGraph.GetStageInfo(TStageId(txIdx, stageIdx));

                if (stageInfo.Meta.ShardKind == NSchemeCache::TSchemeCacheRequest::KindAsyncIndexTable) {
                    TMaybe<TString> error;

                    if (stageInfo.Meta.ShardKey->RowOperation != TKeyDesc::ERowOperation::Read) {
                        error = TStringBuilder() << "Non-read operations can't be performed on async index table"
                            << ": " << stageInfo.Meta.ShardKey->TableId;
                    } else if (Request.IsolationLevel != NKikimrKqp::ISOLATION_LEVEL_READ_STALE) {
                        error = TStringBuilder() << "Read operation can be performed on async index table"
                            << ": " << stageInfo.Meta.ShardKey->TableId << " only with StaleRO isolation level";
                    }

                    if (error) {
                        LOG_E(*error);
                        ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                            YqlIssue({}, NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED, *error));
                        return;
                    }
                }

                LOG_D("Stage " << stageInfo.Id << " AST: " << stage.GetProgramAst());

                ReadOnlyTx &= !stage.GetIsEffectsStage();

                if (stageInfo.Meta.ShardOperations.empty()) {
                    BuildComputeTasks(stageInfo);
                } else if (stageInfo.Meta.IsSysView()) {
                    BuildSysViewScanTasks(stageInfo, holderFactory, typeEnv);
                } else {
                    BuildDatashardTasks(stageInfo, holderFactory, typeEnv);
                }

                BuildKqpStageChannels(TasksGraph, TableKeys, stageInfo, TxId, /* enableSpilling */ false);
            }

            BuildKqpExecuterResults(*tx.Body, Results);
            BuildKqpTaskGraphResultChannels(TasksGraph, *tx.Body, txIdx);
        }

        TIssue validateIssue;
        if (!ValidateTasks(TasksGraph, EExecType::Data, /* enableSpilling */ false, validateIssue)) {
            ReplyErrorAndDie(Ydb::StatusIds::INTERNAL_ERROR, validateIssue);
            return;
        }

        THashMap<ui64, TVector<NDqProto::TDqTask>> datashardTasks;  // shardId -> [task]
        TVector<NDqProto::TDqTask> computeTasks;

        for (auto& task : TasksGraph.GetTasks()) {
            auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);
            auto& stage = GetStage(stageInfo);

            NDqProto::TDqTask taskDesc;
            taskDesc.SetId(task.Id);
            taskDesc.SetStageId(stageInfo.Id.StageId);
            ActorIdToProto(SelfId(), taskDesc.MutableExecuter()->MutableActorId());

            for (auto& input : task.Inputs) {
                FillInputDesc(*taskDesc.AddInputs(), input);
            }

            for (auto& output : task.Outputs) {
                FillOutputDesc(*taskDesc.AddOutputs(), output);
            }

            taskDesc.MutableProgram()->CopyFrom(stage.GetProgram());

            PrepareKqpTaskParameters(stage, stageInfo, task, taskDesc, typeEnv, holderFactory);

            if (task.Meta.ShardId) {
                NKikimrTxDataShard::TKqpTransaction::TDataTaskMeta protoTaskMeta;

                FillTableMeta(stageInfo, protoTaskMeta.MutableTable());

                if (task.Meta.Reads) {
                    for (auto& read : *task.Meta.Reads) {
                        auto* protoReadMeta = protoTaskMeta.AddReads();
                        read.Ranges.SerializeTo(protoReadMeta->MutableRange());
                        for (auto& column : read.Columns) {
                            auto* protoColumn = protoReadMeta->AddColumns();
                            protoColumn->SetId(column.Id);
                            protoColumn->SetType(column.Type);
                            protoColumn->SetName(column.Name);
                        }
                        protoReadMeta->SetItemsLimit(task.Meta.ReadInfo.ItemsLimit);
                        protoReadMeta->SetReverse(task.Meta.ReadInfo.Reverse);
                    }
                }
                if (task.Meta.Writes) {
                    auto* protoWrites = protoTaskMeta.MutableWrites();
                    task.Meta.Writes->Ranges.SerializeTo(protoWrites->MutableRange());
                    if (task.Meta.Writes->IsPureEraseOp()) {
                        protoWrites->SetIsPureEraseOp(true);
                    }

                    for (const auto& [_, columnWrite] : task.Meta.Writes->ColumnWrites) {
                        auto& protoColumnWrite = *protoWrites->AddColumns();

                        auto& protoColumn = *protoColumnWrite.MutableColumn();
                        protoColumn.SetId(columnWrite.Column.Id);
                        protoColumn.SetType(columnWrite.Column.Type);
                        protoColumn.SetName(columnWrite.Column.Name);

                        protoColumnWrite.SetMaxValueSizeBytes(columnWrite.MaxValueSizeBytes);
                    }
                }

                taskDesc.MutableMeta()->PackFrom(protoTaskMeta);
                LOG_D("Task: " << task.Id << ", shard: " << task.Meta.ShardId << ", meta: " << protoTaskMeta.ShortDebugString());

                datashardTasks[task.Meta.ShardId].emplace_back(std::move(taskDesc));
            } else if (stageInfo.Meta.IsSysView()) {
                NKikimrTxDataShard::TKqpTransaction::TScanTaskMeta protoTaskMeta;

                FillTableMeta(stageInfo, protoTaskMeta.MutableTable());

                const auto& tableInfo = TableKeys.GetTable(stageInfo.Meta.TableId);
                for (const auto& keyColumnName : tableInfo.KeyColumns) {
                    const auto& keyColumn = tableInfo.Columns.at(keyColumnName);
                    protoTaskMeta.AddKeyColumnTypes(keyColumn.Type);
                }

                for (bool skipNullKey : stageInfo.Meta.SkipNullKeys) {
                    protoTaskMeta.AddSkipNullKeys(skipNullKey);
                }

                YQL_ENSURE(task.Meta.Reads);
                YQL_ENSURE(!task.Meta.Writes);

                for (auto& column : task.Meta.Reads->front().Columns) {
                    auto* protoColumn = protoTaskMeta.AddColumns();
                    protoColumn->SetId(column.Id);
                    protoColumn->SetType(column.Type);
                    protoColumn->SetName(column.Name);
                }

                for (auto& read : *task.Meta.Reads) {
                    auto* protoReadMeta = protoTaskMeta.AddReads();
                    protoReadMeta->SetShardId(read.ShardId);
                    read.Ranges.SerializeTo(protoReadMeta);

                    YQL_ENSURE((int) read.Columns.size() == protoTaskMeta.GetColumns().size());
                    for (ui64 i = 0; i < read.Columns.size(); ++i) {
                        YQL_ENSURE(read.Columns[i].Id == protoTaskMeta.GetColumns()[i].GetId());
                        YQL_ENSURE(read.Columns[i].Type == protoTaskMeta.GetColumns()[i].GetType());
                    }
                }

                LOG_D("task: " << task.Id << ", node: " << task.Meta.NodeId << ", meta: " << protoTaskMeta.ShortDebugString());

                taskDesc.MutableMeta()->PackFrom(protoTaskMeta);
                computeTasks.emplace_back(std::move(taskDesc));
            } else {
                computeTasks.emplace_back(std::move(taskDesc));
            }
        }

        if (computeTasks.size() > Request.MaxComputeActors) {
            LOG_N("Too many compute actors: " << computeTasks.size());
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED, TStringBuilder()
                    << "Requested too many execution units: " << computeTasks.size()));
            return;
        }

        ui32 shardsLimit = Request.MaxAffectedShards;
        if (i64 msc = (i64) RequestControls.MaxShardCount; msc > 0) {
            shardsLimit = std::min(shardsLimit, (ui32) msc);
        }
        if (shardsLimit > 0 && datashardTasks.size() > shardsLimit) {
            LOG_W("Too many affected shards: datashardTasks=" << datashardTasks.size() << ", limit: " << shardsLimit);
            Counters->TxProxyMon->TxResultError->Inc();
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED, TStringBuilder()
                    << "Affected too many shards: " << datashardTasks.size()));
            return;
        }

        bool fitSize = AllOf(datashardTasks, [this](const auto& x){ return ValidateTaskSize(x.second); });
        if (!fitSize) {
            Counters->TxProxyMon->TxResultError->Inc();
            return;
        }

        auto datashardTxs = BuildDatashardTxs(datashardTasks);

        // Single-shard transactions are always immediate
        ImmediateTx = datashardTxs.size() <= 1;
        switch (Request.IsolationLevel) {
            // OnlineRO with AllowInconsistentReads = true
            case NKikimrKqp::ISOLATION_LEVEL_READ_UNCOMMITTED:
            // StaleRO transactions always execute as immediate
            // (legacy behaviour, for compatibility with current execution engine)
            case NKikimrKqp::ISOLATION_LEVEL_READ_STALE:
                YQL_ENSURE(ReadOnlyTx);
                ImmediateTx = true;
                break;

            default:
                break;
        }

        if (ReadOnlyTx && Request.Snapshot.IsValid()) {
            // Snapshot reads are always immediate
            Snapshot = Request.Snapshot;
            ImmediateTx = true;
        }

        const bool forceSnapshot = (
                ReadOnlyTx &&
                !ImmediateTx &&
                !HasPersistentChannels &&
                !Database.empty() &&
                AppData()->FeatureFlags.GetEnableMvccSnapshotReads());

        if (forceSnapshot) {
            ComputeTasks = std::move(computeTasks);
            DatashardTxs = std::move(datashardTxs);

            auto longTxService = NLongTxService::MakeLongTxServiceID(SelfId().NodeId());
            Send(longTxService, new NLongTxService::TEvLongTxService::TEvAcquireReadSnapshot(Database));

            LOG_T("Create temporary mvcc snapshot, ebcome WaitSnapshotState");
            Become(&TKqpDataExecuter::WaitSnapshotState);
            if (ExecuterStateSpan) {
                ExecuterStateSpan.End();
                ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::DataExecuterWaitSnapshotState, ExecuterSpan.GetTraceId(), "WaitSnapshotState", NWilson::EFlags::AUTO_END);
            }

            return;
        }

        if (prepareTasksSpan) {
            prepareTasksSpan.End();
        }
        ContinueExecute(computeTasks, datashardTxs);
    }

    STATEFN(WaitSnapshotState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(NLongTxService::TEvLongTxService::TEvAcquireReadSnapshotResult, Handle);
                hFunc(TEvKqp::TEvAbortExecution, HandleAbortExecution);
                hFunc(TEvents::TEvWakeup, HandleTimeout);
                default:
                    UnexpectedEvent("WaitSnapshotState", ev->GetTypeRewrite());
            }
        } catch (const yexception& e) {
            InternalError(e.what());
        }
        ReportEventElapsedTime();
    }

    void Handle(NLongTxService::TEvLongTxService::TEvAcquireReadSnapshotResult::TPtr& ev) {
        auto& record = ev->Get()->Record;

        if (record.GetStatus() != Ydb::StatusIds::SUCCESS) {
            ReplyErrorAndDie(record.GetStatus(), record.MutableIssues());
            return;
        }

        Snapshot = TKqpSnapshot(record.GetSnapshotStep(), record.GetSnapshotTxId());
        ImmediateTx = true;

        auto computeTasks = std::move(ComputeTasks);
        auto datashardTxs = std::move(DatashardTxs);
        ContinueExecute(computeTasks, datashardTxs);
    }

    void ContinueExecute(
            TVector<NDqProto::TDqTask>& computeTasks,
            THashMap<ui64, NKikimrTxDataShard::TKqpTransaction>& datashardTxs)
    {
        UseFollowers = Request.IsolationLevel == NKikimrKqp::ISOLATION_LEVEL_READ_STALE;
        if (datashardTxs.size() > 1) {
            // Followers only allowed for single shard transactions.
            // (legacy behaviour, for compatibility with current execution engine)
            UseFollowers = false;
        }
        if (Snapshot.IsValid()) {
            // TODO: KIKIMR-11912
            UseFollowers = false;
        }
        if (UseFollowers) {
            YQL_ENSURE(ReadOnlyTx);
        }

        if (Stats) {
            //Stats->AffectedShards = datashardTxs.size();
            Stats->DatashardStats.reserve(datashardTxs.size());
            //Stats->ComputeStats.reserve(computeTasks.size());
        }

        Execute(computeTasks, datashardTxs);

        if (ImmediateTx) {
            LOG_T("Immediate tx, become ExecuteState");
            Become(&TKqpDataExecuter::ExecuteState);
            if (ExecuterStateSpan) {
                ExecuterStateSpan.End();
                ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::DataExecuterExecuteState, ExecuterSpan.GetTraceId(), "ExecuteState", NWilson::EFlags::AUTO_END);
            }
        } else {
            LOG_T("Not immediate tx, become PrepareState");
            Become(&TKqpDataExecuter::PrepareState);
            if (ExecuterStateSpan) {
                ExecuterStateSpan.End();
                ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::DataExecuterPrepareState, ExecuterSpan.GetTraceId(), "PrepareState", NWilson::EFlags::AUTO_END);
            }
        }
    }

    THashMap<ui64, NKikimrTxDataShard::TKqpTransaction> BuildDatashardTxs(const THashMap<ui64, TVector<NDqProto::TDqTask>>& datashardTasks) {
        THashMap<ui64, NKikimrTxDataShard::TKqpTransaction> datashardTxs;

        for (auto& [shardId, tasks]: datashardTasks) {
            auto& dsTxs = datashardTxs[shardId];
            for (auto& task: tasks) {
                dsTxs.AddTasks()->CopyFrom(task);
            }
        }

        if (auto locksMap = ExtractLocks(Request.Locks); !locksMap.empty()) {
            YQL_ENSURE(Request.ValidateLocks || Request.EraseLocks);
            auto locksOp = Request.ValidateLocks && Request.EraseLocks
                ? NKikimrTxDataShard::TKqpLocks::Commit
                : (Request.ValidateLocks
                        ? NKikimrTxDataShard::TKqpLocks::Validate
                        : NKikimrTxDataShard::TKqpLocks::Rollback);

            TSet<ui64> taskShardIds;
            if (Request.ValidateLocks) {
                for (auto& [shardId, _] : datashardTasks) {
                    if (ShardsWithEffects.contains(shardId)) {
                        taskShardIds.insert(shardId);
                    }
                }
            }

            TSet<ui64> locksSendingShards;
            for (auto& [shardId, locksList] : locksMap) {
                auto& tx = datashardTxs[shardId];
                tx.MutableLocks()->SetOp(locksOp);

                for (auto& lock : locksList) {
                    tx.MutableLocks()->MutableLocks()->Add()->Swap(&lock);
                }

                if (!locksList.empty() && Request.ValidateLocks) {
                    locksSendingShards.insert(shardId);
                }
            }

            if (Request.ValidateLocks) {
                NProtoBuf::RepeatedField<ui64> sendingShards(locksSendingShards.begin(), locksSendingShards.end());
                NProtoBuf::RepeatedField<ui64> receivingShards(taskShardIds.begin(), taskShardIds.end());
                for (auto& [shardId, shardTx] : datashardTxs) {
                    shardTx.MutableLocks()->SetOp(locksOp);
                    shardTx.MutableLocks()->MutableSendingShards()->CopyFrom(sendingShards);
                    shardTx.MutableLocks()->MutableReceivingShards()->CopyFrom(receivingShards);
                }
            }
        }

        return datashardTxs;
    }

    void Execute(TVector<NDqProto::TDqTask>& computeTasks, THashMap<ui64, NKikimrTxDataShard::TKqpTransaction>& datashardTxs) {
        auto lockTxId = Request.AcquireLocksTxId;
        if (lockTxId.Defined() && *lockTxId == 0) {
            lockTxId = TxId;
            LockHandle = TLockHandle(TxId, TActivationContext::ActorSystem());
        }

        NWilson::TSpan sendTasksSpan(TWilsonKqp::DataExecuterSendTasksAndTxs, ExecuterStateSpan.GetTraceId(), "SendTasksAndTxs", NWilson::EFlags::AUTO_END);
        LWTRACK(KqpDataExecuterStartTasksAndTxs, ResponseEv->Orbit, TxId, computeTasks.size(), datashardTxs.size());

        // first, start compute tasks
        TVector<ui64> computeTaskIds{Reserve(computeTasks.size())};
        for (auto&& taskDesc : computeTasks) {
            computeTaskIds.emplace_back(taskDesc.GetId());
            ExecuteDataComputeTask(std::move(taskDesc));
        }

        // then start data tasks with known actor ids of compute tasks
        for (auto& [shardId, shardTx] : datashardTxs) {
            shardTx.SetType(NKikimrTxDataShard::KQP_TX_TYPE_DATA);

            for (auto& protoTask : *shardTx.MutableTasks()) {
                ui64 taskId = protoTask.GetId();
                auto& task = TasksGraph.GetTask(taskId);

                for (ui64 outputIndex = 0; outputIndex < task.Outputs.size(); ++outputIndex) {
                    auto& output = task.Outputs[outputIndex];
                    auto* protoOutput = protoTask.MutableOutputs(outputIndex);

                    for (ui64 outputChannelIndex = 0; outputChannelIndex < output.Channels.size(); ++outputChannelIndex) {
                        ui64 outputChannelId = output.Channels[outputChannelIndex];
                        auto* protoChannel = protoOutput->MutableChannels(outputChannelIndex);

                        ui64 dstTaskId = TasksGraph.GetChannel(outputChannelId).DstTask;

                        if (dstTaskId == 0) {
                            continue;
                        }

                        auto& dstTask = TasksGraph.GetTask(dstTaskId);
                        if (dstTask.ComputeActorId) {
                            protoChannel->MutableDstEndpoint()->Clear();
                            ActorIdToProto(dstTask.ComputeActorId, protoChannel->MutableDstEndpoint()->MutableActorId());
                        } else {
                            if (protoChannel->HasDstEndpoint() && protoChannel->GetDstEndpoint().HasTabletId()) {
                                if (protoChannel->GetDstEndpoint().GetTabletId() == shardId) {
                                    // inplace update
                                } else {
                                    // TODO: send data via executer?
                                    // but we don't have such examples...
                                    YQL_ENSURE(false, "not implemented yet: " << protoTask.DebugString());
                                }
                            } else {
                                YQL_ENSURE(!protoChannel->GetDstEndpoint().IsInitialized());
                                // effects-only stage
                            }
                        }
                    }
                }

                LOG_D("datashard task: " << taskId << ", proto: " << protoTask.ShortDebugString());
            }

            ExecuteDatashardTransaction(shardId, shardTx, lockTxId);
        }

        if (sendTasksSpan) {
            sendTasksSpan.End();
        }

        LOG_I("Total tasks: " << TasksGraph.GetTasks().size()
            << ", readonly: " << ReadOnlyTx
            << ", datashardTxs: " << datashardTxs.size()
            << ", immediate: " << ImmediateTx
            << ", useFollowers: " << UseFollowers);

        LOG_T("Updating channels after the creation of compute actors");
        THashMap<TActorId, THashSet<ui64>> updates;
        for (ui64 taskId : computeTaskIds) {
            auto& task = TasksGraph.GetTask(taskId);
            CollectTaskChannelsUpdates(task, updates);
        }
        PropagateChannelsUpdates(updates);
        CheckExecutionComplete();
    }

    void Finalize() {
        auto& response = *ResponseEv->Record.MutableResponse();

        response.SetStatus(Ydb::StatusIds::SUCCESS);
        Counters->TxProxyMon->ReportStatusOK->Inc();

        TKqpProtoBuilder protoBuilder(*AppData()->FunctionRegistry);
        for (auto& result : Results) {
            auto* protoResult = response.MutableResult()->AddResults();
            if (result.IsStream) {
                protoBuilder.BuildStream(result.Data, result.ItemType, result.ResultItemType.Get(), protoResult);
            } else {
                protoBuilder.BuildValue(result.Data, result.ItemType, protoResult);
            }
        }

        if (!Locks.empty()) {
            if (LockHandle) {
                ResponseEv->LockHandle = std::move(LockHandle);
            }
            BuildLocks(*response.MutableResult()->MutableLocks(), Locks);
        }

        if (Stats) {
            ReportEventElapsedTime();

            Stats->FinishTs = TInstant::Now();
            Stats->ResultRows = response.GetResult().ResultsSize();
            Stats->Finish();

            if (CollectFullStats(Request.StatsMode)) {
                for (ui32 txId = 0; txId < Request.Transactions.size(); ++txId) {
                    const auto& tx = Request.Transactions[txId].Body;
                    auto planWithStats = AddExecStatsToTxPlan(tx->GetPlan(), response.GetResult().GetStats());
                    response.MutableResult()->MutableStats()->AddTxPlansWithStats(planWithStats);
                }
            }

            Stats.reset();
        }

        auto resultSize = response.ByteSize();
        if (resultSize > (int)ReplySizeLimit) {
            TString message = TStringBuilder() << "Query result size limit exceeded. ("
                << resultSize << " > " << ReplySizeLimit << ")";

            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_RESULT_UNAVAILABLE, message);
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED, issue);
            return;
        }

        LWTRACK(KqpDataExecuterFinalize, ResponseEv->Orbit, TxId, LastShard, response.GetResult().ResultsSize(), response.ByteSize());

        if (ExecuterStateSpan) {
            ExecuterStateSpan.End();
            ExecuterStateSpan = {};
        }

        if (ExecuterSpan) {
            ExecuterSpan.EndOk();
        }

        LOG_D("Sending response to: " << Target << ", results: " << Results.size());
        Send(Target, ResponseEv.release());
        PassAway();
    }

    void PassAway() override {
        auto totalTime = TInstant::Now() - StartTime;
        Counters->Counters->DataTxTotalTimeHistogram->Collect(totalTime.MilliSeconds());

        // TxProxyMon compatibility
        Counters->TxProxyMon->TxTotalTimeHgram->Collect(totalTime.MilliSeconds());
        Counters->TxProxyMon->TxExecuteTimeHgram->Collect(totalTime.MilliSeconds());

        Send(MakePipePeNodeCacheID(false), new TEvPipeCache::TEvUnlink(0));

        if (UseFollowers) {
            Send(MakePipePeNodeCacheID(true), new TEvPipeCache::TEvUnlink(0));
        }

        TBase::PassAway();
    }

public:
    static void FillEndpointDesc(NDqProto::TEndpoint& endpoint, const TTask& task) {
        if (task.ComputeActorId) {
            ActorIdToProto(task.ComputeActorId, endpoint.MutableActorId());
        } else if (task.Meta.ShardId) {
            endpoint.SetTabletId(task.Meta.ShardId);
        }
    }

    void FillChannelDesc(NDqProto::TChannel& channelDesc, const TChannel& channel) {
        channelDesc.SetId(channel.Id);
        channelDesc.SetSrcTaskId(channel.SrcTask);
        channelDesc.SetDstTaskId(channel.DstTask);

        YQL_ENSURE(channel.SrcTask, "" << this->DebugString());
        FillEndpointDesc(*channelDesc.MutableSrcEndpoint(), TasksGraph.GetTask(channel.SrcTask));

        if (channel.DstTask) {
            FillEndpointDesc(*channelDesc.MutableDstEndpoint(), TasksGraph.GetTask(channel.DstTask));
        } else {
            // result channel
            ActorIdToProto(SelfId(), channelDesc.MutableDstEndpoint()->MutableActorId());
        }

        channelDesc.SetIsPersistent(IsCrossShardChannel(TasksGraph, channel));
        channelDesc.SetInMemory(channel.InMemory);

        if (channelDesc.GetIsPersistent()) {
            HasPersistentChannels = true;
        }
    }

private:
    void ReplyTxStateUnknown(ui64 shardId) {
        auto message = TStringBuilder() << "Tx state unknown for shard " << shardId << ", txid " << TxId;
        if (ReadOnlyTx) {
            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
            issue.AddSubIssue(new TIssue(message));
            issue.GetSubIssues()[0]->SetCode(NKikimrIssues::TIssuesIds::TX_STATE_UNKNOWN, TSeverityIds::S_ERROR);
            ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
        } else {
            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_STATE_UNKNOWN);
            issue.AddSubIssue(new TIssue(message));
            issue.GetSubIssues()[0]->SetCode(NKikimrIssues::TIssuesIds::TX_STATE_UNKNOWN, TSeverityIds::S_ERROR);
            ReplyErrorAndDie(Ydb::StatusIds::UNDETERMINED, issue);
        }
    }

    static void AddDataShardErrors(const NKikimrTxDataShard::TEvProposeTransactionResult& result, TIssue& issue) {
        for (const auto &err : result.GetError()) {
            issue.AddSubIssue(new TIssue(TStringBuilder()
                << "[" << NKikimrTxDataShard::TError_EKind_Name(err.GetKind()) << "] " << err.GetReason()));
        }
    }

    static std::string_view ToString(TShardState::EState state) {
        switch (state) {
            case TShardState::EState::Initial:   return "Initial"sv;
            case TShardState::EState::Preparing: return "Preparing"sv;
            case TShardState::EState::Prepared:  return "Prepared"sv;
            case TShardState::EState::Executing: return "Executing"sv;
            case TShardState::EState::Finished:  return "Finished"sv;
        }
    }

private:
    NTxProxy::TRequestControls RequestControls;
    ui64 TxCoordinator = 0;
    THashMap<ui64, TShardState> ShardStates;
    TVector<NKikimrTxDataShard::TLock> Locks;
    TVector<TKqpExecuterTxResult> Results;
    bool ReadOnlyTx = true;
    bool ImmediateTx = false;
    bool UseFollowers = false;
    bool TxPlanned = false;

    TInstant FirstPrepareReply;
    TInstant LastPrepareReply;

    // Tracks which shards are expected to have effects
    THashSet<ui64> ShardsWithEffects;
    bool HasPersistentChannels = false;

    // Either requested or temporarily acquired snapshot
    TKqpSnapshot Snapshot;

    // Temporary storage during snapshot acquisition
    TVector<NDqProto::TDqTask> ComputeTasks;
    THashMap<ui64, NKikimrTxDataShard::TKqpTransaction> DatashardTxs;

    // Lock handle for a newly acquired lock
    TLockHandle LockHandle;
    ui64 LastShard = 0;
};

} // namespace

IActor* CreateKqpDataExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database, const TMaybe<TString>& userToken,
    TKqpRequestCounters::TPtr counters)
{
    return new TKqpDataExecuter(std::move(request), database, userToken, counters);
}

} // namespace NKqp
} // namespace NKikimr
