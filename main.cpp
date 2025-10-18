#include "spsc_queue.hpp"
#include "types.hpp"
#include "logger.hpp"
#include "feed_handler_impl.hpp"
#include "trading_engine.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <signal.h>

using namespace hft;

/**
 * Global state for graceful shutdown
 */
std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    (void)signum;
    g_running.store(false, std::memory_order_release);
}

/**
 * TICK-TO-TRADE FEED HANDLER
 * 
 * Architecture:
 * 
 * [NIC] -> [Feed Handler Thread] -> [SPSC Queue] -> [Trading Thread] -> [Order Gateway]
 *           (Core 0, RT priority)                    (Core 1, RT priority)
 * 
 * Feed Handler Thread:
 * - Receives UDP multicast market data
 * - Parses and normalizes messages
 * - Pushes to lock-free queue
 * - Busy polls (no blocking)
 * 
 * Trading Thread:
 * - Consumes from lock-free queue
 * - Runs trading logic
 * - Generates orders
 * 
 * Latency breakdown (typical HFT):
 * - NIC to user space: 200-500ns (kernel bypass)
 * - Parsing: 50-100ns
 * - Queue push: 10-20ns
 * - Queue pop: 10-20ns
 * - Trading logic: 100-500ns
 * - Order send: 200-500ns
 * Total: ~1-2 microseconds tick-to-trade
 */

/**
 * Main function - sets up the tick-to-trade pipeline
 */
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║         HFT TICK-TO-TRADE FEED HANDLER                      ║
║         Lock-Free SPSC | Kernel Bypass UDP                  ║
║         Memory Pool | Async Logger                          ║
╚══════════════════════════════════════════════════════════════╝
    )" << std::endl;
    
    // Configuration
    // In production: read from config file
    const std::string MULTICAST_IP = "233.54.12.1";
    const uint16_t PORT = 15000;
    const int FEED_HANDLER_CORE = 0;
    const int TRADING_ENGINE_CORE = 1;
    const bool USE_HUGE_PAGES = false;  // Set to true if huge pages configured
    
    // Initialize logger
    Logger::initialize("hft_system.log", LogLevel::INFO);
    LOG_INFO("=== HFT System Starting ===");
    
    // Setup signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create shared components
    SPSCQueue<MarketEvent, 65536> event_queue;
    FeedHandlerStats stats;
    
    // Create feed handler and trading engine
    FeedHandler feed_handler(event_queue, stats, FEED_HANDLER_CORE, USE_HUGE_PAGES);
    TradingEngine trading_engine(event_queue, TRADING_ENGINE_CORE);
    
    // Initialize UDP receiver
    std::cout << "[Main] Initializing UDP receiver..." << std::endl;
    LOG_INFO("Initializing UDP receiver");
    
    if (!feed_handler.init(MULTICAST_IP, PORT)) {
        std::cerr << "[Main] Failed to initialize UDP receiver" << std::endl;
        LOG_ERROR("Failed to initialize UDP receiver");
        Logger::shutdown();
        return 1;
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Listening on %s:%d", MULTICAST_IP.c_str(), PORT);
    LOG_INFO(msg);
    std::cout << "[Main] " << msg << std::endl;
    
    // Launch threads
    // In production: consider using std::jthread or manual pthread for more control
    std::thread feed_thread([&]() { feed_handler.run(); });
    std::thread trading_thread([&]() { trading_engine.run(); });
    
    std::cout << "[Main] System running. Press Ctrl+C to stop." << std::endl;
    std::cout << "\n[Main] Key optimizations implemented:" << std::endl;
    std::cout << "  ✓ Lock-free SPSC queue with cache-line alignment" << std::endl;
    std::cout << "  ✓ Non-blocking UDP with socket optimizations" << std::endl;
    std::cout << "  ✓ CPU affinity pinning" << std::endl;
    std::cout << "  ✓ RDTSC for nanosecond timing" << std::endl;
    std::cout << "  ✓ Busy polling (no blocking)" << std::endl;
    std::cout << "  ✓ Memory ordering optimization" << std::endl;
    std::cout << "\n[Main] Industry-standard reliability features:" << std::endl;
    std::cout << "  ✓ Sequence gap detection and recovery" << std::endl;
    std::cout << "  ✓ Duplicate packet filtering (10K sliding window)" << std::endl;
    std::cout << "  ✓ Out-of-order packet buffering (1K buffer)" << std::endl;
    std::cout << "  ✓ Automatic resequencing of buffered packets" << std::endl;
    std::cout << "  ✓ Feed state machine (INITIAL/LIVE/RECOVERING/STALE)" << std::endl;
    std::cout << "  ✓ Gap fill request generation (with retry logic)" << std::endl;
    std::cout << "  ✓ Recovery feed manager integration points" << std::endl;
    std::cout << "  ✓ Lock-free memory pool (8K slots)" << std::endl;
    std::cout << "  ✓ Async logger (64K message queue)" << std::endl;
    std::cout << "\n[Main] Production enhancements to consider:" << std::endl;
    std::cout << "  • Solarflare/DPDK for true kernel bypass" << std::endl;
    std::cout << "  • Hardware timestamping" << std::endl;
    std::cout << "  • Huge pages for memory" << std::endl;
    std::cout << "  • CPU isolation (isolcpus kernel param)" << std::endl;
    std::cout << "  • NUMA awareness" << std::endl;
    std::cout << "  • Compiler optimizations (-O3 -march=native)" << std::endl;
    std::cout << "  • Actual recovery feed TCP connection" << std::endl;
    std::cout << "  • Snapshot refresh protocol" << std::endl;
    std::cout << std::endl;
    
    // Wait for shutdown signal
    feed_thread.join();
    trading_thread.join();
    
    std::cout << "[Main] Shutdown complete" << std::endl;
    LOG_INFO("=== HFT System Shutdown Complete ===");
    
    // Shutdown logger (flushes remaining messages)
    Logger::shutdown();
    
    return 0;
}

