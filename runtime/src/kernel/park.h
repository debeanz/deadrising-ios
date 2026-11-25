#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Waiting for a value in memory to change (critical sections, events, semaphores, thread
// suspension). These waited with C++20 atomic wait / notify_one; on the iPhone the game froze
// with its main thread asleep on the allocator's critical section while no thread held it
// (2026-11-25, hang watchdog): Apple's C++ library keeps the waiters of 1- and 4-byte atomics in a
// shared hash table, and notify_one there can wake a waiter of another address. Here each wait
// re-checks its condition under a bucket lock that every wake takes (no wake can fall between the
// check and the sleep), wakes reach every waiter of the bucket, and a timed wait re-checks in any
// case, every 10 ms, so a waiter never sleeps forever. A re-check that finds the wait over counts
// as a missed wake (MissedWakes, in the "[perf]" report): none on the PC; on the iPhone they come
// when the cores are all busy and the waking thread is held up between its change and its wake
// (2026-11-25: a re-check backing off to 100 ms made those waits longer; back to 10 ms).
namespace Park
{
    struct Bucket
    {
        std::mutex mutex;
        std::condition_variable condition;
    };

    inline Bucket& For(const void* address)
    {
        static Bucket buckets[256];
        const uintptr_t value = uintptr_t(address);
        return buckets[((value >> 3) ^ (value >> 11)) & 255];
    }

    inline std::atomic<uint64_t>& MissedWakes()
    {
        static std::atomic<uint64_t> count{ 0 };
        return count;
    }

    // Sleeps while stillWaiting() returns true (called with the bucket locked), at most until 'deadline'. True when
    // the wait is over, false at the deadline.
    template<class StillWaiting>
    bool WaitWhileUntil(const void* address, std::chrono::steady_clock::time_point deadline, StillWaiting stillWaiting)
    {
        Bucket& bucket = For(address);
        std::unique_lock lock(bucket.mutex);
        auto recheck = std::chrono::milliseconds(10);
        while (stillWaiting())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            const auto until = std::min(deadline, now + recheck);
            if (bucket.condition.wait_until(lock, until) == std::cv_status::timeout && until != deadline)
            {
                if (!stillWaiting())
                {
                    MissedWakes().fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
        }
        return true;
    }

    // Sleeps while stillWaiting() returns true (called with the bucket locked).
    template<class StillWaiting>
    void WaitWhile(const void* address, StillWaiting stillWaiting)
    {
        WaitWhileUntil(address, std::chrono::steady_clock::time_point::max(), stillWaiting);
    }

    // After changing the value: wakes the waiters of its bucket.
    inline void Wake(const void* address)
    {
        Bucket& bucket = For(address);
        {
            std::lock_guard lock(bucket.mutex);
        }
        bucket.condition.notify_all();
    }
}
