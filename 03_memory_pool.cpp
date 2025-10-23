/**
 * LESSON 3: Lock-Free Memory Pool
 * 
 * Why malloc() is BAD in HFT:
 * - Unpredictable latency (50-100ns, can spike to microseconds)
 * - Lock contention in allocator
 * - Fragmentation
 * - System calls
 * 
 * Solution: Pre-allocate everything, use lock-free free list
 * - Predictable latency (5-10ns)
 * - No locks
 * - No fragmentation
 * - No system calls
 */

#include <atomic>
#include <cstdint>
#include <iostream>
#include <cstring>
#include <immintrin.h>

// ============================================================================
// SIMPLE MEMORY POOL - Educational version
// ============================================================================

template<typename T, size_t PoolSize>
class SimpleMemoryPool {
private:
    // Free list node - embedded in unused memory slots
    struct FreeNode {
        FreeNode* next;
    };
    
    // Pre-allocated memory block
    alignas(64) uint8_t memory_[sizeof(T) * PoolSize];
    
    // Lock-free free list head
    alignas(64) std::atomic<FreeNode*> free_list_{nullptr};
    
public:
    SimpleMemoryPool() {
        // Initialize: link all blocks into free list
        FreeNode* head = nullptr;
        for (size_t i = PoolSize; i > 0; --i) {
            FreeNode* node = reinterpret_cast<FreeNode*>(
                memory_ + ((i - 1) * sizeof(T))
            );
            node->next = head;
            head = node;
        }
        free_list_.store(head, std::memory_order_release);
    }
    
    // Allocate: pop from free list (lock-free)
    [[nodiscard]] void* allocate() noexcept {
        FreeNode* old_head = free_list_.load(std::memory_order_acquire);
        
        // Try to pop from free list using CAS (Compare-And-Swap)
        while (old_head != nullptr) {
            FreeNode* new_head = old_head->next;
            
            // If CAS succeeds, we got the node
            if (free_list_.compare_exchange_weak(
                old_head, new_head,
                std::memory_order_release,
                std::memory_order_acquire)) {
                return static_cast<void*>(old_head);
            }
            // CAS failed (another thread changed it), retry
        }
        
        return nullptr;  // Pool exhausted
    }
    
    // Deallocate: push to free list (lock-free)
    void deallocate(void* ptr) noexcept {
        if (!ptr) return;
        
        FreeNode* node = static_cast<FreeNode*>(ptr);
        FreeNode* old_head = free_list_.load(std::memory_order_acquire);
        
        // Try to push to free list using CAS
        do {
            node->next = old_head;
        } while (!free_list_.compare_exchange_weak(
            old_head, node,
            std::memory_order_release,
            std::memory_order_acquire));
    }
    
    // Construct object in-place (placement new)
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) noexcept {
        void* ptr = allocate();
        if (!ptr) return nullptr;
        return new (ptr) T(std::forward<Args>(args)...);
    }
    
    // Destroy and deallocate
    void destroy(T* ptr) noexcept {
        if (!ptr) return;
        ptr->~T();  // Call destructor
        deallocate(ptr);
    }
};

// ============================================================================
// EXAMPLE: Market Data Event
// ============================================================================

struct MarketEvent {
    uint64_t timestamp;
    uint32_t symbol_id;
    uint64_t price;
    uint32_t quantity;
    
    MarketEvent() = default;
    MarketEvent(uint64_t ts, uint32_t sym, uint64_t p, uint32_t q)
        : timestamp(ts), symbol_id(sym), price(p), quantity(q) {}
};

// ============================================================================
// BENCHMARK: Memory pool vs malloc
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void benchmark_malloc() {
    constexpr int ITERATIONS = 10000;
    uint64_t total_cycles = 0;
    
    for (int i = 0; i < ITERATIONS; ++i) {
        uint64_t start = rdtsc();
        
        MarketEvent* event = new MarketEvent(123, 456, 789, 100);
        volatile uint64_t x = event->timestamp;  // Prevent optimization
        delete event;
        
        uint64_t end = rdtsc();
        total_cycles += (end - start);
    }
    
    std::cout << "malloc/delete: " << (total_cycles / ITERATIONS) << " cycles/op\n";
}

void benchmark_pool() {
    constexpr int ITERATIONS = 10000;
    SimpleMemoryPool<MarketEvent, 1024> pool;
    uint64_t total_cycles = 0;
    
    for (int i = 0; i < ITERATIONS; ++i) {
        uint64_t start = rdtsc();
        
        MarketEvent* event = pool.construct(123, 456, 789, 100);
        volatile uint64_t x = event->timestamp;  // Prevent optimization
        pool.destroy(event);
        
        uint64_t end = rdtsc();
        total_cycles += (end - start);
    }
    
    std::cout << "pool alloc/free: " << (total_cycles / ITERATIONS) << " cycles/op\n";
}

int main() {
    std::cout << "=== MEMORY POOL vs MALLOC ===\n\n";
    
    std::cout << "Benchmark (10,000 allocations):\n";
    benchmark_malloc();
    benchmark_pool();
    
    std::cout << "\nTypical results:\n";
    std::cout << "  malloc/delete: 150-300 cycles (~50-100 ns)\n";
    std::cout << "  pool alloc/free: 15-30 cycles (~5-10 ns)\n";
    std::cout << "  Speedup: 10-20x faster!\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Pre-allocate memory at startup\n";
    std::cout << "  • Lock-free free list (CAS operations)\n";
    std::cout << "  • No fragmentation (fixed size blocks)\n";
    std::cout << "  • Predictable latency (critical for HFT)\n";
    std::cout << "  • Used for: orders, events, messages\n";
    
    return 0;
}

