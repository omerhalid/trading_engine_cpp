#pragma once

#include "spsc_queue.hpp"
#include "types.hpp"
#include "utils.hpp"
#include "udp_receiver.hpp"
#include "packet_manager.hpp"
#include "logger.hpp"
#include "memory_pool.hpp"
#include <iostream>
#include <atomic>

namespace hft {

/**
 * Feed Handler Implementation
 * 
 * Producer side of tick-to-trade pipeline:
 * - Receives UDP multicast market data
 * - Gap/duplicate detection and handling
 * - Parses and normalizes messages
 * - Pushes to lock-free queue
 * - Busy polls (no blocking)
 * 
 * Runs on dedicated CPU core with RT priority
 */
class FeedHandler {
private:
    UDPReceiver receiver_;
    SPSCQueue<MarketEvent, 65536>& event_queue_;
    FeedHandlerStats& stats_;
    
    // Industry-standard packet management
    PacketManager packet_manager_;
    RecoveryFeedManager recovery_manager_;
    
    // Memory pool for market events (optional - demonstrates usage)
    MemoryPool<MarketEvent, 8192> event_pool_;
    
    uint64_t expected_sequence_{0};
    int core_id_;
    
    // Maintenance timer for periodic checks
    uint64_t last_maintenance_time_{0};
    static constexpr uint64_t MAINTENANCE_INTERVAL_NS = 100000000ULL; // 100ms
    uint64_t last_log_time_{0};
    static constexpr uint64_t LOG_INTERVAL_NS = 5000000000ULL; // 5 seconds

public:
    FeedHandler(SPSCQueue<MarketEvent, 65536>& queue, 
               FeedHandlerStats& stats,
               int core_id = 0,
               bool use_huge_pages = false)
        : event_queue_(queue), stats_(stats), event_pool_(use_huge_pages), core_id_(core_id) {
        
        // Setup gap fill callback
        packet_manager_.set_gap_fill_callback([this](const GapFillRequest& req) {
            handle_gap_fill_request(req);
        });
        
        LOG_INFO("FeedHandler initialized");
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
        LOG_INFO("FeedHandler thread started");
        
        uint64_t spin_count = 0;
        constexpr uint64_t STATS_INTERVAL = 1000000; // Print stats every 1M iterations
        
        last_maintenance_time_ = LatencyTracker::rdtsc();
        last_log_time_ = last_maintenance_time_;
        
        while (g_running.load(std::memory_order_acquire)) {
            const uint64_t current_time = LatencyTracker::rdtsc();
            
            // Periodic maintenance (gap timeout checks, etc.)
            if (current_time - last_maintenance_time_ > MAINTENANCE_INTERVAL_NS) {
                packet_manager_.periodic_maintenance(current_time);
                last_maintenance_time_ = current_time;
                
                // Log memory pool stats periodically
                if (current_time - last_log_time_ > LOG_INTERVAL_NS) {
                    log_stats();
                    last_log_time_ = current_time;
                }
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
        char msg[256];
        snprintf(msg, sizeof(msg), "GAP DETECTED: sequences %lu to %lu (gap size: %lu)",
                req.start_seq, req.end_seq, (req.end_seq - req.start_seq + 1));
        LOG_WARN(msg);
        
        std::cout << "[FeedHandler] " << msg << std::endl;
        
        // In production: send recovery request
        // Examples:
        // 1. CME MDP 3.0: Send TCP request to Replay channel
        // 2. NASDAQ ITCH: Send MOLD UDP retransmit request
        // 3. NYSE Pillar: Send Retransmission request
        
        recovery_manager_.request_retransmission(req.start_seq, req.end_seq);
        
        // Log state transition
        const char* state_str = "UNKNOWN";
        switch (packet_manager_.get_state()) {
            case FeedState::INITIAL:
                state_str = "INITIAL";
                break;
            case FeedState::LIVE:
                state_str = "LIVE";
                break;
            case FeedState::RECOVERING:
                state_str = "RECOVERING";
                LOG_WARN("Feed state: RECOVERING");
                break;
            case FeedState::STALE:
                state_str = "STALE";
                LOG_ERROR("Feed state: STALE - requesting snapshot");
                recovery_manager_.request_snapshot();
                break;
        }
        std::cout << "[FeedHandler] Feed state: " << state_str << std::endl;
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
    
    /**
     * Log statistics to async logger
     */
    void log_stats() {
        const auto& pm_stats = packet_manager_.get_stats();
        const auto pool_stats = event_pool_.get_stats();
        
        char msg[512];
        snprintf(msg, sizeof(msg), 
                "Stats: Packets(recv=%lu proc=%lu drop=%lu) PacketMgr(dup=%lu gaps=%lu) "
                "MemPool(alloc=%lu dealloc=%lu inuse=%lu fail=%lu)",
                stats_.packets_received.load(std::memory_order_relaxed),
                stats_.packets_processed.load(std::memory_order_relaxed),
                stats_.packets_dropped.load(std::memory_order_relaxed),
                pm_stats.duplicates, pm_stats.gaps_detected,
                pool_stats.allocations, pool_stats.deallocations,
                pool_stats.in_use, pool_stats.failures);
        LOG_INFO(msg);
    }
};

// External global for shutdown signaling
extern std::atomic<bool> g_running;

} // namespace hft

