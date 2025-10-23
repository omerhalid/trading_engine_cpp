/**
 * LESSON 8: CPU Affinity and Thread Pinning
 * 
 * Why pin threads to specific CPU cores?
 * - Avoid context switches (each switch = 10,000+ cycles)
 * - Keep L1/L2 cache hot (cache misses = 100-300 cycles)
 * - Predictable performance
 * - NUMA awareness (memory on same socket)
 * 
 * Production setup:
 * - Core 0: Feed handler
 * - Core 1: Trading engine  
 * - Core 2: Order gateway
 * - Cores 0-2: isolated from kernel scheduler (isolcpus)
 * - All on same NUMA node
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <cstdint>
#include <chrono>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

// ============================================================================
// CPU Pinning Utilities
// ============================================================================

class CPUAffinity {
public:
    /**
     * Pin calling thread to specific CPU core
     */
    static bool pin_to_core(int core_id) {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        
        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (result == 0) {
            std::cout << "  Thread pinned to core " << core_id << "\n";
            return true;
        } else {
            std::cout << "  Failed to pin to core " << core_id << "\n";
            return false;
        }
#else
        std::cout << "  CPU pinning not supported on this OS (Linux only)\n";
        (void)core_id;
        return false;
#endif
    }
    
    /**
     * Set real-time priority (requires root or CAP_SYS_NICE)
     */
    static bool set_realtime_priority(int priority = 99) {
#ifdef __linux__
        struct sched_param param;
        param.sched_priority = priority;  // 1-99, higher = more priority
        
        int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        if (result == 0) {
            std::cout << "  Real-time priority set: " << priority << "\n";
            return true;
        } else {
            std::cout << "  Failed to set RT priority (need root/CAP_SYS_NICE)\n";
            return false;
        }
#else
        std::cout << "  RT priority not supported on this OS\n";
        (void)priority;
        return false;
#endif
    }
    
    /**
     * Get current CPU core
     */
    static int get_current_core() {
#ifdef __linux__
        return sched_getcpu();
#else
        return -1;
#endif
    }
};

// ============================================================================
// DEMO: Thread migration impact
// ============================================================================

std::atomic<bool> g_running{true};

void worker_unpinned() {
    std::cout << "[Unpinned] Starting...\n";
    
    uint64_t iterations = 0;
    auto start = std::chrono::steady_clock::now();
    
    while (g_running.load() && iterations < 100000000) {
        // Simulate work
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) x++;
        iterations++;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "[Unpinned] Completed: " << iterations << " iterations in " << duration << " ms\n";
}

void worker_pinned(int core) {
    CPUAffinity::pin_to_core(core);
    
    std::cout << "[Pinned to core " << core << "] Starting...\n";
    
    uint64_t iterations = 0;
    auto start = std::chrono::steady_clock::now();
    
    while (g_running.load() && iterations < 100000000) {
        // Simulate work
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) x++;
        iterations++;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "[Pinned to core " << core << "] Completed: " << iterations 
              << " iterations in " << duration << " ms\n";
}

int main() {
    std::cout << "=== CPU AFFINITY & THREAD PINNING ===\n\n";
    
    std::cout << "System info:\n";
    std::cout << "  Hardware concurrency: " << std::thread::hardware_concurrency() << " cores\n";
    std::cout << "  Current core: " << CPUAffinity::get_current_core() << "\n\n";
    
    // Demo 1: Unpinned thread (can migrate between cores)
    std::cout << "1. Unpinned Thread (OS can move between cores):\n";
    std::thread t1(worker_unpinned);
    t1.join();
    
    // Demo 2: Pinned thread (locked to specific core)
    std::cout << "\n2. Pinned Thread (locked to core 0):\n";
    g_running.store(true);
    std::thread t2(worker_pinned, 0);
    t2.join();
    
    std::cout << "\n3. Real-time Priority Test:\n";
    std::cout << "  Attempting to set RT priority...\n";
    CPUAffinity::set_realtime_priority(99);
    
    std::cout << "\nPRODUCTION SETUP:\n";
    std::cout << "  1. Isolate cores from kernel:\n";
    std::cout << "     Add to kernel params: isolcpus=0-3 nohz_full=0-3\n\n";
    
    std::cout << "  2. Pin threads:\n";
    std::cout << "     Core 0: Feed handler (highest priority)\n";
    std::cout << "     Core 1: Trading engine\n";
    std::cout << "     Core 2: Order gateway\n";
    std::cout << "     Core 3+: Non-critical tasks\n\n";
    
    std::cout << "  3. Set capabilities:\n";
    std::cout << "     sudo setcap cap_sys_nice=+ep ./your_program\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Pinning prevents expensive context switches\n";
    std::cout << "  • Keeps L1/L2 cache hot (huge performance win)\n";
    std::cout << "  • RT priority ensures kernel won't preempt\n";
    std::cout << "  • All major HFT firms use isolated cores\n";
    std::cout << "  • Typical improvement: 10-50% latency reduction\n";
    
    return 0;
}

