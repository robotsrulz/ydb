#include "monotonic.h"

#include <chrono>

namespace NActors {

    namespace {
        // Unfortunately time_since_epoch() is sometimes negative on wine
        // Remember initial time point at program start and use offsets from that
        std::chrono::steady_clock::time_point MonotonicOffset = std::chrono::steady_clock::now();
    }

    ui64 GetMonotonicMicroSeconds() {
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - MonotonicOffset).count();
        // Steady clock is supposed to never jump backwards, but it's better to be safe in case of buggy implementations
        if (Y_UNLIKELY(microseconds < 0)) {
            microseconds = 0;
        }
        // Add one so we never return zero
        return microseconds + 1;
    }

} // namespace NActors

template<>
void Out<NActors::TMonotonic>(
    IOutputStream& o,
    NActors::TMonotonic t)
{
    o << t - NActors::TMonotonic::Zero();
}
