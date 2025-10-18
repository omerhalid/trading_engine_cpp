#pragma once

#include <cstdint>
#include <immintrin.h>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

namespace hft {

/**
 * Timestamping utilities - critical for latency measurement
 */
class LatencyTracker {
public:
    /**
     * Get current time in nanoseconds using RDTSC (Read Time-Stamp Counter)
     * This is the fastest way to get timestamps on x86_64
     * 
     * Note: Requires TSC to be synchronized across cores (modern CPUs do this)
     * Alternative: clock_gettime(CLOCK_MONOTONIC_RAW) if TSC is unreliable
     */
    static inline uint64_t rdtsc() noexcept {
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }
    
    /**
     * Serializing version of RDTSC - ensures all prior instructions complete
     * Use this when you need accurate "after operation" timestamps
     */
    static inline uint64_t rdtscp() noexcept {
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }
    
    /**
     * Convert TSC ticks to nanoseconds
     * In production, calibrate this on startup
     */
    static inline uint64_t tsc_to_ns(uint64_t tsc, double tsc_freq_ghz = 3.0) noexcept {
        return static_cast<uint64_t>(tsc / tsc_freq_ghz);
    }
};

/**
 * CPU affinity and thread pinning utilities
 * Critical for consistent low latency - avoid context switches
 */
class ThreadUtils {
public:
    /**
     * Pin thread to specific CPU core
     * In production, you'd pin:
     * - Feed handler to core 0
     * - Trading logic to core 1
     * - Order gateway to core 2
     * All on same NUMA node, with isolated cores
     */
    static bool pin_to_core(int core_id) noexcept {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
        // macOS doesn't support thread affinity in the same way
        (void)core_id;
        return true; // No-op for compatibility
#endif
    }
    
    /**
     * Set thread priority to real-time
     * Requires root/CAP_SYS_NICE
     */
    static bool set_realtime_priority() noexcept {
#ifdef __linux__
        struct sched_param param;
        param.sched_priority = 99; // Max priority
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
        return true; // No-op for compatibility
#endif
    }
};

/**
 * Busy-wait spinlock utilities
 * Better than yield/sleep for sub-microsecond waits
 */
class SpinWait {
public:
    /**
     * Pause instruction - tells CPU we're in a spin loop
     * Reduces power consumption and improves performance
     */
    static inline void pause() noexcept {
        _mm_pause(); // x86 specific
    }
    
    /**
     * Spin for a specific number of iterations
     */
    static void spin(uint32_t iterations) noexcept {
        for (uint32_t i = 0; i < iterations; ++i) {
            pause();
        }
    }
};

} // namespace hft

