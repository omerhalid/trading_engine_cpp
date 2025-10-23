/**
 * LESSON 1: Low-Latency Basics
 * 
 * Learn the fundamental building blocks of HFT systems:
 * - RDTSC timestamping (sub-nanosecond precision)
 * - Cache line alignment (prevent false sharing)
 * - Memory ordering (atomic operations)
 * - Spin waiting (avoid context switches)
 */

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <immintrin.h>

// ============================================================================
// CONCEPT 1: RDTSC - Fastest way to measure time on x86
// ============================================================================

class Timer {
public:
    // Read Time-Stamp Counter - reads CPU cycle counter directly
    // ~10 cycles latency vs ~1000+ cycles for clock_gettime()
    static inline uint64_t rdtsc() noexcept {
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }
    
    // Serializing version - ensures all prior instructions complete
    // Use when you need exact "after operation" timestamp
    static inline uint64_t rdtscp() noexcept {
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }
    
    // Convert cycles to nanoseconds (calibrate on your CPU)
    static inline uint64_t cycles_to_ns(uint64_t cycles, double cpu_ghz = 3.0) noexcept {
        return static_cast<uint64_t>(cycles / cpu_ghz);
    }
};

// ============================================================================
// CONCEPT 2: Cache Line Alignment - Prevent False Sharing
// ============================================================================

// Modern CPUs load memory in 64-byte cache lines
// If two threads access different variables on same cache line,
// they fight over the cache line = "false sharing" = SLOW

// BAD: Both variables share same cache line
struct BadCounters {
    std::atomic<uint64_t> producer_count{0};  // Thread 1 writes here
    std::atomic<uint64_t> consumer_count{0};  // Thread 2 writes here
    // ^^^ These are adjacent = same cache line = false sharing!
};

// GOOD: Each variable on separate cache line
struct GoodCounters {
    alignas(64) std::atomic<uint64_t> producer_count{0};  // Own cache line
    alignas(64) std::atomic<uint64_t> consumer_count{0};  // Own cache line
    // ^^^ 64-byte aligned = no false sharing = FAST
};

// ============================================================================
// CONCEPT 3: Memory Ordering - Control CPU reordering
// ============================================================================

class MemoryOrderingExample {
private:
    alignas(64) std::atomic<int> data{0};
    alignas(64) std::atomic<bool> ready{false};
    
public:
    // Producer: Write data then signal ready
    void produce(int value) {
        data.store(value, std::memory_order_relaxed);  // Can reorder
        ready.store(true, std::memory_order_release);  // Barrier: all prior writes visible
    }
    
    // Consumer: Wait for ready then read data
    int consume() {
        while (!ready.load(std::memory_order_acquire)) {  // Barrier: see all prior writes
            _mm_pause();  // Tell CPU we're spinning
        }
        return data.load(std::memory_order_relaxed);
    }
};

// ============================================================================
// CONCEPT 4: Spin Waiting - Never block in hot path
// ============================================================================

class SpinWait {
public:
    // WRONG: Blocks thread, context switch (~10,000 ns overhead)
    static void bad_wait() {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    
    // RIGHT: Busy wait, stays on CPU, no context switch
    static void good_wait() {
        _mm_pause();  // x86 PAUSE instruction:
                      // - Reduces power consumption
                      // - Improves performance on hyperthreading
                      // - Signals to CPU we're in spin loop
    }
};

// ============================================================================
// DEMONSTRATION: Measure latency of different approaches
// ============================================================================

int main() {
    std::cout << "=== LOW-LATENCY HFT BASICS ===\n\n";
    
    // Demo 1: RDTSC timing
    std::cout << "1. RDTSC Timing:\n";
    uint64_t start = Timer::rdtsc();
    volatile int x = 0;  // Prevent optimization
    for (int i = 0; i < 1000; ++i) x++;
    uint64_t end = Timer::rdtscp();
    std::cout << "   1000 increments: " << (end - start) << " cycles\n";
    std::cout << "   (~" << Timer::cycles_to_ns(end - start) << " ns)\n\n";
    
    // Demo 2: False sharing impact
    std::cout << "2. Cache Line Alignment:\n";
    std::cout << "   BadCounters size: " << sizeof(BadCounters) << " bytes\n";
    std::cout << "   GoodCounters size: " << sizeof(GoodCounters) << " bytes\n";
    std::cout << "   (GoodCounters is larger but MUCH faster)\n\n";
    
    // Demo 3: Memory ordering
    std::cout << "3. Memory Ordering:\n";
    MemoryOrderingExample example;
    std::thread producer([&]() {
        example.produce(42);
    });
    std::thread consumer([&]() {
        int value = example.consume();
        std::cout << "   Consumed value: " << value << "\n";
    });
    producer.join();
    consumer.join();
    std::cout << "   (acquire/release ensures correct ordering)\n\n";
    
    // Demo 4: Spin wait
    std::cout << "4. Spin Waiting:\n";
    start = Timer::rdtsc();
    SpinWait::good_wait();
    end = Timer::rdtscp();
    std::cout << "   _mm_pause(): " << (end - start) << " cycles\n";
    std::cout << "   (vs ~30,000 cycles for context switch)\n\n";
    
    std::cout << "KEY TAKEAWAYS:\n";
    std::cout << "  • RDTSC for sub-nanosecond timing\n";
    std::cout << "  • Cache-line align to prevent false sharing\n";
    std::cout << "  • Use memory_order_acquire/release for synchronization\n";
    std::cout << "  • Spin-wait instead of blocking\n";
    
    return 0;
}

