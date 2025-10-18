#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

// Cache line size for x86_64 - critical for avoiding false sharing
static constexpr size_t CACHE_LINE_SIZE = 64;

// Align to cache line to prevent false sharing between producer and consumer
#define CACHE_LINE_ALIGNED alignas(CACHE_LINE_SIZE)

namespace hft {

/**
 * Lock-free Single Producer Single Consumer (SPSC) Ring Buffer
 * 
 * Design principles:
 * - Producer and consumer indices on separate cache lines (no false sharing)
 * - Power-of-2 size for fast modulo operations (bitwise AND instead of %)
 * - Memory ordering optimizations (relaxed where safe, acquire/release for synchronization)
 * - No dynamic allocation in hot path
 * - Templated for zero-cost abstraction
 * 
 * This is production-grade pattern used in HFT shops like Jane Street, Citadel, Jump Trading
 */
template<typename T, size_t Size>
class SPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

private:
    // Ring buffer storage - aligned to cache line
    alignas(CACHE_LINE_SIZE) T buffer_[Size];
    
    // Size mask for fast modulo (Size - 1)
    static constexpr size_t SIZE_MASK = Size - 1;
    
    // Producer cache line - only written by producer
    CACHE_LINE_ALIGNED std::atomic<uint64_t> write_pos_{0};
    
    // Padding to ensure consumer variables are on different cache line
    CACHE_LINE_ALIGNED std::atomic<uint64_t> read_pos_{0};
    
    // Cached positions to reduce atomic operations
    // Producer caches last known read position
    CACHE_LINE_ALIGNED uint64_t cached_read_pos_{0};
    
    // Consumer caches last known write position  
    CACHE_LINE_ALIGNED uint64_t cached_write_pos_{0};

public:
    SPSCQueue() = default;
    
    // Non-copyable, non-movable (contains atomics)
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    
    /**
     * Try to push item into queue (producer side)
     * 
     * @param item Item to push
     * @return true if successful, false if queue is full
     * 
     * Uses memory_order_relaxed for read since we only need to see our own writes
     * Uses memory_order_release for write to ensure item is visible to consumer
     */
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const uint64_t current_write = write_pos_.load(std::memory_order_relaxed);
        const uint64_t next_write = current_write + 1;
        
        // Check if queue is full - use cached read position first
        if (next_write - cached_read_pos_ > Size) {
            // Cache miss - reload actual read position
            cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
            
            if (next_write - cached_read_pos_ > Size) {
                return false; // Queue is full
            }
        }
        
        // Write data to buffer
        buffer_[current_write & SIZE_MASK] = item;
        
        // Release write position - ensures item is visible before position update
        write_pos_.store(next_write, std::memory_order_release);
        
        return true;
    }
    
    /**
     * Try to pop item from queue (consumer side)
     * 
     * @param item Reference to store popped item
     * @return true if successful, false if queue is empty
     * 
     * Uses memory_order_acquire for read to see producer's writes
     * Uses memory_order_release for position update
     */
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const uint64_t current_read = read_pos_.load(std::memory_order_relaxed);
        
        // Check if queue is empty - use cached write position first
        if (current_read >= cached_write_pos_) {
            // Cache miss - reload actual write position
            cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
            
            if (current_read >= cached_write_pos_) {
                return false; // Queue is empty
            }
        }
        
        // Read data from buffer
        item = buffer_[current_read & SIZE_MASK];
        
        // Release read position - ensures item read before position update
        read_pos_.store(current_read + 1, std::memory_order_release);
        
        return true;
    }
    
    /**
     * Get approximate size (may be stale, but safe)
     */
    [[nodiscard]] size_t size() const noexcept {
        const uint64_t write = write_pos_.load(std::memory_order_acquire);
        const uint64_t read = read_pos_.load(std::memory_order_acquire);
        return write - read;
    }
    
    /**
     * Check if queue is empty (may be stale)
     */
    [[nodiscard]] bool empty() const noexcept {
        return read_pos_.load(std::memory_order_acquire) >= 
               write_pos_.load(std::memory_order_acquire);
    }
    
    /**
     * Get queue capacity
     */
    [[nodiscard]] constexpr size_t capacity() const noexcept {
        return Size;
    }
};

} // namespace hft

