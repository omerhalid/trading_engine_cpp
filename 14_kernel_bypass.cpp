/**
 * LESSON 14: Kernel Bypass Concepts
 * 
 * Standard network stack (slow):
 * [Application] -> [syscall] -> [Kernel TCP/IP stack] -> [Driver] -> [NIC]
 * Latency: 5-10 microseconds
 * 
 * Kernel bypass (fast):
 * [Application] -> [User-space driver] -> [DMA] -> [NIC]
 * Latency: 200-500 nanoseconds
 * 
 * Technologies:
 * - Solarflare OpenOnload (ef_vi API) - Most popular in HFT
 * - Intel DPDK - Popular for networking
 * - Mellanox/NVIDIA VMA
 * - Exablaze ExaNIC
 * 
 * This lesson explains the concepts (actual implementation requires special NICs)
 */

#include <iostream>
#include <cstdint>
#include <cstring>

// ============================================================================
// CONCEPT: Standard Kernel Socket (what we've been using)
// ============================================================================

class KernelSocket {
public:
    /**
     * Standard receive path:
     * 
     * 1. Packet arrives at NIC
     * 2. NIC generates interrupt
     * 3. Kernel handles interrupt
     * 4. Kernel copies packet to socket buffer
     * 5. Application calls recvfrom() - syscall!
     * 6. Kernel copies data to application buffer
     * 7. Return from syscall
     * 
     * Problems:
     * - 2 context switches (user->kernel->user)
     * - 2 memory copies
     * - Interrupt handling overhead
     * - Kernel scheduler can delay
     * 
     * Total: 5-10 microseconds
     */
    
    void explain() {
        std::cout << "Standard Socket Path:\n";
        std::cout << "  [NIC] -> Interrupt\n";
        std::cout << "     -> [Kernel] copies to socket buffer\n";
        std::cout << "     -> recvfrom() syscall (context switch)\n";
        std::cout << "     -> [Kernel] copies to user buffer\n";
        std::cout << "     -> return to user (context switch)\n";
        std::cout << "  Latency: ~5-10 microseconds\n";
        std::cout << "  Copies: 2 (NIC->kernel, kernel->user)\n";
        std::cout << "  Syscalls: 1 per packet\n\n";
    }
};

// ============================================================================
// CONCEPT: Kernel Bypass (Solarflare ef_vi style)
// ============================================================================

class KernelBypass {
public:
    /**
     * Kernel bypass path:
     * 
     * 1. Packet arrives at NIC
     * 2. NIC DMAs directly to user-space buffer (pre-mapped)
     * 3. Application polls event queue (memory read, no syscall!)
     * 4. Process packet from user-space buffer
     * 
     * Benefits:
     * - Zero context switches
     * - Zero copies (DMA directly to user space)
     * - No interrupts (polling mode)
     * - No kernel involvement
     * 
     * Total: 200-500 nanoseconds
     */
    
    void explain() {
        std::cout << "Kernel Bypass Path:\n";
        std::cout << "  [NIC] -> DMA directly to user-space buffer\n";
        std::cout << "     -> Application polls event queue (memory read)\n";
        std::cout << "     -> Process packet (already in user space)\n";
        std::cout << "  Latency: ~200-500 nanoseconds\n";
        std::cout << "  Copies: 0 (DMA to user space)\n";
        std::cout << "  Syscalls: 0 (pure polling)\n\n";
    }
    
    /**
     * Pseudo-code for Solarflare ef_vi:
     * 
     * // One-time setup
     * ef_vi vi;
     * ef_driver_handle dh;
     * ef_vi_alloc_from_pd(&vi, dh, &pd, ...);
     * 
     * // Register memory for DMA
     * ef_memreg mr;
     * ef_memreg_alloc(&mr, dh, &pd, dh, buffer, buffer_size);
     * 
     * // Hot path - poll for packets (NO SYSCALLS!)
     * while (1) {
     *     ef_event events[32];
     *     int n_ev = ef_eventq_poll(&vi, events, 32);  // Memory read!
     *     
     *     for (int i = 0; i < n_ev; i++) {
     *         if (EF_EVENT_TYPE(events[i]) == EF_EVENT_TYPE_RX) {
     *             // Packet available in pre-mapped buffer
     *             void* pkt = ef_event_rx_ptr(&events[i]);
     *             int len = ef_event_rx_bytes(&events[i]);
     *             
     *             // Process packet (zero copy!)
     *             process_packet(pkt, len);
     *             
     *             // Release buffer back to NIC
     *             ef_vi_receive_post(&vi, ...);
     *         }
     *     }
     * }
     */
};

// ============================================================================
// CONCEPT: Memory-mapped packet buffers
// ============================================================================

struct PacketBuffer {
    // In kernel bypass, NIC DMAs packets here
    // This memory is shared between NIC and application
    alignas(4096) uint8_t data[2048];
    uint16_t length;
    uint16_t flags;
};

class DMABufferPool {
private:
    static constexpr size_t NUM_BUFFERS = 2048;
    
    // Pre-allocated, DMA-capable buffers
    // In real system: registered with NIC for DMA
    alignas(4096) PacketBuffer buffers_[NUM_BUFFERS];
    
    // Event queue (written by NIC, polled by application)
    struct Event {
        uint32_t buffer_id;
        uint16_t length;
        uint16_t flags;
    };
    
    alignas(64) std::atomic<uint64_t> event_write_{0};
    alignas(64) std::atomic<uint64_t> event_read_{0};
    Event events_[1024];
    
public:
    /**
     * Poll for received packets (like ef_eventq_poll)
     * This is a MEMORY READ, not a syscall!
     */
    int poll_events(Event* out_events, int max_events) {
        uint64_t r = event_read_.load(std::memory_order_relaxed);
        uint64_t w = event_write_.load(std::memory_order_acquire);
        
        int count = 0;
        while (r < w && count < max_events) {
            out_events[count++] = events_[r & 1023];
            r++;
        }
        
        event_read_.store(r, std::memory_order_release);
        return count;
    }
    
    /**
     * Get packet buffer for event
     */
    uint8_t* get_packet(const Event& event) {
        return buffers_[event.buffer_id].data;
    }
};

// ============================================================================
// COMPARISON: Show the difference
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

int main() {
    std::cout << "=== KERNEL BYPASS CONCEPTS ===\n\n";
    
    std::cout << "1. Standard Kernel Socket:\n";
    KernelSocket ksock;
    ksock.explain();
    
    std::cout << "2. Kernel Bypass (Solarflare ef_vi):\n";
    KernelBypass bypass;
    bypass.explain();
    
    std::cout << "3. Latency Breakdown:\n\n";
    std::cout << "  Component          Kernel Socket   Kernel Bypass\n";
    std::cout << "  ─────────────────────────────────────────────────\n";
    std::cout << "  NIC -> Memory      1-2 us          200-300 ns (DMA)\n";
    std::cout << "  Interrupt          500-1000 ns     0 (polling)\n";
    std::cout << "  Syscall overhead   300-500 ns      0\n";
    std::cout << "  Memory copy        200-500 ns      0 (zero-copy)\n";
    std::cout << "  ─────────────────────────────────────────────────\n";
    std::cout << "  TOTAL              5-10 us         200-500 ns\n";
    std::cout << "  Speedup:           1x              20-50x faster!\n\n";
    
    std::cout << "4. Memory Access Pattern:\n";
    DMABufferPool pool;
    
    // Simulate polling (this is just memory read!)
    uint64_t start = rdtsc();
    DMABufferPool::Event events[32];
    int n_events = pool.poll_events(events, 32);
    uint64_t end = rdtsc();
    
    std::cout << "  Poll operation: " << (end - start) << " cycles\n";
    std::cout << "  (This is pure memory read - no syscall!)\n\n";
    
    std::cout << "KERNEL BYPASS VENDORS:\n";
    std::cout << "  • Solarflare (Xilinx): ef_vi API\n";
    std::cout << "    - Most popular in HFT\n";
    std::cout << "    - 200-500ns latency\n";
    std::cout << "    - Used by: Citadel, Jump, Jane Street\n\n";
    
    std::cout << "  • Intel DPDK:\n";
    std::cout << "    - More common in networking/NFV\n";
    std::cout << "    - Poll mode drivers (PMD)\n";
    std::cout << "    - Some HFT firms use it\n\n";
    
    std::cout << "  • Mellanox VMA:\n";
    std::cout << "    - LD_PRELOAD injection\n";
    std::cout << "    - Transparent acceleration\n\n";
    
    std::cout << "  • Exablaze ExaNIC:\n";
    std::cout << "    - Ultra-low latency\n";
    std::cout << "    - FPGA-based\n\n";
    
    std::cout << "SETUP REQUIREMENTS:\n";
    std::cout << "  1. Special NIC (Solarflare, Mellanox, etc.)\n";
    std::cout << "  2. Vendor drivers installed\n";
    std::cout << "  3. Memory registration (huge pages)\n";
    std::cout << "  4. Kernel modules loaded\n";
    std::cout << "  5. CPU core isolation\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Kernel bypass = 20-50x faster than sockets\n";
    std::cout << "  • Zero-copy via DMA to user space\n";
    std::cout << "  • Polling instead of interrupts\n";
    std::cout << "  • No syscalls in hot path\n";
    std::cout << "  • Required for sub-microsecond latency\n";
    std::cout << "  • All top HFT firms use kernel bypass\n\n";
    
    std::cout << "NEXT STEPS:\n";
    std::cout << "  • Our production code is kernel-bypass ready\n";
    std::cout << "  • Replace recvfrom() with ef_eventq_poll()\n";
    std::cout << "  • See udp_receiver.hpp comments for integration points\n";
    
    return 0;
}

