/**
 * LESSON 6: Complete Tick-to-Trade System (Simplified)
 * 
 * Brings everything together:
 * - UDP receiver (Lesson 4)
 * - SPSC queue (Lesson 2)
 * - Market data parsing
 * - Simple trading logic
 * 
 * This is a simplified version to understand the full flow.
 * For production version see main.cpp
 */

#include <atomic>
#include <thread>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <signal.h>

// ============================================================================
// Minimal SPSC Queue (from Lesson 2)
// ============================================================================

template<typename T, size_t Size>
class SPSCQueue {
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
};

// ============================================================================
// Market Data Types
// ============================================================================

struct __attribute__((packed)) TradePacket {
    uint64_t sequence;
    uint64_t timestamp;
    uint32_t symbol_id;
    uint64_t price;      // Fixed point: price * 10000
    uint32_t quantity;
    uint8_t  side;       // 'B' or 'S'
};

struct MarketEvent {
    uint64_t recv_time;  // When we received it
    uint32_t symbol_id;
    uint64_t price;
    uint32_t quantity;
    uint8_t  side;
};

// ============================================================================
// Timing utilities
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

static inline uint64_t rdtscp() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// ============================================================================
// Feed Handler Thread (Producer)
// ============================================================================

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false);
}

class SimpleFeedHandler {
private:
    SPSCQueue<MarketEvent, 4096>& queue_;
    uint64_t packets_received_{0};
    uint64_t next_expected_seq_{0};
    
public:
    SimpleFeedHandler(SPSCQueue<MarketEvent, 4096>& q) : queue_(q) {}
    
    void run() {
        std::cout << "[FeedHandler] Started (simulated data)\n";
        
        // Simulate receiving packets
        // In real system: busy-poll UDP socket
        while (g_running.load() && packets_received_ < 100) {
            // Simulate packet arrival
            uint64_t recv_tsc = rdtsc();
            
            // Create fake trade packet
            TradePacket packet;
            packet.sequence = next_expected_seq_++;
            packet.timestamp = recv_tsc;
            packet.symbol_id = 12345;  // AAPL
            packet.price = 1500000 + (packets_received_ % 100);  // $150.00 + cents
            packet.quantity = 100;
            packet.side = (packets_received_ % 2) ? 'B' : 'S';
            
            // Parse and normalize
            MarketEvent event;
            event.recv_time = recv_tsc;
            event.symbol_id = packet.symbol_id;
            event.price = packet.price;
            event.quantity = packet.quantity;
            event.side = packet.side;
            
            // Push to queue
            if (!queue_.try_push(event)) {
                std::cout << "[FeedHandler] Queue full!\n";
                break;
            }
            
            packets_received_++;
            
            // Simulate packet rate (1000 per second)
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        
        std::cout << "[FeedHandler] Stopped. Packets: " << packets_received_ << "\n";
    }
};

// ============================================================================
// Trading Engine Thread (Consumer)
// ============================================================================

class SimpleTradingEngine {
private:
    SPSCQueue<MarketEvent, 4096>& queue_;
    uint64_t events_processed_{0};
    uint64_t total_latency_cycles_{0};
    
public:
    SimpleTradingEngine(SPSCQueue<MarketEvent, 4096>& q) : queue_(q) {}
    
    void run() {
        std::cout << "[TradingEngine] Started\n";
        
        MarketEvent event;
        
        while (g_running.load() || queue_.size() > 0) {
            if (queue_.try_pop(event)) {
                // Timestamp when we process
                uint64_t process_tsc = rdtscp();
                
                // Calculate tick-to-trade latency
                uint64_t latency = process_tsc - event.recv_time;
                total_latency_cycles_ += latency;
                
                // Simple trading logic
                process_event(event);
                
                events_processed_++;
                
                if (events_processed_ % 10 == 0) {
                    double avg_latency = (double)total_latency_cycles_ / events_processed_;
                    double avg_ns = avg_latency / 3.0;  // Assume 3GHz CPU
                    std::cout << "[TradingEngine] Processed " << events_processed_ 
                              << " events, Avg latency: " << (int)avg_ns << " ns\n";
                }
            }
            else {
                // No data - spin wait
                _mm_pause();
            }
        }
        
        std::cout << "[TradingEngine] Stopped. Total events: " << events_processed_ << "\n";
        
        if (events_processed_ > 0) {
            double avg_latency = (double)total_latency_cycles_ / events_processed_;
            double avg_ns = avg_latency / 3.0;
            std::cout << "[TradingEngine] Average tick-to-trade: " << (int)avg_ns << " ns\n";
        }
    }
    
private:
    void process_event(const MarketEvent& event) {
        // Example strategy: print large trades
        if (event.quantity > 500) {
            std::cout << "  [TRADE] Large order: " 
                      << (event.side == 'B' ? "BUY" : "SELL")
                      << " " << event.quantity
                      << " @ $" << (event.price / 10000.0)
                      << "\n";
        }
        
        // In real system: update order book, run strategies, send orders
    }
};

// ============================================================================
// Main: Put it all together
// ============================================================================

int main() {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║         SIMPLE TICK-TO-TRADE SYSTEM                         ║
║         Feed Handler -> SPSC Queue -> Trading Engine        ║
╚══════════════════════════════════════════════════════════════╝
    )" << "\n";
    
    signal(SIGINT, signal_handler);
    
    // Create SPSC queue (shared between threads)
    SPSCQueue<MarketEvent, 4096> event_queue;
    
    // Create feed handler and trading engine
    SimpleFeedHandler feed_handler(event_queue);
    SimpleTradingEngine trading_engine(event_queue);
    
    // Launch threads
    std::thread feed_thread([&]() { feed_handler.run(); });
    std::thread trading_thread([&]() { trading_engine.run(); });
    
    std::cout << "\n[Main] System running...\n";
    std::cout << "[Main] Will process 100 simulated packets then stop\n\n";
    
    // Wait for completion
    feed_thread.join();
    trading_thread.join();
    
    std::cout << "\n[Main] Complete!\n\n";
    
    std::cout << "ARCHITECTURE:\n";
    std::cout << "  [Feed Handler Thread]    [Trading Engine Thread]\n";
    std::cout << "         ↓                           ↑\n";
    std::cout << "    Receive UDP              Process Market Data\n";
    std::cout << "    Parse Packet             Update Order Book\n";
    std::cout << "    Push to Queue ------→    Pop from Queue\n";
    std::cout << "    (Producer)               Run Strategy\n";
    std::cout << "                             Send Orders\n";
    std::cout << "                             (Consumer)\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Two threads: producer (feed) and consumer (trading)\n";
    std::cout << "  • SPSC queue connects them (lock-free)\n";
    std::cout << "  • Each thread on separate CPU core\n";
    std::cout << "  • Measure latency from receive to trade decision\n";
    std::cout << "  • Typical latency: 1-10 microseconds\n";
    
    return 0;
}

