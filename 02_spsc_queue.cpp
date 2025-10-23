/**
 * LESSON 2: Lock-Free SPSC Queue
 * 
 * Learn the core data structure used in HFT for inter-thread communication:
 * - Single Producer Single Consumer (SPSC)
 * - Lock-free (no mutexes, no blocking)
 * - Ring buffer (fixed size, pre-allocated)
 * - Cache-line optimized
 * 
 * Used between: Feed Handler -> Trading Engine -> Order Gateway
 */

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <immintrin.h>

// ============================================================================
// SIMPLE SPSC QUEUE - Educational version with comments
// ============================================================================

template<typename T, size_t SIZE>
class SPSCQueue
{
    private:
        alignas(64) T buffer[SIZE];
        alignas(64) std::atomic<size_t> head_;
        alignas(64) std::atomic<size_t> tail_;
    
    public:
        
        bool try_push(const T& value)
        {
            size_t current_head = head_.load(std::memory_order_relaxed);
            size_t next_head    = (current_head + 1) % SIZE;

            if(next_head == tail_.load(std::memory_order_acquire))
            {
                return false; // full
            }

            buffer[current_head] = value;
            head_.store(next_head, std::memory_order_release);

            return true;
        }

        std::optional<T> try_pop()
        {
            size_t current_tail = tail_.load(std::memory_order_relaxed);
            size_t next_tail    = (current_tail + 1) % SIZE;

            if(current_tail == head_.load(std::memory_order_acquire))
            {
                return std::nullopt;
            }

            T value = buffer[current_tail];
            tail_.store(next_tail, std::memory_order_realase);

            return value;
        }

        size_t size()
        {
            return (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire) + SIZE) % SIZE;  
        }
};

// ============================================================================
// OPTIMIZED SPSC QUEUE - Production version with caching
// ============================================================================

template<typename T, size_t Size>
class OptimizedSPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    
private:
    alignas(64) T buffer_[Size];
    static constexpr size_t MASK = Size - 1;
    
    // Actual positions (updated by each thread)
    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    
    // OPTIMIZATION: Cached positions to reduce atomic loads
    // Producer caches last known read position
    alignas(64) uint64_t cached_read_pos_{0};
    // Consumer caches last known write position
    alignas(64) uint64_t cached_write_pos_{0};
    
public:
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const uint64_t current_write = write_pos_.load(std::memory_order_relaxed);
        const uint64_t next_write = current_write + 1;
        
        // Use cached read position first (avoids expensive atomic load)
        if (next_write - cached_read_pos_ > Size) {
            // Cache stale, reload actual read position
            cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
            if (next_write - cached_read_pos_ > Size) {
                return false;
            }
        }
        
        buffer_[current_write & MASK] = item;
        write_pos_.store(next_write, std::memory_order_release);
        return true;
    }
    
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const uint64_t current_read = read_pos_.load(std::memory_order_relaxed);
        
        // Use cached write position first
        if (current_read >= cached_write_pos_) {
            cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
            if (current_read >= cached_write_pos_) {
                return false;
            }
        }
        
        item = buffer_[current_read & MASK];
        read_pos_.store(current_read + 1, std::memory_order_release);
        return true;
    }
};

// ============================================================================
// BENCHMARK: Compare simple vs optimized
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

template<typename Queue>
void benchmark(const char* name) {
    Queue queue;
    constexpr int ITERATIONS = 1000000;
    
    std::atomic<bool> ready{false};
    
    // Producer thread
    std::thread producer([&]() {
        while (!ready.load()) _mm_pause();
        
        uint64_t start = rdtsc();
        for (int i = 0; i < ITERATIONS; ++i) {
            while (!queue.try_push(i)) _mm_pause();
        }
        uint64_t end = rdtsc();
        std::cout << "  Producer: " << ((end - start) / ITERATIONS) << " cycles/op\n";
    });
    
    // Consumer thread
    std::thread consumer([&]() {
        while (!ready.load()) _mm_pause();
        
        uint64_t start = rdtsc();
        int value;
        int count = 0;
        while (count < ITERATIONS) {
            if (queue.try_pop(value)) count++;
            else _mm_pause();
        }
        uint64_t end = rdtsc();
        std::cout << "  Consumer: " << ((end - start) / ITERATIONS) << " cycles/op\n";
    });
    
    ready.store(true);
    producer.join();
    consumer.join();
}

int main() {
    std::cout << "=== SPSC QUEUE COMPARISON ===\n\n";
    
    std::cout << "Simple SPSC Queue (1M operations):\n";
    benchmark<SimpleSPSCQueue<int, 1024>>("Simple");
    
    std::cout << "\nOptimized SPSC Queue (1M operations):\n";
    benchmark<OptimizedSPSCQueue<int, 1024>>("Optimized");
    
    std::cout << "\nKEY LEARNINGS:\n";
    std::cout << "  • Lock-free = no mutexes = predictable latency\n";
    std::cout << "  • Power-of-2 size = fast modulo (bitwise AND)\n";
    std::cout << "  • Cache-line alignment = no false sharing\n";
    std::cout << "  • Cached positions = fewer atomic loads\n";
    std::cout << "  • Typical latency: 10-20 cycles (~3-6 ns)\n";
    
    return 0;
}

