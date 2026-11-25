#pragma once

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
// check and the sleep), wakes reach every waiter of the bucket, and a 10 ms timed wait re-checks
// in any case, so a waiter never sleeps forever.
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

    // Sleeps while stillWaiting() returns true (called with the bucket locked).
    template<class StillWaiting>
    void WaitWhile(const void* address, StillWaiting stillWaiting)
    {
        Bucket& bucket = For(address);
        std::unique_lock lock(bucket.mutex);
        while (stillWaiting())
            bucket.condition.wait_for(lock, std::chrono::milliseconds(10));
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
