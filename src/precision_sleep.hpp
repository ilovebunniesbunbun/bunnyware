#pragma once

#include <cerrno>
#include <chrono>
#include <ctime>

// Sleeps the calling thread until an absolute CLOCK_MONOTONIC deadline using
// clock_nanosleep(TIMER_ABSTIME). Unlike sleep_for() followed by a yield-spin,
// this yields the CPU for the entire wait (no busy polling of the clock) and
// cannot accumulate drift because the deadline is absolute. The absolute
// deadline is also immune to EINTR: on signal interruption the sleep simply
// resumes against the same target time.
inline void precision_sleep_until(std::chrono::steady_clock::time_point deadline) {
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(deadline);
    const auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - secs);

    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(secs.time_since_epoch().count());
    ts.tv_nsec = static_cast<long>(nsecs.count());

    int rc = 0;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    } while (rc == EINTR);
}

// Convenience overload: sleep for a relative duration, anchored to the
// steady clock at call time.
template <class Rep, class Period>
inline void precision_sleep_for(const std::chrono::duration<Rep, Period>& duration) {
    if (duration <= std::chrono::duration<Rep, Period>::zero()) return;
    precision_sleep_until(std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
}
