#pragma once

#include <cstdint>
#include <atomic>

namespace hft {

/**
 * Market data message types
 * In real systems: NYSE, NASDAQ, CME have proprietary binary protocols
 * (e.g., ITCH for NASDAQ, MDP3 for CME)
 */
enum class MessageType : uint8_t {
    TRADE = 0x01,
    QUOTE = 0x02,
    ORDER_ADD = 0x03,
    ORDER_DELETE = 0x04,
    ORDER_MODIFY = 0x05,
    HEARTBEAT = 0xFF
};

/**
 * Trade message - typical HFT structure
 * - Packed to minimize size (cache efficiency)
 * - Fixed size for predictable memory access
 * - Natural alignment where possible
 */
struct __attribute__((packed)) TradeMessage {
    uint64_t timestamp_ns;      // Exchange timestamp in nanoseconds
    uint64_t sequence_num;      // Sequence number for gap detection
    uint32_t symbol_id;         // Symbol ID (mapped, not string for speed)
    uint32_t trade_id;          // Unique trade identifier
    uint64_t price;             // Price in fixed point (e.g., *10000 for 4 decimals)
    uint32_t quantity;          // Quantity
    uint8_t  side;              // 'B' or 'S'
    uint8_t  padding[3];        // Explicit padding for alignment
};

/**
 * Quote/Level update message
 */
struct __attribute__((packed)) QuoteMessage {
    uint64_t timestamp_ns;
    uint64_t sequence_num;
    uint32_t symbol_id;
    uint64_t bid_price;
    uint64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint8_t  num_levels;        // For market depth
    uint8_t  padding[7];
};

/**
 * Generic market data packet container
 * In production, you'd have a packet header followed by multiple messages
 */
struct __attribute__((packed)) MarketDataPacket {
    MessageType msg_type;
    uint8_t     version;
    uint16_t    payload_size;
    uint64_t    packet_sequence;
    
    union {
        TradeMessage trade;
        QuoteMessage quote;
        uint8_t raw_data[256];  // Max payload size
    } payload;
};

/**
 * Processed market event after normalization
 * This is what goes into the SPSC queue and consumed by trading logic
 */
struct MarketEvent {
    uint64_t recv_timestamp_ns;     // When we received it (RDTSC)
    uint64_t exchange_timestamp_ns; // Exchange timestamp
    uint32_t symbol_id;
    MessageType type;
    
    // Union for different event types
    union {
        struct {
            uint64_t price;
            uint32_t quantity;
            uint8_t  side;
        } trade;
        
        struct {
            uint64_t bid_price;
            uint64_t ask_price;
            uint32_t bid_size;
            uint32_t ask_size;
        } quote;
    } data;
};

/**
 * Statistics tracker for monitoring feed handler performance
 */
struct FeedHandlerStats {
    alignas(64) std::atomic<uint64_t> packets_received{0};
    alignas(64) std::atomic<uint64_t> packets_processed{0};
    alignas(64) std::atomic<uint64_t> packets_dropped{0};
    alignas(64) std::atomic<uint64_t> sequence_gaps{0};
    alignas(64) std::atomic<uint64_t> total_latency_ns{0};
    
    // Min/max latency tracking (not atomic for simplicity, could add spinlock)
    uint64_t min_latency_ns{UINT64_MAX};
    uint64_t max_latency_ns{0};
    
    void update_latency(uint64_t latency_ns) noexcept {
        total_latency_ns.fetch_add(latency_ns, std::memory_order_relaxed);
        if (latency_ns < min_latency_ns) min_latency_ns = latency_ns;
        if (latency_ns > max_latency_ns) max_latency_ns = latency_ns;
    }
    
    double avg_latency_ns() const noexcept {
        uint64_t processed = packets_processed.load(std::memory_order_relaxed);
        if (processed == 0) return 0.0;
        return static_cast<double>(total_latency_ns.load(std::memory_order_relaxed)) / processed;
    }
};

} // namespace hft

