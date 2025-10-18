#pragma once

#include <cstdint>

namespace hft {

/**
 * Error Handling for Low-Latency Systems
 * 
 * NO EXCEPTIONS - They add non-deterministic overhead:
 * - Stack unwinding is unpredictable
 * - Exception tables add memory overhead
 * - Branch mispredictions in exception paths
 * - Can cause latency spikes (10-100x slower)
 * 
 * Instead, we use:
 * 1. Error codes (fast, deterministic)
 * 2. Early returns (fail fast)
 * 3. Assertions in debug mode only
 * 4. Logging for post-mortem analysis
 * 
 * Pattern used by all major HFT firms:
 * - Jane Street
 * - Citadel
 * - Jump Trading
 * - Optiver
 * - Tower Research
 */

/**
 * Error codes for system operations
 * Keep it simple - success/failure is often enough
 */
enum class ErrorCode : uint8_t {
    SUCCESS = 0,
    
    // Network errors
    NETWORK_INIT_FAILED = 1,
    SOCKET_CREATE_FAILED = 2,
    SOCKET_BIND_FAILED = 3,
    SOCKET_RECV_FAILED = 4,
    
    // Memory errors
    MEMORY_POOL_EXHAUSTED = 10,
    ALLOCATION_FAILED = 11,
    
    // Protocol errors
    INVALID_PACKET = 20,
    SEQUENCE_GAP_TOO_LARGE = 21,
    FEED_STALE = 22,
    
    // Queue errors
    QUEUE_FULL = 30,
    QUEUE_EMPTY = 31,
    
    // System errors
    THREAD_AFFINITY_FAILED = 40,
    RT_PRIORITY_FAILED = 41,
    
    // Generic
    UNKNOWN_ERROR = 255
};

/**
 * Result type for operations that can fail
 * Simple, fast, no overhead
 */
template<typename T>
struct Result {
    T value;
    ErrorCode error;
    
    // Check if operation succeeded
    [[nodiscard]] bool ok() const noexcept {
        return error == ErrorCode::SUCCESS;
    }
    
    // Check if operation failed
    [[nodiscard]] bool is_error() const noexcept {
        return error != ErrorCode::SUCCESS;
    }
    
    // Get value (caller must check ok() first)
    [[nodiscard]] T& get() noexcept {
        return value;
    }
    
    [[nodiscard]] const T& get() const noexcept {
        return value;
    }
};

/**
 * Specialization for void (operation with no return value)
 */
template<>
struct Result<void> {
    ErrorCode error;
    
    [[nodiscard]] bool ok() const noexcept {
        return error == ErrorCode::SUCCESS;
    }
    
    [[nodiscard]] bool is_error() const noexcept {
        return error != ErrorCode::SUCCESS;
    }
};

/**
 * Helper to create success result
 */
template<typename T>
[[nodiscard]] Result<T> Ok(T value) noexcept {
    return Result<T>{value, ErrorCode::SUCCESS};
}

/**
 * Helper to create error result
 */
template<typename T>
[[nodiscard]] Result<T> Err(ErrorCode error) noexcept {
    return Result<T>{T{}, error};
}

/**
 * Helper for void operations
 */
[[nodiscard]] inline Result<void> Ok() noexcept {
    return Result<void>{ErrorCode::SUCCESS};
}

[[nodiscard]] inline Result<void> Err(ErrorCode error) noexcept {
    return Result<void>{error};
}

/**
 * Convert error code to string (for logging only, not hot path)
 */
inline const char* error_to_string(ErrorCode error) noexcept {
    switch (error) {
        case ErrorCode::SUCCESS: return "Success";
        case ErrorCode::NETWORK_INIT_FAILED: return "Network initialization failed";
        case ErrorCode::SOCKET_CREATE_FAILED: return "Socket creation failed";
        case ErrorCode::SOCKET_BIND_FAILED: return "Socket bind failed";
        case ErrorCode::SOCKET_RECV_FAILED: return "Socket receive failed";
        case ErrorCode::MEMORY_POOL_EXHAUSTED: return "Memory pool exhausted";
        case ErrorCode::ALLOCATION_FAILED: return "Allocation failed";
        case ErrorCode::INVALID_PACKET: return "Invalid packet";
        case ErrorCode::SEQUENCE_GAP_TOO_LARGE: return "Sequence gap too large";
        case ErrorCode::FEED_STALE: return "Feed stale";
        case ErrorCode::QUEUE_FULL: return "Queue full";
        case ErrorCode::QUEUE_EMPTY: return "Queue empty";
        case ErrorCode::THREAD_AFFINITY_FAILED: return "Thread affinity failed";
        case ErrorCode::RT_PRIORITY_FAILED: return "RT priority failed";
        default: return "Unknown error";
    }
}

/**
 * Debug assertions (compiled out in release builds)
 * Use for invariants that should never be violated
 */
#ifdef NDEBUG
    #define HFT_ASSERT(condition) ((void)0)
    #define HFT_ASSERT_MSG(condition, msg) ((void)0)
#else
    #include <cassert>
    #define HFT_ASSERT(condition) assert(condition)
    #define HFT_ASSERT_MSG(condition, msg) assert((condition) && (msg))
#endif

/**
 * Likely/Unlikely hints for branch prediction
 * Use to optimize hot paths
 */
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

} // namespace hft

