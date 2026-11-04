#pragma once
#include <time.h>   // time_t — retryAfter() answers in seconds

// Clock maintenance for the radio windows: an SNTP sync once a week, retried after a failure, and a way
// to force the next one when the clock is suspect. The wake schedule itself — profiles, night window,
// intervals — lives in bench_screens.cpp (readPlan).
class ClockSync {
public:
    // How long to stand back after `fails` failed syncs in a row: 6 h -> 12 -> 24 with a clock,
    // 30 min -> 1 h -> 2 -> 4 without one. Flat retries were the mistake the Wi-Fi join path made
    // until a router switched off for a weekend flattened a cell — an attempt that cannot succeed
    // costs up to 30 s of radio, and four a day doubles the whole radio budget for ever.
    //
    // Visible here rather than hidden in the .cpp because it is ARITHMETIC — a ladder with two caps
    // and an off-by-one waiting to happen — and arithmetic deserves to be asked directly rather than
    // inferred from the shape of the code. The log line the owner may read quotes it too.
    static time_t retryAfter(bool clock_valid, unsigned fails);

    static ClockSync& getInstance();

    // Call while Wi-Fi is connected: an SNTP sync if the clock was never set or the last sync is a week
    // old, and not sooner than 6 h after a failed attempt (30 min while the clock has never been set).
    // Blocks up to 30 s.
    void maintainClock();

    // Forget when we last synced, so the next radio window re-disciplines the clock whatever the
    // routine interval says. For the cases where the clock is not merely old but SUSPECT: the
    // device lost power, was reflashed, or is reporting a time earlier than data it has already
    // recorded. A weekly schedule is right for drift and wrong for a clock that is simply wrong.
    void forceClockSync();

private:
    ClockSync() = default;
};
