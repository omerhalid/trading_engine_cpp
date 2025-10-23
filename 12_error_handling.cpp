/**
 * LESSON 12: Error Handling Without Exceptions
 * 
 * Why NO exceptions in HFT?
 * - Non-deterministic overhead (stack unwinding)
 * - Can cause 10-100x latency spikes
 * - Exception tables pollute instruction cache
 * - Branch mispredictions on exception paths
 * 
 * Industry standard: Error codes + fail fast
 * - Deterministic performance
 * - Explicit error handling
 * - Compiler flag: -fno-exceptions
 * 
 * Used by: ALL major HFT firms (Jane Street, Citadel, Jump, Optiver)
 */

#include <cstdint>
#include <iostream>
#include <immintrin.h>

// ============================================================================
// Error Codes - Simple and Fast
// ============================================================================

enum class ErrorCode : uint8_t {
    SUCCESS = 0,
    INVALID_INPUT = 1,
    BUFFER_FULL = 2,
    NETWORK_ERROR = 3,
    PARSE_ERROR = 4,
    TIMEOUT = 5,
};

// Convert to string (for logging only, not hot path)
const char* error_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS: return "Success";
        case ErrorCode::INVALID_INPUT: return "Invalid input";
        case ErrorCode::BUFFER_FULL: return "Buffer full";
        case ErrorCode::NETWORK_ERROR: return "Network error";
        case ErrorCode::PARSE_ERROR: return "Parse error";
        case ErrorCode::TIMEOUT: return "Timeout";
        default: return "Unknown error";
    }
}

// ============================================================================
// Result Type - Rust-style error handling
// ============================================================================

template<typename T>
struct Result {
    T value;
    ErrorCode error;
    
    // Check success
    bool ok() const { return error == ErrorCode::SUCCESS; }
    bool is_error() const { return error != ErrorCode::SUCCESS; }
    
    // Get value (caller MUST check ok() first!)
    T& get() { return value; }
    const T& get() const { return value; }
};

// Helper constructors
template<typename T>
Result<T> Ok(T val) {
    return Result<T>{val, ErrorCode::SUCCESS};
}

template<typename T>
Result<T> Err(ErrorCode code) {
    return Result<T>{T{}, code};
}

// ============================================================================
// EXAMPLE: Parse packet with error handling
// ============================================================================

struct Packet {
    uint32_t magic;
    uint16_t size;
    uint64_t sequence;
    uint8_t data[256];
};

// BAD: Uses exceptions (disabled with -fno-exceptions anyway)
// Packet parse_packet_bad(const uint8_t* buffer, size_t len) {
//     if (!buffer) throw std::runtime_error("Null buffer");
//     if (len < sizeof(Packet)) throw std::runtime_error("Too small");
//     // ... would cause non-deterministic overhead
// }

// GOOD: Returns Result type
Result<Packet> parse_packet(const uint8_t* buffer, size_t len) {
    // Validate input (fast checks first)
    if (!buffer) {
        return Err<Packet>(ErrorCode::INVALID_INPUT);
    }
    
    if (len < sizeof(Packet)) {
        return Err<Packet>(ErrorCode::INVALID_INPUT);
    }
    
    // Zero-copy parse
    const Packet* pkt = reinterpret_cast<const Packet*>(buffer);
    
    // Validate magic number
    if (pkt->magic != 0xDEADBEEF) {
        return Err<Packet>(ErrorCode::PARSE_ERROR);
    }
    
    // Success - return packet
    return Ok(*pkt);
}

// ============================================================================
// Branch Hints for Error Paths
// ============================================================================

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// Optimized version with branch hints
Result<Packet> parse_packet_optimized(const uint8_t* buffer, size_t len) {
    // Error paths are UNLIKELY
    if (UNLIKELY(!buffer)) {
        return Err<Packet>(ErrorCode::INVALID_INPUT);
    }
    
    if (UNLIKELY(len < sizeof(Packet))) {
        return Err<Packet>(ErrorCode::INVALID_INPUT);
    }
    
    const Packet* pkt = reinterpret_cast<const Packet*>(buffer);
    
    if (UNLIKELY(pkt->magic != 0xDEADBEEF)) {
        return Err<Packet>(ErrorCode::PARSE_ERROR);
    }
    
    // Hot path - success
    return Ok(*pkt);
}

// ============================================================================
// Usage Pattern
// ============================================================================

void process_packet_stream(const uint8_t* buffer, size_t len) {
    auto result = parse_packet_optimized(buffer, len);
    
    // Check for error (UNLIKELY in production)
    if (UNLIKELY(result.is_error())) {
        // Log error (async) and return
        // In production: increment error counter, maybe log
        std::cerr << "Parse error: " << error_string(result.error) << "\n";
        return;  // Fail fast
    }
    
    // Happy path - process packet
    Packet& pkt = result.get();
    std::cout << "Processing packet seq: " << pkt.sequence << "\n";
}

// ============================================================================
// Debug Assertions (compile out in release)
// ============================================================================

#ifdef NDEBUG
    #define HFT_ASSERT(cond) ((void)0)
    #define HFT_ASSERT_MSG(cond, msg) ((void)0)
#else
    #include <cassert>
    #define HFT_ASSERT(cond) assert(cond)
    #define HFT_ASSERT_MSG(cond, msg) assert((cond) && (msg))
#endif

void example_assertions() {
    int* ptr = nullptr;
    
    // Debug builds: checks assertion
    // Release builds: compiled out (zero overhead)
    HFT_ASSERT(ptr != nullptr);
    
    // With message
    HFT_ASSERT_MSG(ptr != nullptr, "Pointer must not be null");
}

// ============================================================================
// BENCHMARK: Error handling overhead
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void benchmark_error_handling() {
    constexpr int ITERATIONS = 1000000;
    
    // Valid packet
    uint8_t buffer[sizeof(Packet)];
    Packet* pkt = reinterpret_cast<Packet*>(buffer);
    pkt->magic = 0xDEADBEEF;
    pkt->size = 64;
    pkt->sequence = 123;
    
    // Benchmark success path
    uint64_t start = rdtsc();
    for (int i = 0; i < ITERATIONS; ++i) {
        auto result = parse_packet_optimized(buffer, sizeof(buffer));
        volatile bool ok = result.ok();
    }
    uint64_t end = rdtsc();
    
    std::cout << "  Result<T> (success path): " << ((end - start) / ITERATIONS) << " cycles\n";
    std::cout << "  (~" << ((end - start) / ITERATIONS / 3) << " ns)\n";
}

int main() {
    std::cout << "=== ERROR HANDLING WITHOUT EXCEPTIONS ===\n\n";
    
    std::cout << "1. Why No Exceptions in HFT:\n";
    std::cout << "  ✗ Stack unwinding = unpredictable latency\n";
    std::cout << "  ✗ Exception tables = cache pollution\n";
    std::cout << "  ✗ Can cause 10-100x latency spikes\n";
    std::cout << "  ✓ Error codes = deterministic, fast\n\n";
    
    std::cout << "2. Result<T> Pattern:\n";
    uint8_t buffer[sizeof(Packet)];
    Packet* pkt = reinterpret_cast<Packet*>(buffer);
    pkt->magic = 0xDEADBEEF;
    pkt->size = 64;
    pkt->sequence = 42;
    
    auto result = parse_packet(buffer, sizeof(buffer));
    if (result.ok()) {
        std::cout << "  Parsed packet, seq: " << result.get().sequence << "\n";
    } else {
        std::cout << "  Error: " << error_string(result.error) << "\n";
    }
    
    std::cout << "\n3. Error Path with Invalid Input:\n";
    pkt->magic = 0xBADBAD;  // Invalid magic
    result = parse_packet(buffer, sizeof(buffer));
    if (result.is_error()) {
        std::cout << "  Error (as expected): " << error_string(result.error) << "\n";
    }
    
    std::cout << "\n4. Performance Benchmark:\n";
    benchmark_error_handling();
    
    std::cout << "\n5. Debug Assertions:\n";
    std::cout << "  Compiled with -DNDEBUG: assertions removed (zero overhead)\n";
    std::cout << "  Debug builds: assertions active for validation\n";
    
    std::cout << "\nCOMPILER FLAGS FOR PRODUCTION:\n";
    std::cout << "  -fno-exceptions    Disable exception handling\n";
    std::cout << "  -fno-rtti          Disable runtime type info\n";
    std::cout << "  -DNDEBUG           Disable assertions\n";
    std::cout << "  Benefits: Smaller binary, faster, predictable\n\n";
    
    std::cout << "KEY LEARNINGS:\n";
    std::cout << "  • Use Result<T> for operations that can fail\n";
    std::cout << "  • Error codes are fast and deterministic\n";
    std::cout << "  • UNLIKELY() hints for error paths\n";
    std::cout << "  • Fail fast - don't try to recover in hot path\n";
    std::cout << "  • Assertions for debug, compiled out in release\n";
    std::cout << "  • All HFT shops disable exceptions\n";
    
    return 0;
}

