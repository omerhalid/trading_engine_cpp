/**
 * LESSON 11: Asynchronous Logging
 * 
 * Why NOT use printf/cout in hot path?
 * - Disk I/O: 1000-10000+ microseconds
 * - Kernel syscall: 100-1000 ns
 * - Locks in stdio: unpredictable
 * - Formatting: 50-200 ns
 * 
 * Solution: Async logging
 * - Hot path: Push message to lock-free queue (~20ns)
 * - Background thread: Drain queue to disk
 * - No blocking, no syscalls in hot path
 * 
 * All HFT firms use async logging (or no logging in hot path at all)
 */

#include <atomic>
#include <thread>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <fstream>
#include <immintrin.h>

// ============================================================================
// Minimal SPSC Queue for log messages
// ============================================================================

template<typename T, size_t Size>
class LogQueue {
    static_assert((Size & (Size - 1)) == 0, "Power of 2");
private:
    alignas(64) T buffer_[Size];
    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    static constexpr size_t MASK = Size - 1;
    
public:
    bool try_push(const T& item) noexcept {
        uint64_t w = write_pos_.load(std::memory_order_relaxed);
        uint64_t r = read_pos_.load(std::memory_order_acquire);
        if (w - r >= Size) return false;
        buffer_[w & MASK] = item;
        write_pos_.store(w + 1, std::memory_order_release);
        return true;
    }
    
    bool try_pop(T& item) noexcept {
        uint64_t r = read_pos_.load(std::memory_order_relaxed);
        uint64_t w = write_pos_.load(std::memory_order_acquire);
        if (r >= w) return false;
        item = buffer_[r & MASK];
        read_pos_.store(r + 1, std::memory_order_release);
        return true;
    }
    
    bool empty() const noexcept {
        return read_pos_.load(std::memory_order_acquire) >= 
               write_pos_.load(std::memory_order_acquire);
    }
};

// ============================================================================
// Log Entry (fixed size - no dynamic allocation)
// ============================================================================

struct LogEntry {
    uint64_t timestamp_ns;
    char message[256];  // Fixed size
    
    LogEntry() : timestamp_ns(0) { message[0] = '\0'; }
};

// ============================================================================
// Simple Async Logger
// ============================================================================

class AsyncLogger {
private:
    LogQueue<LogEntry, 8192> queue_;
    std::thread io_thread_;
    std::atomic<bool> running_{true};
    std::atomic<uint64_t> messages_logged_{0};
    std::atomic<uint64_t> messages_dropped_{0};
    
public:
    AsyncLogger() {
        // Start I/O thread
        io_thread_ = std::thread([this]() {
            LogEntry entry;
            while (running_.load()) {
                if (queue_.try_pop(entry)) {
                    // Write to stdout (in production: write to file)
                    std::cout << "[LOG] " << entry.message << "\n";
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            // Drain remaining
            while (queue_.try_pop(entry)) {
                std::cout << "[LOG] " << entry.message << "\n";
            }
        });
    }
    
    ~AsyncLogger() {
        running_.store(false);
        if (io_thread_.joinable()) io_thread_.join();
    }
    
    // HOT PATH: Non-blocking log
    void log(const char* msg) noexcept {
        LogEntry entry;
        entry.timestamp_ns = get_time_ns();
        
        size_t len = strlen(msg);
        size_t copy_len = std::min(len, sizeof(entry.message) - 1);
        memcpy(entry.message, msg, copy_len);
        entry.message[copy_len] = '\0';
        
        if (queue_.try_push(entry)) {
            messages_logged_.fetch_add(1, std::memory_order_relaxed);
        } else {
            messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    void print_stats() const {
        std::cout << "\nLogger Stats:\n";
        std::cout << "  Logged: " << messages_logged_.load() << "\n";
        std::cout << "  Dropped: " << messages_dropped_.load() << "\n";
    }
    
private:
    static uint64_t get_time_ns() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }
};

// ============================================================================
// BENCHMARK: Blocking vs Async logging
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void benchmark_blocking_log() {
    constexpr int ITERATIONS = 1000;
    
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        // Blocking I/O - VERY SLOW
        // std::cout << "Log message " << i << "\n";  // Commented to avoid spam
        
        // Simulate the syscall overhead
        std::ofstream file("/dev/null", std::ios::app);
        file << "Log message " << i << "\n";
    }
    uint64_t end = rdtsc();
    
    std::cout << "  Blocking (cout/file): " << ((end - start) / ITERATIONS) 
              << " cycles/log (~" << ((end - start) / ITERATIONS / 3000) << " us)\n";
}

void benchmark_async_log() {
    AsyncLogger logger;
    constexpr int ITERATIONS = 1000;
    
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        logger.log("Log message");  // Non-blocking!
    }
    uint64_t end = rdtsc();
    
    std::cout << "  Async (lock-free queue): " << ((end - start) / ITERATIONS) 
              << " cycles/log (~" << ((end - start) / ITERATIONS / 3) << " ns)\n";
    
    // Wait for I/O thread to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int main() {
    std::cout << "=== ASYNCHRONOUS LOGGING ===\n\n";
    
    std::cout << "1. Logging Performance (1000 messages):\n";
    benchmark_blocking_log();
    
    std::cout << "\n2. Async Logging:\n";
    benchmark_async_log();
    
    std::cout << "\nLOGGING STRATEGIES IN HFT:\n";
    std::cout << "  • Hot path: NO logging (or async only)\n";
    std::cout << "  • Cold path: Can use blocking logs\n";
    std::cout << "  • Errors: Async log + increment counter\n";
    std::cout << "  • Startup/shutdown: Blocking logs OK\n";
    std::cout << "  • Production: Ring buffer to shared memory\n\n";
    
    std::cout << "LATENCY COMPARISON:\n";
    std::cout << "  • printf/cout: 10,000-100,000 cycles (disk I/O)\n";
    std::cout << "  • Async log (queue push): 20-50 cycles\n";
    std::cout << "  • No logging: 0 cycles (best for hot path)\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Never block for I/O in hot path\n";
    std::cout << "  • Use lock-free queue to I/O thread\n";
    std::cout << "  • Fixed-size messages (no malloc)\n";
    std::cout << "  • Graceful degradation (drop if queue full)\n";
    std::cout << "  • Many HFT systems log nothing in hot path\n";
    
    return 0;
}

