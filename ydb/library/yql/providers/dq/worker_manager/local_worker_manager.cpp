#include "local_worker_manager.h"
#include <ydb/library/yql/providers/dq/worker_manager/interface/events.h>

#include <ydb/library/yql/providers/dq/actors/actor_helpers.h>
#include <ydb/library/yql/providers/dq/actors/compute_actor.h>
#include <ydb/library/yql/providers/dq/actors/worker_actor.h>
#include <ydb/library/yql/providers/dq/runtime/runtime_data.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_impl.h>
#include <ydb/library/yql/dq/common/dq_resource_quoter.h>

#include <ydb/library/yql/utils/failure_injector/failure_injector.h>
#include <ydb/library/yql/utils/log/log.h>

#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/interconnect/interconnect.h>

#include "worker_manager_common.h"

#include <util/generic/vector.h>
#include <util/system/mutex.h>
#include <util/random/random.h>
#include <util/system/rusage.h>

using namespace NActors;

namespace NYql::NDqs {

union TDqLocalResourceId {
    struct {
        ui32 Counter;
        ui16 Seed;
        ui16 NodeId;
    };
    ui64 Data;
};

static_assert(sizeof(TDqLocalResourceId) == 8);

class TLocalWorkerManager: public TWorkerManagerCommon<TLocalWorkerManager> {

public:
    static constexpr char ActorName[] = "YQL_DQ_LWM";

    TLocalWorkerManager(const TLocalWorkerManagerOptions& options)
        : TWorkerManagerCommon<TLocalWorkerManager>(&TLocalWorkerManager::Handler)
        , Options(options)
        , MemoryQuoter(std::make_shared<NDq::TResourceQuoter>(Options.MkqlTotalMemoryLimit))
    {
        Options.Counters.MkqlMemoryLimit->Set(Options.MkqlTotalMemoryLimit);
        Options.Counters.MkqlMemoryAllocated->Set(0);

        MemoryQuoter->SetNotifier([limitCounter = Options.Counters.MkqlMemoryLimit, allocatedCounter = Options.Counters.MkqlMemoryAllocated](const ui64 limit, ui64 allocated) {
            limitCounter->Set(limit);
            allocatedCounter->Set(allocated);
        });

        AllocateMemoryFn = [quoter = MemoryQuoter](const auto& txId, ui64, ui64 size) {
            // mem per task is not tracked yet
            return quoter->Allocate(txId, 0, size);
        };

        FreeMemoryFn = [quoter = MemoryQuoter](const auto& txId, ui64, ui64 size) {
            // mem per task is not tracked yet
            quoter->Free(txId, 0, size);
        };
    }

private:
    STRICT_STFUNC(Handler, {
        hFunc(TEvAllocateWorkersRequest, OnAllocateWorkersRequest)
        hFunc(TEvFreeWorkersNotify, OnFreeWorkers)
        cFunc(TEvents::TEvPoison::EventType, PassAway)
        cFunc(TEvents::TEvBootstrap::EventType, Bootstrap)
        cFunc(TEvents::TEvWakeup::EventType, WakeUp)
        IgnoreFunc(TEvInterconnect::TEvNodeConnected)
        hFunc(TEvInterconnect::TEvNodeDisconnected, OnDisconnected)
        hFunc(TEvents::TEvUndelivered, OnUndelivered)
        hFunc(TEvConfigureFailureInjectorRequest, OnConfigureFailureInjector)
        HFunc(TEvRoutesRequest, OnRoutesRequest)
        hFunc(TEvQueryStatus, OnQueryStatus)
    })

    TAutoPtr<IEventHandle> AfterRegister(const TActorId& self, const TActorId& parentId) override {
        return new IEventHandle(self, parentId, new TEvents::TEvBootstrap(), 0);
    }

    void Bootstrap() {
        ResourceId.Seed = static_cast<ui16>(RandomNumber<ui64>());
        ResourceId.Counter = 0;

        Send(SelfId(), new TEvents::TEvWakeup());
    }

    void WakeUp() {
        auto currentRusage = TRusage::Get();
        TRusage delta;
        delta.Utime = currentRusage.Utime - Rusage.Utime;
        delta.Stime = currentRusage.Stime - Rusage.Stime;
        delta.MajorPageFaults = currentRusage.MajorPageFaults - Rusage.MajorPageFaults;
        if (Options.RuntimeData) {
            Options.RuntimeData->AddRusageDelta(delta);
        }
        Rusage = currentRusage;

        FreeOnDeadline();

        TActivationContext::Schedule(TDuration::MilliSeconds(800), new IEventHandle(SelfId(), SelfId(), new TEvents::TEvWakeup(), 0));
    }

    void DoPassAway() override {
        for (const auto& [resourceId, _] : AllocatedWorkers) {
            FreeGroup(resourceId);
        }

        AllocatedWorkers.clear();
        _exit(0);
    }

    void Deallocate(ui32 nodeId) {
        TVector<ui64> toDeallocate;

        YQL_CLOG(DEBUG, ProviderDq) << "Deallocate " << nodeId;
        for (const auto& [k, v] : AllocatedWorkers) {
            if (v.Sender.NodeId() == nodeId) {
                toDeallocate.push_back(k);
            }
        }

        for (const auto& k : toDeallocate) {
            FreeGroup(k);
        }
    }

    void Deallocate(const NActors::TActorId& senderId) {
        TVector<ui64> toDeallocate;

        YQL_CLOG(DEBUG, ProviderDq) << "Deallocate " << senderId;
        for (const auto& [k, v] : AllocatedWorkers) {
            if (v.Sender == senderId) {
                toDeallocate.push_back(k);
            }
        }

        for (const auto& k : toDeallocate) {
            FreeGroup(k);
        }
    }

    void OnDisconnected(TEvInterconnect::TEvNodeDisconnected::TPtr& ev)
    {
        YQL_CLOG(DEBUG, ProviderDq) << "Disconnected " << ev->Get()->NodeId;
        Unsubscribe(ev->Get()->NodeId);
        Deallocate(ev->Get()->NodeId);
    }

    void OnUndelivered(TEvents::TEvUndelivered::TPtr& ev)
    {
        Y_VERIFY(ev->Get()->Reason == TEvents::TEvUndelivered::Disconnected
            || ev->Get()->Reason == TEvents::TEvUndelivered::ReasonActorUnknown);

        YQL_CLOG(DEBUG, ProviderDq) << "Undelivered " << ev->Sender;

        switch (ev->Get()->Reason) {
        case TEvents::TEvUndelivered::Disconnected:
            Deallocate(ev->Sender.NodeId());
            break;
        case TEvents::TEvUndelivered::ReasonActorUnknown:
            Deallocate(ev->Sender);
            break;
        default:
            break;
        }
    }

    void OnConfigureFailureInjector(TEvConfigureFailureInjectorRequest::TPtr& ev) {
        YQL_CLOG(DEBUG, ProviderDq) << "TEvConfigureFailureInjectorRequest ";

        auto& request = ev->Get()->Record.GetRequest();
        YQL_ENSURE(request.GetNodeId() == SelfId().NodeId(), "Wrong node id!");

        TFailureInjector::Set(request.GetName(), request.GetSkip(), request.GetCountOfFails());
        YQL_CLOG(DEBUG, ProviderDq) << "Failure injector is configured " << request.GetName();

        auto response = MakeHolder<TEvConfigureFailureInjectorResponse>();
        auto* r = response->Record.MutableResponse();
        r->Setsuccess(true);

        Send(ev->Sender, response.Release());
    }

    void OnAllocateWorkersRequest(TEvAllocateWorkersRequest::TPtr& ev) {
        ui64 resourceId;
        if (ev->Get()->Record.GetResourceId()) {
            resourceId = ev->Get()->Record.GetResourceId();
        } else {
            auto resource = ResourceId;
            resource.NodeId = ev->Sender.NodeId();
            resourceId = resource.Data;
            ResourceId.Counter ++;
        }
        bool createComputeActor = ev->Get()->Record.GetCreateComputeActor();
        TString computeActorType = ev->Get()->Record.GetComputeActorType();

        if (createComputeActor && !Options.CanUseComputeActor) {
            Send(ev->Sender, MakeHolder<TEvAllocateWorkersResponse>("Compute Actor Disabled"), 0, ev->Cookie);
            return;
        }

        YQL_LOG_CTX_ROOT_SCOPE(ev->Get()->Record.GetTraceId());
        YQL_CLOG(DEBUG, ProviderDq) << "TLocalWorkerManager::TEvAllocateWorkersRequest " << resourceId;
        TFailureInjector::Reach("allocate_workers_failure", [] { ::_exit(1); });

        auto& allocationInfo = AllocatedWorkers[resourceId];
        auto traceId = ev->Get()->Record.GetTraceId();
        allocationInfo.TxId = traceId;

        auto count = ev->Get()->Record.GetCount();

        Y_VERIFY(count > 0);

        bool canAllocate = MemoryQuoter->Allocate(traceId, 0, count * Options.MkqlInitialMemoryLimit);

        if (!canAllocate) {
            Send(ev->Sender, MakeHolder<TEvAllocateWorkersResponse>("Not enough memory to allocate tasks"), 0, ev->Cookie);
            return;
        }

        if (allocationInfo.WorkerActors.empty()) {
            allocationInfo.WorkerActors.reserve(count);
            allocationInfo.Sender = ev->Sender;
            if (ev->Get()->Record.GetFreeWorkerAfterMs()) {
                allocationInfo.Deadline =
                    TInstant::Now() + TDuration::MilliSeconds(ev->Get()->Record.GetFreeWorkerAfterMs());
            }

            auto& tasks = *ev->Get()->Record.MutableTask();

            if (createComputeActor) {
                Y_VERIFY(static_cast<int>(tasks.size()) == static_cast<int>(count));
            }
            auto resultId = ActorIdFromProto(ev->Get()->Record.GetResultActorId());

            for (ui32 i = 0; i < count; i++) {
                THolder<NActors::IActor> actor;

                if (createComputeActor) {
                    auto id = tasks[i].GetId();
                    auto stageId = tasks[i].GetStageId();
                    YQL_CLOG(DEBUG, ProviderDq) << "Create compute actor: " << computeActorType;
                    auto taskCounters = Options.DqTaskCounters ? Options.DqTaskCounters->GetSubgroup("operation", traceId)->GetSubgroup("stage", ToString(stageId))->GetSubgroup("id", ToString(id)) : nullptr;
                    actor.Reset(NYql::CreateComputeActor(
                        Options,
                        Options.MkqlTotalMemoryLimit ? AllocateMemoryFn : nullptr,
                        Options.MkqlTotalMemoryLimit ? FreeMemoryFn : nullptr,
                        resultId,
                        traceId,
                        std::move(tasks[i]),
                        computeActorType,
                        Options.TaskRunnerActorFactory,
                        taskCounters));
                } else {
                    actor.Reset(CreateWorkerActor(
                        Options.RuntimeData,
                        traceId,
                        Options.TaskRunnerActorFactory,
                        Options.AsyncIoFactory));
                }
                allocationInfo.WorkerActors.emplace_back(RegisterChild(
                    actor.Release(), createComputeActor ? NYql::NDq::TEvDq::TEvAbortExecution::Unavailable("Aborted by LWM").Release() : nullptr
                ));
            }

            Options.Counters.ActiveWorkers->Add(count);
        }

        Send(ev->Sender,
            MakeHolder<TEvAllocateWorkersResponse>(resourceId, allocationInfo.WorkerActors),
            IEventHandle::FlagTrackDelivery | IEventHandle::FlagSubscribeOnSession,
            ev->Cookie);
        Subscribe(ev->Sender.NodeId());
    }

    void OnFreeWorkers(TEvFreeWorkersNotify::TPtr& ev) {
        ui64 resourceId = ev->Get()->Record.GetResourceId();
        YQL_CLOG(DEBUG, ProviderDq) << "TEvFreeWorkersNotify " << resourceId;
        FreeGroup(resourceId, ev->Sender);
    }

    void OnQueryStatus(TEvQueryStatus::TPtr& ev) {
        auto response = MakeHolder<TEvQueryStatusResponse>();
        Send(ev->Sender, response.Release());
    }

    void FreeGroup(ui64 id, NActors::TActorId sender = NActors::TActorId()) {
        YQL_CLOG(DEBUG, ProviderDq) << "Free Group " << id;
        auto it = AllocatedWorkers.find(id);
        if (it != AllocatedWorkers.end()) {
            for (const auto& actorId : it->second.WorkerActors) {
                UnregisterChild(actorId);
            }

            if (sender && it->second.Sender != sender) {
                Options.Counters.FreeGroupError->Inc();
                YQL_CLOG(ERROR, ProviderDq) << "Free Group " << id << " mismatched alloc-free senders: " << it->second.Sender << " and " << sender << " TxId: " << it->second.TxId;
            }

            MemoryQuoter->Free(it->second.TxId, 0);
            Options.Counters.ActiveWorkers->Sub(it->second.WorkerActors.size());
            AllocatedWorkers.erase(it);
        }
    }

    void FreeOnDeadline() {
        auto now = TInstant::Now();
        THashSet<ui32> todelete;
        for (const auto& [id, info] : AllocatedWorkers) {
            if (info.Deadline && info.Deadline < now) {
                todelete.insert(id);
            }
        }
        for (const auto& id : todelete) {
            YQL_CLOG(DEBUG, ProviderDq) << "Free on deadline: " << id;
            FreeGroup(id);
        }
    }

    TLocalWorkerManagerOptions Options;

    struct TAllocationInfo {
        TVector<NActors::TActorId> WorkerActors;
        NActors::TActorId Sender;
        TInstant Deadline;
        NDq::TTxId TxId;
    };
    THashMap<ui64, TAllocationInfo> AllocatedWorkers;
    TDqLocalResourceId ResourceId;

    TRusage Rusage;

    NDq::TAllocateMemoryCallback AllocateMemoryFn;
    NDq::TFreeMemoryCallback FreeMemoryFn;
    std::shared_ptr<NDq::TResourceQuoter> MemoryQuoter;
};


NActors::IActor* CreateLocalWorkerManager(const TLocalWorkerManagerOptions& options)
{
    return new TLocalWorkerManager(options);
}

} // namespace NYql::NDqs
