/**
 * LESSON 13: Shared Memory IPC (Inter-Process Communication)
 * 
 * Why shared memory for HFT?
 * - Fastest IPC method (10-50ns latency)
 * - No kernel involvement (after setup)
 * - Zero-copy (both processes access same memory)
 * - Lock-free queues work across processes
 * 
 * Use cases:
 * - Feed handler -> Multiple trading strategies (separate processes)
 * - Market data recorder (separate process for safety)
 * - Risk checker (separate process, can kill strategies)
 * 
 * Production pattern: SPSC queue in shared memory
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <cstdint>
#include <atomic>
#include <immintrin.h>

// ============================================================================
// Shared Memory Manager
// ============================================================================

class SharedMemory {
private:
    void* ptr_{nullptr};
    size_t size_{0};
    int fd_{-1};
    bool is_creator_{false};
    
public:
    /**
     * Create or open shared memory region
     * 
     * @param name Shared memory name (e.g., "/hft_feed")
     * @param size Size in bytes
     * @param create True to create, false to open existing
     */
    bool init(const char* name, size_t size, bool create) {
        size_ = size;
        is_creator_ = create;
        
        if (create) {
            // Create new shared memory
            fd_ = shm_open(name, O_CREAT | O_RDWR, 0666);
            if (fd_ < 0) {
                std::cerr << "shm_open failed (create)\n";
                return false;
            }
            
            // Set size
            if (ftruncate(fd_, size) < 0) {
                std::cerr << "ftruncate failed\n";
                close(fd_);
                return false;
            }
        } else {
            // Open existing shared memory
            fd_ = shm_open(name, O_RDWR, 0666);
            if (fd_ < 0) {
                std::cerr << "shm_open failed (open)\n";
                return false;
            }
        }
        
        // Map into process address space
        ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr_ == MAP_FAILED) {
            std::cerr << "mmap failed\n";
            close(fd_);
            return false;
        }
        
        // Lock in memory (prevent swapping)
        mlock(ptr_, size);
        
        std::cout << (create ? "[SHM] Created: " : "[SHM] Opened: ") 
                  << name << " (" << size << " bytes)\n";
        return true;
    }
    
    ~SharedMemory() {
        if (ptr_ != nullptr && ptr_ != MAP_FAILED) {
            munlock(ptr_, size_);
            munmap(ptr_, size_);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    
    void* get() { return ptr_; }
    size_t size() const { return size_; }
    
    // Helper: construct object in shared memory
    template<typename T>
    T* construct() {
        if (!ptr_ || size_ < sizeof(T)) return nullptr;
        if (is_creator_) {
            return new (ptr_) T();  // Placement new
        }
        return static_cast<T*>(ptr_);
    }
};

// ============================================================================
// SPSC Queue in Shared Memory (cross-process)
// ============================================================================

template<typename T, size_t Size>
class SharedSPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "Power of 2");
    
private:
    // MUST use std::atomic for cross-process sync
    // Regular variables won't work across processes!
    alignas(64) T buffer_[Size];
    alignas(64) std::atomic<uint64_t> write_pos_;
    alignas(64) std::atomic<uint64_t> read_pos_;
    static constexpr size_t MASK = Size - 1;
    
public:
    // Constructor must be called in shared memory
    SharedSPSCQueue() : write_pos_(0), read_pos_(0) {}
    
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
    
    size_t size() const noexcept {
        return write_pos_.load(std::memory_order_acquire) - 
               read_pos_.load(std::memory_order_acquire);
    }
};

// ============================================================================
// Market Event (shared between processes)
// ============================================================================

struct MarketEvent {
    uint64_t timestamp;
    uint32_t symbol_id;
    uint64_t price;
    uint32_t quantity;
    uint8_t side;
};

// ============================================================================
// DEMO: Producer Process
// ============================================================================

void run_producer() {
    std::cout << "=== PRODUCER PROCESS ===\n";
    
    // Create shared memory
    SharedMemory shm;
    if (!shm.init("/hft_demo", sizeof(SharedSPSCQueue<MarketEvent, 1024>), true)) {
        return;
    }
    
    // Construct queue in shared memory
    auto* queue = shm.construct<SharedSPSCQueue<MarketEvent, 1024>>();
    
    std::cout << "[Producer] Sending 10 events...\n";
    
    for (int i = 0; i < 10; ++i) {
        MarketEvent event;
        event.timestamp = i;
        event.symbol_id = 12345;
        event.price = 1500000 + i * 100;
        event.quantity = 100;
        event.side = (i % 2) ? 'B' : 'S';
        
        while (!queue->try_push(event)) {
            _mm_pause();  // Spin until space available
        }
        
        std::cout << "[Producer] Sent event " << i << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "[Producer] Complete\n";
}

// ============================================================================
// DEMO: Consumer Process
// ============================================================================

void run_consumer() {
    std::cout << "=== CONSUMER PROCESS ===\n";
    
    // Open existing shared memory
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Wait for creator
    
    SharedMemory shm;
    if (!shm.init("/hft_demo", sizeof(SharedSPSCQueue<MarketEvent, 1024>), false)) {
        return;
    }
    
    // Get queue from shared memory
    auto* queue = shm.construct<SharedSPSCQueue<MarketEvent, 1024>>();
    
    std::cout << "[Consumer] Waiting for events...\n";
    
    int received = 0;
    while (received < 10) {
        MarketEvent event;
        if (queue->try_pop(event)) {
            std::cout << "[Consumer] Received: seq=" << event.timestamp 
                      << " price=$" << (event.price / 10000.0) << "\n";
            received++;
        } else {
            _mm_pause();  // Spin wait
        }
    }
    
    std::cout << "[Consumer] Complete\n";
}

// ============================================================================
// Main: Choose role
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "=== SHARED MEMORY IPC ===\n\n";
    
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  Terminal 1: " << argv[0] << " producer\n";
        std::cout << "  Terminal 2: " << argv[0] << " consumer\n\n";
        
        std::cout << "SHARED MEMORY BENEFITS:\n";
        std::cout << "  • Fastest IPC: 10-50ns latency\n";
        std::cout << "  • Zero-copy: both processes access same memory\n";
        std::cout << "  • No kernel after setup: pure user-space\n";
        std::cout << "  • Lock-free queues work across processes\n\n";
        
        std::cout << "PRODUCTION USE CASES:\n";
        std::cout << "  1. Feed handler -> Multiple strategies (isolation)\n";
        std::cout << "  2. Market data recorder (separate process for safety)\n";
        std::cout << "  3. Risk checker (can kill strategy process)\n";
        std::cout << "  4. Separate order gateway (network isolation)\n\n";
        
        std::cout << "KEY LEARNINGS:\n";
        std::cout << "  • shm_open() creates named shared memory\n";
        std::cout << "  • mmap() maps into process address space\n";
        std::cout << "  • std::atomic works across processes\n";
        std::cout << "  • SPSC queue in shared memory = ultra-fast IPC\n";
        std::cout << "  • mlock() prevents swapping (critical!)\n";
        
        return 1;
    }
    
    std::string mode = argv[1];
    if (mode == "producer") {
        run_producer();
    } else if (mode == "consumer") {
        run_consumer();
    }
    
    // Cleanup
    if (mode == "producer") {
        shm_unlink("/hft_demo");
    }
    
    return 0;
}

