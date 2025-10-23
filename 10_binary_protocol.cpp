/**
 * LESSON 10: Binary Protocols vs Text
 * 
 * Why binary protocols in HFT?
 * - Smaller size (less bandwidth, faster parsing)
 * - Fixed offsets (direct memory access, no scanning)
 * - No string parsing (atoi/atof are slow)
 * - Predictable size (better for cache)
 * 
 * Real exchange protocols (all binary):
 * - NASDAQ ITCH: Pure binary
 * - CME MDP 3.0: Binary with SBE encoding
 * - NYSE Pillar: Binary
 * - FIX: Text-based (legacy, being replaced)
 */

#include <cstdint>
#include <iostream>
#include <cstring>
#include <string>
#include <sstream>
#include <immintrin.h>

// ============================================================================
// TEXT PROTOCOL (like FIX - Financial Information eXchange)
// ============================================================================

// Example FIX message:
// "8=FIX.4.2|9=65|35=D|49=SENDER|56=TARGET|34=1|52=20250101-12:00:00|11=ORDER1|21=1|55=AAPL|54=1|38=100|40=2|44=150.50|"
// Delimited with '|', requires parsing each field

class TextProtocol {
public:
    static void parse_trade(const char* message, uint64_t& price, uint32_t& quantity) {
        // Simulate FIX parsing (simplified)
        std::string msg(message);
        
        // Find price field (tag 44)
        size_t price_pos = msg.find("44=");
        if (price_pos != std::string::npos) {
            price = std::stod(msg.substr(price_pos + 3)) * 10000;
        }
        
        // Find quantity field (tag 38)
        size_t qty_pos = msg.find("38=");
        if (qty_pos != std::string::npos) {
            quantity = std::stoi(msg.substr(qty_pos + 3));
        }
    }
};

// ============================================================================
// BINARY PROTOCOL (like NASDAQ ITCH)
// ============================================================================

// Fixed structure - all fields at known offsets
struct __attribute__((packed)) BinaryTrade {
    uint8_t  msg_type;       // 'T' for trade
    uint64_t timestamp;      // Nanoseconds
    uint64_t sequence;
    uint32_t symbol_id;      // Mapped to ID (not string!)
    uint64_t price;          // Fixed point
    uint32_t quantity;
    uint8_t  side;
};

class BinaryProtocol {
public:
    static void parse_trade(const uint8_t* data, uint64_t& price, uint32_t& quantity) {
        // Direct memory access - no parsing!
        const BinaryTrade* trade = reinterpret_cast<const BinaryTrade*>(data);
        price = trade->price;
        quantity = trade->quantity;
    }
};

// ============================================================================
// OPTIMIZED: Zero-copy with pointer casting
// ============================================================================

class ZeroCopyParser {
public:
    // No copying - work directly with network buffer
    static const BinaryTrade* get_trade(const uint8_t* network_buffer) {
        // Validate message type
        if (UNLIKELY(network_buffer[0] != 'T')) {
            return nullptr;
        }
        
        // Cast directly - zero copy!
        return reinterpret_cast<const BinaryTrade*>(network_buffer);
    }
};

// ============================================================================
// BENCHMARK: Text vs Binary parsing
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

#define UNLIKELY(x) __builtin_expect(!!(x), 0)

void benchmark_text_protocol() {
    const char* fix_msg = "8=FIX.4.2|9=65|35=D|44=150.50|38=100|55=AAPL|";
    constexpr int ITERATIONS = 100000;
    
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        uint64_t price;
        uint32_t quantity;
        TextProtocol::parse_trade(fix_msg, price, quantity);
        volatile uint64_t p = price;  // Prevent optimization
    }
    uint64_t end = rdtsc();
    
    std::cout << "  Text (FIX): " << ((end - start) / ITERATIONS) << " cycles/parse\n";
}

void benchmark_binary_protocol() {
    BinaryTrade binary_msg;
    binary_msg.msg_type = 'T';
    binary_msg.price = 1505000;
    binary_msg.quantity = 100;
    binary_msg.symbol_id = 12345;
    
    constexpr int ITERATIONS = 100000;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&binary_msg);
    
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        uint64_t price;
        uint32_t quantity;
        BinaryProtocol::parse_trade(data, price, quantity);
        volatile uint64_t p = price;  // Prevent optimization
    }
    uint64_t end = rdtsc();
    
    std::cout << "  Binary (ITCH-style): " << ((end - start) / ITERATIONS) << " cycles/parse\n";
}

void benchmark_zerocopy() {
    BinaryTrade binary_msg;
    binary_msg.msg_type = 'T';
    binary_msg.price = 1505000;
    binary_msg.quantity = 100;
    
    constexpr int ITERATIONS = 100000;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&binary_msg);
    
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        const BinaryTrade* trade = ZeroCopyParser::get_trade(data);
        volatile uint64_t p = trade->price;  // Prevent optimization
    }
    uint64_t end = rdtsc();
    
    std::cout << "  Zero-copy (pointer cast): " << ((end - start) / ITERATIONS) << " cycles/parse\n";
}

int main() {
    std::cout << "=== BINARY PROTOCOLS & BRANCH PREDICTION ===\n\n";
    
    std::cout << "1. Protocol Parsing Performance (100K iterations):\n";
    benchmark_text_protocol();
    benchmark_binary_protocol();
    benchmark_zerocopy();
    
    std::cout << "\n2. Message Size Comparison:\n";
    const char* fix = "8=FIX.4.2|35=D|44=150.50|38=100|55=AAPL|54=1|";
    std::cout << "  Text (FIX): " << strlen(fix) << " bytes\n";
    std::cout << "  Binary (ITCH): " << sizeof(BinaryTrade) << " bytes\n";
    std::cout << "  Savings: " << (100 - (sizeof(BinaryTrade) * 100 / strlen(fix))) << "%\n";
    
    std::cout << "\n3. Validation with Branch Hints:\n";
    Packet valid_pkt;
    valid_pkt.magic = 0xDEADBEEF;
    valid_pkt.size = 64;
    
    uint64_t start = rdtsc();
    for (int i = 0; i < 1000000; ++i) {
        volatile bool result = validate_packet_bad(&valid_pkt);
    }
    uint64_t end = rdtsc();
    std::cout << "  Without UNLIKELY: " << ((end - start) / 1000000) << " cycles\n";
    
    start = rdtsc();
    for (int i = 0; i < 1000000; ++i) {
        volatile bool result = validate_packet_good(&valid_pkt);
    }
    end = rdtsc();
    std::cout << "  With UNLIKELY: " << ((end - start) / 1000000) << " cycles\n";
    
    std::cout << "\nREAL EXCHANGE PROTOCOLS:\n";
    std::cout << "  • NASDAQ ITCH: Binary, ~50 bytes/msg, ~20 cycles to parse\n";
    std::cout << "  • CME MDP 3.0: Binary (SBE), ~30-100 bytes\n";
    std::cout << "  • NYSE Pillar: Binary, ~40 bytes average\n";
    std::cout << "  • FIX: Text, 100-300 bytes, ~1000 cycles to parse (legacy)\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Binary = 10-50x faster than text parsing\n";
    std::cout << "  • Fixed fields = direct memory access (no scanning)\n";
    std::cout << "  • Zero-copy = work with network buffer directly\n";
    std::cout << "  • LIKELY/UNLIKELY help branch predictor\n";
    std::cout << "  • Minimize branches in hot paths\n";
    std::cout << "  • All modern exchanges use binary protocols\n";
    
    return 0;
}

