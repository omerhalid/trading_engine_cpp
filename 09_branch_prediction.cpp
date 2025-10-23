/**
 * LESSON 9: Branch Prediction & Hot Path Optimization
 * 
 * CPU branch predictor:
 * - Correct prediction: 0 cycles
 * - Misprediction: 10-20 cycles penalty (pipeline flush)
 * 
 * In HFT, we optimize hot paths:
 * - Use LIKELY/UNLIKELY hints
 * - Structure code to favor fast path
 * - Minimize branches in critical sections
 * - Profile with perf to find mispredictions
 * 
 * Every cycle counts when processing millions of packets/sec
 */

#include <cstdint>
#include <iostream>
#include <cstring>
#include <immintrin.h>

// ============================================================================
// Branch Prediction Hints
// ============================================================================

// Tell compiler which branch is more likely
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// ============================================================================
// EXAMPLE: Packet validation
// ============================================================================

struct Packet {
    uint32_t magic;      // Should always be 0xDEADBEEF
    uint32_t size;
    uint64_t sequence;
    uint8_t  data[128];
};

// Without hints
bool validate_packet_bad(const Packet* pkt) {
    // Most packets are valid, but compiler doesn't know this
    if (pkt == nullptr) return false;
    if (pkt->magic != 0xDEADBEEF) return false;
    if (pkt->size > sizeof(pkt->data)) return false;
    return true;
}

// With hints - optimize for success path
bool validate_packet_good(const Packet* pkt) {
    // Tell compiler: these errors are UNLIKELY
    if (UNLIKELY(pkt == nullptr)) return false;
    if (UNLIKELY(pkt->magic != 0xDEADBEEF)) return false;
    if (UNLIKELY(pkt->size > sizeof(pkt->data))) return false;
    return true;  // LIKELY path - no branches!
}

// ============================================================================
// EXAMPLE: Fast path vs slow path
// ============================================================================

class OrderProcessor {
public:
    // BAD: Equally weighted branches
    void process_order_bad(int order_type) {
        if (order_type == 1) {
            // Market order (95% of orders)
            handle_market_order();
        } else if (order_type == 2) {
            // Limit order (4% of orders)
            handle_limit_order();
        } else {
            // Stop order (1% of orders)
            handle_stop_order();
        }
    }
    
    // GOOD: Optimize for common case
    void process_order_good(int order_type) {
        if (LIKELY(order_type == 1)) {
            // Hot path - market order (95%)
            handle_market_order();
            return;
        }
        
        // Cold path - less common orders
        if (order_type == 2) {
            handle_limit_order();
        } else {
            handle_stop_order();
        }
    }
    
private:
    void handle_market_order() { /* implementation */ }
    void handle_limit_order() { /* implementation */ }
    void handle_stop_order() { /* implementation */ }
};

// ============================================================================
// EXAMPLE: Minimize branches in hot loop
// ============================================================================

// BAD: Branch in every iteration
uint64_t sum_array_bad(const uint64_t* arr, size_t size, bool skip_zeros) {
    uint64_t sum = 0;
    for (size_t i = 0; i < size; ++i) {
        if (skip_zeros && arr[i] == 0) {  // Branch every iteration!
            continue;
        }
        sum += arr[i];
    }
    return sum;
}

// GOOD: Hoist branch out of loop
uint64_t sum_array_good(const uint64_t* arr, size_t size, bool skip_zeros) {
    if (LIKELY(!skip_zeros)) {
        // Hot path - no branch in loop
        uint64_t sum = 0;
        for (size_t i = 0; i < size; ++i) {
            sum += arr[i];
        }
        return sum;
    } else {
        // Cold path - has branch
        uint64_t sum = 0;
        for (size_t i = 0; i < size; ++i) {
            if (arr[i] != 0) sum += arr[i];
        }
        return sum;
    }
}

// ============================================================================
// BENCHMARK: Measure branch misprediction impact
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void benchmark_validation() {
    constexpr int ITERATIONS = 1000000;
    
    Packet valid_pkt;
    valid_pkt.magic = 0xDEADBEEF;
    valid_pkt.size = 64;
    valid_pkt.sequence = 123;
    
    // Benchmark without hints
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        volatile bool result = validate_packet_bad(&valid_pkt);
    }
    uint64_t end = rdtsc();
    std::cout << "  Without hints: " << ((end - start) / ITERATIONS) << " cycles/validation\n";
    
    // Benchmark with hints
    start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        volatile bool result = validate_packet_good(&valid_pkt);
    }
    end = rdtsc();
    std::cout << "  With UNLIKELY hints: " << ((end - start) / ITERATIONS) << " cycles/validation\n";
}

void benchmark_array_sum() {
    constexpr size_t SIZE = 10000;
    uint64_t arr[SIZE];
    for (size_t i = 0; i < SIZE; ++i) arr[i] = i;
    
    constexpr int ITERATIONS = 10000;
    
    // Bad version
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        volatile uint64_t sum = sum_array_bad(arr, SIZE, false);
    }
    uint64_t end = rdtsc();
    std::cout << "  Branch in loop: " << ((end - start) / ITERATIONS) << " cycles/sum\n";
    
    // Good version
    start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        volatile uint64_t sum = sum_array_good(arr, SIZE, false);
    }
    end = rdtsc();
    std::cout << "  Branch hoisted: " << ((end - start) / ITERATIONS) << " cycles/sum\n";
}

int main() {
    std::cout << "=== BRANCH PREDICTION & OPTIMIZATION ===\n\n";
    
    std::cout << "1. Validation with LIKELY/UNLIKELY:\n";
    benchmark_validation();
    
    std::cout << "\n2. Loop Branch Hoisting:\n";
    benchmark_array_sum();
    
    std::cout << "\n3. CPU Information:\n";
    std::cout << "  Hardware threads: " << std::thread::hardware_concurrency() << "\n";
#ifdef __linux__
    std::cout << "  Current CPU: " << sched_getcpu() << "\n";
#endif
    
    std::cout << "\nOPTIMIZATION TECHNIQUES:\n";
    std::cout << "  1. Use LIKELY/UNLIKELY for error paths\n";
    std::cout << "  2. Structure code with fast path first\n";
    std::cout << "  3. Hoist branches out of hot loops\n";
    std::cout << "  4. Use lookup tables instead of if/else chains\n";
    std::cout << "  5. Profile with: perf stat -e branch-misses ./program\n\n";
    
    std::cout << "BRANCH MISPREDICTION COST:\n";
    std::cout << "  • Correct prediction: 0 cycles\n";
    std::cout << "  • Misprediction: 10-20 cycles (pipeline flush)\n";
    std::cout << "  • At 1M packets/sec: 1% misprediction = 10-20us wasted!\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • CPU predicts branches based on history\n";
    std::cout << "  • Help compiler with LIKELY/UNLIKELY\n";
    std::cout << "  • Structure code for predictable branches\n";
    std::cout << "  • Profile to find hotspots\n";
    std::cout << "  • Every branch misprediction costs 10-20 cycles\n";
    
    return 0;
}

