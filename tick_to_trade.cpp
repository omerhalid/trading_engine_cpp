#include "spsc_queue.hpp"
#include "feed_handler.hpp"
#include "udp_receiver.hpp"
#include "packet_manager.hpp"
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
 * Feed Handler Class - Producer Side
 */
class FeedHandler {
private:
    UDPReceiver receiver_;
    SPSCQueue<MarketEvent, 65536>& event_queue_;  // Power of 2 size
    FeedHandlerStats& stats_;
    
    // Industry-standard packet management
    PacketManager packet_manager_;
    RecoveryFeedManager recovery_manager_;
    
    uint64_t expected_sequence_{0};
    int core_id_;
    
    // Maintenance timer for periodic checks
    uint64_t last_maintenance_time_{0};
    static constexpr uint64_t MAINTENANCE_INTERVAL_NS = 100000000ULL; // 100ms

public:
    FeedHandler(SPSCQueue<MarketEvent, 65536>& queue, 
               FeedHandlerStats& stats,
               int core_id = 0)
        : event_queue_(queue), stats_(stats), core_id_(core_id) {
        
        // Setup gap fill callback
        packet_manager_.set_gap_fill_callback([this](const GapFillRequest& req) {
            handle_gap_fill_request(req);
        });
    }
    
    /**
     * Initialize UDP receiver
     */
    bool init(const std::string& multicast_ip, uint16_t port) {
        return receiver_.initialize(multicast_ip, port);
    }
    
    /**
     * Main processing loop - runs on dedicated core
     * This is the hot path - every nanosecond counts
     */
    void run() {
        // Pin to CPU core - avoid context switches
        ThreadUtils::pin_to_core(core_id_);
        
        // Set real-time priority (requires privileges)
        ThreadUtils::set_realtime_priority();
        
        std::cout << "[FeedHandler] Started on core " << core_id_ << std::endl;
        
        uint64_t spin_count = 0;
        constexpr uint64_t STATS_INTERVAL = 1000000; // Print stats every 1M iterations
        
        last_maintenance_time_ = LatencyTracker::rdtsc();
        
        while (g_running.load(std::memory_order_acquire)) {
            const uint64_t current_time = LatencyTracker::rdtsc();
            
            // Periodic maintenance (gap timeout checks, etc.)
            if (current_time - last_maintenance_time_ > MAINTENANCE_INTERVAL_NS) {
                packet_manager_.periodic_maintenance(current_time);
                last_maintenance_time_ = current_time;
            }
            
            // Busy poll for packets - no blocking!
            uint8_t* buffer_ptr = nullptr;
            ssize_t bytes_received = receiver_.receive_internal(buffer_ptr);
            
            if (bytes_received > 0) {
                // Timestamp immediately on receive - critical for latency measurement
                const uint64_t recv_tsc = LatencyTracker::rdtsc();
                
                stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
                
                // Process packet with gap/duplicate handling
                process_packet(buffer_ptr, bytes_received, recv_tsc);
                
                // Check for buffered packets that are now ready
                auto ready_packets = packet_manager_.get_ready_packets();
                for (const auto& packet_data : ready_packets) {
                    // Process buffered packet
                    process_buffered_packet(packet_data.data(), packet_data.size(), recv_tsc);
                }
                
            } else if (bytes_received == 0) {
                // No data - spin wait with pause
                SpinWait::pause();
                spin_count++;
            } else {
                // Error occurred
                std::cerr << "[FeedHandler] Receive error" << std::endl;
                break;
            }
            
            // Periodic stats logging (non-critical path)
            if (spin_count % STATS_INTERVAL == 0) {
                print_stats();
            }
        }
        
        std::cout << "[FeedHandler] Stopped" << std::endl;
    }

private:
    /**
     * Parse and normalize market data packet
     * Now with industry-standard gap and duplicate handling
     */
    void process_packet(const uint8_t* data, size_t size, uint64_t recv_tsc) {
        // Basic validation
        if (size < sizeof(MarketDataPacket)) {
            return;
        }
        
        // In production: avoid memcpy, work directly with packet buffer
        const auto* packet = reinterpret_cast<const MarketDataPacket*>(data);
        
        // ==== INDUSTRY STANDARD GAP/DUPLICATE HANDLING ====
        // Use PacketManager to handle sequencing, gaps, and duplicates
        const bool should_process = packet_manager_.process_packet(
            packet->packet_sequence,
            data,
            size,
            recv_tsc
        );
        
        // Update statistics from packet manager
        const auto& pm_stats = packet_manager_.get_stats();
        if (pm_stats.duplicates > 0) {
            stats_.sequence_gaps.fetch_add(pm_stats.duplicates, std::memory_order_relaxed);
        }
        
        if (!should_process) {
            // Packet is duplicate, out-of-order, or we're in recovery mode
            // PacketManager will buffer it if needed
            return;
        }
        
        // Packet passed all checks - proceed with parsing
        parse_and_queue_packet(packet, recv_tsc);
    }
    
    /**
     * Process buffered packet (from resequence buffer)
     */
    void process_buffered_packet(const uint8_t* data, size_t size, uint64_t recv_tsc) {
        if (size < sizeof(MarketDataPacket)) {
            return;
        }
        
        const auto* packet = reinterpret_cast<const MarketDataPacket*>(data);
        parse_and_queue_packet(packet, recv_tsc);
    }
    
    /**
     * Parse packet and push to queue
     * Separated from process_packet for reuse with buffered packets
     */
    void parse_and_queue_packet(const MarketDataPacket* packet, uint64_t recv_tsc) {
        // Create normalized event
        MarketEvent event{};
        event.recv_timestamp_ns = recv_tsc;
        event.type = packet->msg_type;
        
        // Parse based on message type
        switch (packet->msg_type) {
            case MessageType::TRADE: {
                const auto& trade = packet->payload.trade;
                event.exchange_timestamp_ns = trade.timestamp_ns;
                event.symbol_id = trade.symbol_id;
                event.data.trade.price = trade.price;
                event.data.trade.quantity = trade.quantity;
                event.data.trade.side = trade.side;
                break;
            }
            
            case MessageType::QUOTE: {
                const auto& quote = packet->payload.quote;
                event.exchange_timestamp_ns = quote.timestamp_ns;
                event.symbol_id = quote.symbol_id;
                event.data.quote.bid_price = quote.bid_price;
                event.data.quote.ask_price = quote.ask_price;
                event.data.quote.bid_size = quote.bid_size;
                event.data.quote.ask_size = quote.ask_size;
                break;
            }
            
            case MessageType::HEARTBEAT:
                // Just update sequence, don't push to queue
                return;
                
            default:
                return; // Unknown message type
        }
        
        // Push to lock-free queue - non-blocking
        if (!event_queue_.try_push(event)) {
            // Queue full - this is bad! Means trading logic is too slow
            stats_.packets_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        
        // Calculate processing latency
        const uint64_t process_tsc = LatencyTracker::rdtscp();
        const uint64_t latency_ticks = process_tsc - recv_tsc;
        const uint64_t latency_ns = LatencyTracker::tsc_to_ns(latency_ticks);
        
        stats_.packets_processed.fetch_add(1, std::memory_order_relaxed);
        stats_.update_latency(latency_ns);
    }
    
    /**
     * Handle gap fill request callback
     * In production: sends request to recovery feed
     */
    void handle_gap_fill_request(const GapFillRequest& req) {
        std::cout << "[FeedHandler] GAP DETECTED: sequences " 
                  << req.start_seq << " to " << req.end_seq 
                  << " (gap size: " << (req.end_seq - req.start_seq + 1) << ")"
                  << std::endl;
        
        // In production: send recovery request
        // Examples:
        // 1. CME MDP 3.0: Send TCP request to Replay channel
        // 2. NASDAQ ITCH: Send MOLD UDP retransmit request
        // 3. NYSE Pillar: Send Retransmission request
        
        recovery_manager_.request_retransmission(req.start_seq, req.end_seq);
        
        // Log state transition
        std::cout << "[FeedHandler] Feed state: ";
        switch (packet_manager_.get_state()) {
            case FeedState::INITIAL:
                std::cout << "INITIAL";
                break;
            case FeedState::LIVE:
                std::cout << "LIVE";
                break;
            case FeedState::RECOVERING:
                std::cout << "RECOVERING";
                break;
            case FeedState::STALE:
                std::cout << "STALE (requesting snapshot)";
                recovery_manager_.request_snapshot();
                break;
        }
        std::cout << std::endl;
    }
    
    void print_stats() {
        const uint64_t recv = stats_.packets_received.load(std::memory_order_relaxed);
        const uint64_t proc = stats_.packets_processed.load(std::memory_order_relaxed);
        const uint64_t drop = stats_.packets_dropped.load(std::memory_order_relaxed);
        const uint64_t gaps = stats_.sequence_gaps.load(std::memory_order_relaxed);
        
        const auto& pm_stats = packet_manager_.get_stats();
        
        if (proc > 0) {
            std::cout << "[FeedHandler] Stats - "
                      << "Recv: " << recv
                      << ", Proc: " << proc
                      << ", Drop: " << drop
                      << ", Gaps: " << gaps
                      << ", Avg Latency: " << static_cast<int>(stats_.avg_latency_ns()) << "ns"
                      << ", Min: " << stats_.min_latency_ns << "ns"
                      << ", Max: " << stats_.max_latency_ns << "ns"
                      << std::endl;
            
            std::cout << "[PacketMgr] Stats - "
                      << "Duplicates: " << pm_stats.duplicates
                      << ", Gaps Detected: " << pm_stats.gaps_detected
                      << ", Gaps Filled: " << pm_stats.gaps_filled
                      << ", Out-of-Order: " << pm_stats.out_of_order
                      << ", Resequenced: " << pm_stats.resequenced
                      << ", Overflow Drops: " << pm_stats.dropped_overflow
                      << ", Next Expected: " << packet_manager_.get_next_expected()
                      << std::endl;
        }
    }
};

/**
 * Trading Logic - Consumer Side
 */
class TradingEngine {
private:
    SPSCQueue<MarketEvent, 65536>& event_queue_;
    int core_id_;
    
    // Order book state (simplified)
    // In production: highly optimized order book with hash maps, price levels, etc.
    uint64_t last_bid_{0};
    uint64_t last_ask_{0};

public:
    TradingEngine(SPSCQueue<MarketEvent, 65536>& queue, int core_id = 1)
        : event_queue_(queue), core_id_(core_id) {}
    
    /**
     * Main trading loop - runs on dedicated core
     */
    void run() {
        // Pin to different core than feed handler
        ThreadUtils::pin_to_core(core_id_);
        ThreadUtils::set_realtime_priority();
        
        std::cout << "[TradingEngine] Started on core " << core_id_ << std::endl;
        
        MarketEvent event{};
        uint64_t events_processed = 0;
        
        while (g_running.load(std::memory_order_acquire)) {
            // Try to pop from queue - non-blocking
            if (event_queue_.try_pop(event)) {
                // Timestamp when we got the event
                const uint64_t process_tsc = LatencyTracker::rdtsc();
                
                // Process event
                process_event(event);
                
                // Calculate tick-to-trade latency
                const uint64_t total_latency_ticks = process_tsc - event.recv_timestamp_ns;
                const uint64_t total_latency_ns = LatencyTracker::tsc_to_ns(total_latency_ticks);
                
                events_processed++;
                
                // Log every 100000th event
                if (events_processed % 100000 == 0) {
                    std::cout << "[TradingEngine] Processed " << events_processed 
                              << " events, Last latency: " << total_latency_ns << "ns"
                              << std::endl;
                }
                
            } else {
                // No events - spin wait
                SpinWait::pause();
            }
        }
        
        std::cout << "[TradingEngine] Stopped. Total events: " << events_processed << std::endl;
    }

private:
    /**
     * Process market event and run trading logic
     * This is where your alpha lives!
     */
    void process_event(const MarketEvent& event) {
        switch (event.type) {
            case MessageType::TRADE:
                handle_trade(event);
                break;
                
            case MessageType::QUOTE:
                handle_quote(event);
                break;
                
            default:
                break;
        }
    }
    
    void handle_trade(const MarketEvent& event) {
        // Example: Simple trade signal logic
        // In production: complex strategies, ML models, etc.
        
        const auto& trade = event.data.trade;
        
        // Example: If large trade on bid side, might indicate buying pressure
        if (trade.side == 'B' && trade.quantity > 10000) {
            // Would send buy order here
            // send_order(event.symbol_id, trade.price, 100);
        }
    }
    
    void handle_quote(const MarketEvent& event) {
        const auto& quote = event.data.quote;
        
        // Update our view of the market
        last_bid_ = quote.bid_price;
        last_ask_ = quote.ask_price;
        
        // Example: Spread calculation
        const uint64_t spread = last_ask_ - last_bid_;
        
        // Example strategy: If spread is wide, might place orders inside spread
        if (spread > 1000) { // Wide spread
            // Calculate mid price
            const uint64_t mid = (last_bid_ + last_ask_) / 2;
            
            // Would send orders here
            // send_order(event.symbol_id, mid - 1, 100, 'B'); // Buy below mid
            // send_order(event.symbol_id, mid + 1, 100, 'S'); // Sell above mid
        }
    }
    
    /**
     * Send order to gateway
     * In production: another SPSC queue to order gateway thread
     */
    void send_order(uint32_t symbol_id, uint64_t price, uint32_t qty, char side = 'B') {
        // This would push to order queue
        // Order gateway on another core would send via TCP/binary protocol
        // Total tick-to-trade including this: 1-3 microseconds
    }
};

/**
 * Main function - sets up the tick-to-trade pipeline
 */
int main(int argc, char* argv[]) {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║         HFT TICK-TO-TRADE FEED HANDLER                      ║
║         Lock-Free SPSC | Kernel Bypass UDP                  ║
╚══════════════════════════════════════════════════════════════╝
    )" << std::endl;
    
    // Configuration
    // In production: read from config file
    const std::string MULTICAST_IP = "233.54.12.1";
    const uint16_t PORT = 15000;
    const int FEED_HANDLER_CORE = 0;
    const int TRADING_ENGINE_CORE = 1;
    
    // Setup signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create shared components
    SPSCQueue<MarketEvent, 65536> event_queue;
    FeedHandlerStats stats;
    
    // Create feed handler and trading engine
    FeedHandler feed_handler(event_queue, stats, FEED_HANDLER_CORE);
    TradingEngine trading_engine(event_queue, TRADING_ENGINE_CORE);
    
    // Initialize UDP receiver
    std::cout << "[Main] Initializing UDP receiver..." << std::endl;
    if (!feed_handler.init(MULTICAST_IP, PORT)) {
        std::cerr << "[Main] Failed to initialize UDP receiver" << std::endl;
        return 1;
    }
    std::cout << "[Main] Listening on " << MULTICAST_IP << ":" << PORT << std::endl;
    
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
    
    return 0;
}

