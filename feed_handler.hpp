#pragma once

#include "spsc_queue.hpp"
#include <cstdint>
#include <cstring>
#include <chrono>
#include <array>
#include <immintrin.h> // For _mm_pause() - x86 specific

namespace hft {

/**
 * Market data message types
 * In real systems: NYSE, NASDAQ, CME have proprietary binary protocols
 * (e.g., ITCH for NASDAQ, MDP3 for CME)
 */
enum class MessageType : uint8_t {
    TRADE = 0x01,
    QUOTE = 0x02,
    ORDER_ADD = 0x03,
    ORDER_DELETE = 0x04,
    ORDER_MODIFY = 0x05,
    HEARTBEAT = 0xFF
};

/**
 * Trade message - typical HFT structure
 * - Packed to minimize size (cache efficiency)
 * - Fixed size for predictable memory access
 * - Natural alignment where possible
 */
struct __attribute__((packed)) TradeMessage {
    uint64_t timestamp_ns;      // Exchange timestamp in nanoseconds
    uint64_t sequence_num;      // Sequence number for gap detection
    uint32_t symbol_id;         // Symbol ID (mapped, not string for speed)
    uint32_t trade_id;          // Unique trade identifier
    uint64_t price;             // Price in fixed point (e.g., *10000 for 4 decimals)
    uint32_t quantity;          // Quantity
    uint8_t  side;              // 'B' or 'S'
    uint8_t  padding[3];        // Explicit padding for alignment
};

/**
 * Quote/Level update message
 */
struct __attribute__((packed)) QuoteMessage {
    uint64_t timestamp_ns;
    uint64_t sequence_num;
    uint32_t symbol_id;
    uint64_t bid_price;
    uint64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint8_t  num_levels;        // For market depth
    uint8_t  padding[7];
};

/**
 * Generic market data packet container
 * In production, you'd have a packet header followed by multiple messages
 */
struct __attribute__((packed)) MarketDataPacket {
    MessageType msg_type;
    uint8_t     version;
    uint16_t    payload_size;
    uint64_t    packet_sequence;
    
    union {
        TradeMessage trade;
        QuoteMessage quote;
        uint8_t raw_data[256];  // Max payload size
    } payload;
};

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
 * Statistics tracker for monitoring feed handler performance
 */
struct FeedHandlerStats {
    alignas(64) std::atomic<uint64_t> packets_received{0};
    alignas(64) std::atomic<uint64_t> packets_processed{0};
    alignas(64) std::atomic<uint64_t> packets_dropped{0};
    alignas(64) std::atomic<uint64_t> sequence_gaps{0};
    alignas(64) std::atomic<uint64_t> total_latency_ns{0};
    
    // Min/max latency tracking (not atomic for simplicity, could add spinlock)
    uint64_t min_latency_ns{UINT64_MAX};
    uint64_t max_latency_ns{0};
    
    void update_latency(uint64_t latency_ns) noexcept {
        total_latency_ns.fetch_add(latency_ns, std::memory_order_relaxed);
        if (latency_ns < min_latency_ns) min_latency_ns = latency_ns;
        if (latency_ns > max_latency_ns) max_latency_ns = latency_ns;
    }
    
    double avg_latency_ns() const noexcept {
        uint64_t processed = packets_processed.load(std::memory_order_relaxed);
        if (processed == 0) return 0.0;
        return static_cast<double>(total_latency_ns.load(std::memory_order_relaxed)) / processed;
    }
};

/**
 * Processed market event after normalization
 * This is what goes into the SPSC queue and consumed by trading logic
 */
struct MarketEvent {
    uint64_t recv_timestamp_ns;     // When we received it (RDTSC)
    uint64_t exchange_timestamp_ns; // Exchange timestamp
    uint32_t symbol_id;
    MessageType type;
    
    // Union for different event types
    union {
        struct {
            uint64_t price;
            uint32_t quantity;
            uint8_t  side;
        } trade;
        
        struct {
            uint64_t bid_price;
            uint64_t ask_price;
            uint32_t bid_size;
            uint32_t ask_size;
        } quote;
    } data;
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

