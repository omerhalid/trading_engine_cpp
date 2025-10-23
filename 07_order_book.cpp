/**
 * LESSON 7: Order Book Implementation
 * 
 * Core of any trading system - maintain live view of market:
 * - Bid/Ask price levels
 * - Queue position
 * - Total liquidity
 * - Fast lookups (O(1) for best bid/ask)
 * 
 * HFT requirements:
 * - Update in < 100ns
 * - No dynamic allocation
 * - Cache-friendly data structures
 */

#include <cstdint>
#include <iostream>
#include <map>
#include <unordered_map>
#include <immintrin.h>

// ============================================================================
// SIMPLE ORDER BOOK - Top of book only (best bid/ask)
// ============================================================================

class SimpleOrderBook {
private:
    uint64_t best_bid_price_{0};
    uint64_t best_ask_price_{UINT64_MAX};
    uint32_t best_bid_size_{0};
    uint32_t best_ask_size_{0};
    
public:
    // Update bid side
    void update_bid(uint64_t price, uint32_t size) {
        best_bid_price_ = price;
        best_bid_size_ = size;
    }
    
    // Update ask side
    void update_ask(uint64_t price, uint32_t size) {
        best_ask_price_ = price;
        best_ask_size_ = size;
    }
    
    // Get mid price
    uint64_t mid_price() const {
        if (best_bid_price_ == 0 || best_ask_price_ == UINT64_MAX) {
            return 0;
        }
        return (best_bid_price_ + best_ask_price_) / 2;
    }
    
    // Get spread
    uint64_t spread() const {
        if (best_bid_price_ == 0 || best_ask_price_ == UINT64_MAX) {
            return UINT64_MAX;
        }
        return best_ask_price_ - best_bid_price_;
    }
    
    void print() const {
        std::cout << "  Bid: $" << (best_bid_price_ / 10000.0) 
                  << " x " << best_bid_size_ << "\n";
        std::cout << "  Ask: $" << (best_ask_price_ / 10000.0) 
                  << " x " << best_ask_size_ << "\n";
        std::cout << "  Mid: $" << (mid_price() / 10000.0) << "\n";
        std::cout << "  Spread: " << (spread() / 100.0) << " cents\n";
    }
};

// ============================================================================
// FULL ORDER BOOK - Multiple price levels (depth)
// ============================================================================

struct PriceLevel {
    uint64_t price;
    uint32_t total_size;
    uint16_t order_count;
};

class FullOrderBook {
private:
    // Use map for sorted price levels (Red-Black tree)
    // Key = price, Value = total size at that price
    std::map<uint64_t, uint32_t, std::greater<uint64_t>> bids_;  // Descending
    std::map<uint64_t, uint32_t> asks_;  // Ascending
    
public:
    // Add order
    void add_order(uint64_t price, uint32_t size, char side) {
        if (side == 'B') {
            bids_[price] += size;
        } else {
            asks_[price] += size;
        }
    }
    
    // Cancel order
    void cancel_order(uint64_t price, uint32_t size, char side) {
        if (side == 'B') {
            auto it = bids_.find(price);
            if (it != bids_.end()) {
                if (it->second <= size) {
                    bids_.erase(it);
                } else {
                    it->second -= size;
                }
            }
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) {
                if (it->second <= size) {
                    asks_.erase(it);
                } else {
                    it->second -= size;
                }
            }
        }
    }
    
    // Get best bid (O(1) for map)
    PriceLevel best_bid() const {
        if (bids_.empty()) return {0, 0, 0};
        auto it = bids_.begin();
        return {it->first, it->second, 1};
    }
    
    // Get best ask (O(1) for map)
    PriceLevel best_ask() const {
        if (asks_.empty()) return {UINT64_MAX, 0, 0};
        auto it = asks_.begin();
        return {it->first, it->second, 1};
    }
    
    // Get market depth (top N levels)
    void print_depth(size_t levels = 5) const {
        std::cout << "\n  === Order Book Depth ===\n";
        
        // Print asks (reverse order - highest to lowest)
        auto ask_it = asks_.rbegin();
        for (size_t i = 0; i < levels && ask_it != asks_.rend(); ++i, ++ask_it) {
            std::cout << "  Ask: $" << (ask_it->first / 10000.0) 
                      << " x " << ask_it->second << "\n";
        }
        
        std::cout << "  ─────────────────────\n";
        
        // Print bids (highest to lowest)
        auto bid_it = bids_.begin();
        for (size_t i = 0; i < levels && bid_it != bids_.end(); ++i, ++bid_it) {
            std::cout << "  Bid: $" << (bid_it->first / 10000.0) 
                      << " x " << bid_it->second << "\n";
        }
    }
    
    // Calculate total liquidity within N cents of mid
    uint64_t liquidity_near_mid(uint64_t cents) const {
        if (bids_.empty() || asks_.empty()) return 0;
        
        uint64_t mid = (bids_.begin()->first + asks_.begin()->first) / 2;
        uint64_t range = cents * 100;  // Convert cents to ticks
        
        uint64_t total = 0;
        
        // Sum bids within range
        for (const auto& [price, size] : bids_) {
            if (mid - price <= range) {
                total += size;
            }
        }
        
        // Sum asks within range
        for (const auto& [price, size] : asks_) {
            if (price - mid <= range) {
                total += size;
            }
        }
        
        return total;
    }
};

// ============================================================================
// PRODUCTION ORDER BOOK - Optimized with price level array
// ============================================================================

class FastOrderBook {
private:
    // Fixed price level array (for stocks typically $0.01 tick size)
    // Assume price range: $50 - $250, tick = $0.01 = 20,000 levels
    static constexpr size_t MAX_LEVELS = 20000;
    static constexpr uint64_t MIN_PRICE = 500000;  // $50.00
    static constexpr uint64_t TICK_SIZE = 100;     // $0.01
    
    // Direct array access = O(1) for any price level
    uint32_t bid_levels_[MAX_LEVELS]{};
    uint32_t ask_levels_[MAX_LEVELS]{};
    
    // Track best levels (indices)
    uint32_t best_bid_idx_{0};
    uint32_t best_ask_idx_{MAX_LEVELS - 1};
    
    // Convert price to array index
    uint32_t price_to_idx(uint64_t price) const {
        return (price - MIN_PRICE) / TICK_SIZE;
    }
    
public:
    // Update price level (O(1))
    void update_level(uint64_t price, uint32_t size, char side) {
        uint32_t idx = price_to_idx(price);
        if (idx >= MAX_LEVELS) return;
        
        if (side == 'B') {
            bid_levels_[idx] = size;
            if (idx > best_bid_idx_ && size > 0) {
                best_bid_idx_ = idx;
            }
        } else {
            ask_levels_[idx] = size;
            if (idx < best_ask_idx_ && size > 0) {
                best_ask_idx_ = idx;
            }
        }
    }
    
    // Get best bid (O(1))
    PriceLevel best_bid() const {
        uint64_t price = MIN_PRICE + (best_bid_idx_ * TICK_SIZE);
        return {price, bid_levels_[best_bid_idx_], 1};
    }
    
    // Get best ask (O(1))
    PriceLevel best_ask() const {
        uint64_t price = MIN_PRICE + (best_ask_idx_ * TICK_SIZE);
        return {price, ask_levels_[best_ask_idx_], 1};
    }
};

// ============================================================================
// DEMO: Order book operations
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

int main() {
    std::cout << "=== ORDER BOOK IMPLEMENTATION ===\n\n";
    
    // Demo 1: Simple order book
    std::cout << "1. Simple Order Book (top of book only):\n";
    SimpleOrderBook simple_book;
    simple_book.update_bid(1499500, 100);  // $149.95 x 100
    simple_book.update_ask(1500000, 200);  // $150.00 x 200
    simple_book.print();
    
    // Demo 2: Full order book
    std::cout << "\n2. Full Order Book (market depth):\n";
    FullOrderBook full_book;
    
    // Add multiple bid levels
    full_book.add_order(1499500, 100, 'B');
    full_book.add_order(1499400, 200, 'B');
    full_book.add_order(1499300, 150, 'B');
    full_book.add_order(1499200, 300, 'B');
    full_book.add_order(1499100, 250, 'B');
    
    // Add multiple ask levels
    full_book.add_order(1500000, 150, 'S');
    full_book.add_order(1500100, 200, 'S');
    full_book.add_order(1500200, 175, 'S');
    full_book.add_order(1500300, 300, 'S');
    full_book.add_order(1500400, 225, 'S');
    
    full_book.print_depth(5);
    
    std::cout << "\n  Liquidity within 10 cents of mid: " 
              << full_book.liquidity_near_mid(10) << " shares\n";
    
    // Demo 3: Benchmark update performance
    std::cout << "\n3. Performance Benchmark:\n";
    FastOrderBook fast_book;
    
    constexpr int UPDATES = 10000;
    uint64_t start = rdtsc();
    
    for (int i = 0; i < UPDATES; ++i) {
        fast_book.update_level(1500000 + (i % 100) * 100, 100, 'B');
    }
    
    uint64_t end = rdtsc();
    std::cout << "  " << UPDATES << " order book updates: " 
              << ((end - start) / UPDATES) << " cycles/update\n";
    std::cout << "  (~" << ((end - start) / UPDATES / 3) << " ns per update)\n";
    
    std::cout << "\nKEY LEARNINGS:\n";
    std::cout << "  • Simple book: just best bid/ask (fastest)\n";
    std::cout << "  • Full book: map-based (flexible but slower)\n";
    std::cout << "  • Fast book: array-based (O(1) everything)\n";
    std::cout << "  • Production: hybrid approach or custom structure\n";
    std::cout << "  • Critical path: updating on every market data tick\n";
    
    return 0;
}

