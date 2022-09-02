#include "test_load_actor.h"

#include <ydb/core/base/appdata.h>

#include <library/cpp/monlib/service/pages/templates.h>

namespace NKikimr {

class TMemoryTestLoadActor : public TActorBootstrapped<TMemoryTestLoadActor> {
    enum {
        EvAllocateBlock = EventSpaceBegin(TEvents::ES_PRIVATE),
        EvEnd
    };

    struct TEvAllocateBlock : public TEventLocal<TEvAllocateBlock, EvAllocateBlock> {};

    const TActorId Parent;
    const ui64 Tag;

    TDuration Duration;
    ui64 BlockSize;
    TDuration Interval;

    TInstant TestStartTime;
    TVector<TVector<char>> Blocks;
    ui64 AllocatedSize = 0;

public:
    static constexpr auto ActorActivityType() {
        return NKikimrServices::TActivity::BS_LOAD_PDISK_LOG_WRITE;
    }

    TMemoryTestLoadActor(const NKikimrBlobStorage::TEvTestLoadRequest::TMemoryLoadStart& cmd,
        const TActorId& parent, const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters, ui64 index, ui64 tag)
        : Parent(parent)
        , Tag(tag)
    {
        Y_UNUSED(counters);
        Y_UNUSED(index);

        VERIFY_PARAM(DurationSeconds);
        Duration = TDuration::Seconds(cmd.GetDurationSeconds());

        VERIFY_PARAM(BlockSize);
        BlockSize = cmd.GetBlockSize();

        VERIFY_PARAM(IntervalUs);
        Interval = TDuration::MicroSeconds(cmd.GetIntervalUs());

        Blocks.reserve(Duration.MicroSeconds() / Interval.MicroSeconds() + 1);
    }

    void Bootstrap(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::BS_LOAD_TEST, "Tag# " << Tag
            << " TMemoryTestLoadActor Bootstrap called");

        Become(&TMemoryTestLoadActor::StateFunc);

        LOG_INFO_S(ctx, NKikimrServices::BS_LOAD_TEST, "Tag# " << Tag
            << " Schedule PoisonPill");

        ctx.Schedule(Duration, new TEvents::TEvPoisonPill);
        ctx.Schedule(Interval, new TEvAllocateBlock);
        TestStartTime = TAppData::TimeProvider->Now();
    }

    void HandlePoisonPill(const TActorContext& ctx) {
        LOG_INFO_S(ctx, NKikimrServices::BS_LOAD_TEST, "Tag# " << Tag
            << " Handle PoisonPill");

        TIntrusivePtr<TLoadReport> report(new TLoadReport());
        report->Duration = Duration;
        ctx.Send(Parent, new TEvTestLoadFinished(Tag, report, "OK"));
        Die(ctx);
    }

    void Handle(TEvAllocateBlock::TPtr&, const TActorContext& ctx) {
        auto size = RandomNumber<ui64>(BlockSize * 2 + 1);

        Blocks.push_back({});
        auto& block = Blocks.back();
        block.resize(size);
        for (size_t i = 0; i < size; ++i) {
            block[i] = 0;
        }
        AllocatedSize += size;

        LOG_DEBUG_S(ctx, NKikimrServices::BS_LOAD_TEST, "Tag# " << Tag
            << " Handle AllocateBlock");

        ctx.Schedule(Interval, new TEvAllocateBlock);
    }

    void Handle(NMon::TEvHttpInfo::TPtr& ev, const TActorContext& ctx) {
#define PARAM(NAME, VALUE) \
    TABLER() { \
        TABLED() { str << NAME; } \
        TABLED() { str << VALUE; } \
    }
        TStringStream str;
        HTML(str) {
            TABLE_CLASS("table table-condensed") {
                TABLEHEAD() {
                    TABLER() {
                        TABLEH() { str << "Parameter"; }
                        TABLEH() { str << "Value"; }
                    }
                }
                TABLEBODY() {
                    PARAM("Elapsed time / Duration",
                        (TAppData::TimeProvider->Now() - TestStartTime).Seconds() << "s / "
                        << Duration.Seconds() << "s");
                    PARAM("Interval", Interval.MicroSeconds() << "us");
                    PARAM("Block size", BlockSize);
                    PARAM("Allocated bytes", AllocatedSize);
                    PARAM("Allocated blocks", Blocks.size());
                }
            }
        }

        ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes(str.Str(), ev->Get()->SubRequestId));
    }

    STRICT_STFUNC(StateFunc,
        CFunc(TEvents::TSystem::PoisonPill, HandlePoisonPill)
        HFunc(TEvAllocateBlock, Handle)
        HFunc(NMon::TEvHttpInfo, Handle)
    )
};

IActor* CreateMemoryTestLoad(
    const NKikimrBlobStorage::TEvTestLoadRequest::TMemoryLoadStart& cmd,
    const TActorId& parent,
    const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters,
    ui64 index,
    ui64 tag)
{
    return new TMemoryTestLoadActor(cmd, parent, counters, index, tag);
}

} // NKikimr
