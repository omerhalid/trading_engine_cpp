#pragma once

#include "spsc_queue.hpp"
#include "types.hpp"
#include "utils.hpp"
#include "logger.hpp"
#include <iostream>
#include <atomic>

namespace hft {

/**
 * Trading Engine Implementation
 * 
 * Consumer side of tick-to-trade pipeline:
 * - Consumes from lock-free queue
 * - Updates order book state
 * - Runs trading strategies
 * - Generates orders
 * 
 * Runs on dedicated CPU core with RT priority
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
        LOG_INFO("TradingEngine thread started");
        
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
        (void)symbol_id;
        (void)price;
        (void)qty;
        (void)side;
    }
};

// External global for shutdown signaling
extern std::atomic<bool> g_running;

} // namespace hft

