#pragma once

#include <cstdint>
#include <unordered_set>
#include <map>
#include <deque>
#include <vector>
#include <functional>

namespace hft {

/**
 * Feed State Machine
 * Industry standard: SIAC, CME, NASDAQ all use similar state models
 */
enum class FeedState : uint8_t {
    INITIAL,        // Starting up, waiting for snapshot
    RECOVERING,     // Gap detected, requesting recovery
    LIVE,           // Normal operation
    STALE,          // Too many gaps, need full resync
};

/**
 * Gap Recovery Request
 * Used to request retransmission from exchange/vendor
 */
struct GapFillRequest {
    uint64_t start_seq;
    uint64_t end_seq;
    uint64_t request_time_ns;
    uint8_t  retry_count;
};

/**
 * Packet Manager - Industry Standard Gap and Duplicate Handling
 * 
 * Features:
 * 1. Sequence gap detection with configurable threshold
 * 2. Duplicate packet filtering using sliding window
 * 3. Out-of-order packet buffering and resequencing
 * 4. Gap fill request generation
 * 5. Recovery feed integration
 * 6. Statistics tracking
 * 
 * Based on patterns from:
 * - NYSE Pillar Protocol
 * - NASDAQ TotalView-ITCH
 * - CME MDP 3.0
 * - OPRA (Options Price Reporting Authority)
 */
class PacketManager {
private:
    // Current state
    FeedState state_{FeedState::INITIAL};
    
    // Sequence tracking
    uint64_t next_expected_seq_{0};
    uint64_t highest_seq_seen_{0};
    
    // Duplicate detection - sliding window approach
    // Keep track of recent sequence numbers in a bit vector for O(1) lookup
    static constexpr size_t DUPLICATE_WINDOW_SIZE = 10000;
    std::deque<uint64_t> recent_sequences_;
    std::unordered_set<uint64_t> recent_seq_set_;
    
    // Out-of-order buffer - holds packets that arrive early
    // Key: sequence number, Value: packet data
    std::map<uint64_t, std::vector<uint8_t>> resequence_buffer_;
    static constexpr size_t MAX_RESEQUENCE_BUFFER_SIZE = 1000;
    
    // Gap tracking and recovery
    std::vector<GapFillRequest> pending_gaps_;
    static constexpr uint64_t MAX_GAP_SIZE = 1000;  // Beyond this, trigger full resync
    static constexpr uint64_t GAP_TIMEOUT_NS = 1000000000ULL; // 1 second
    static constexpr uint8_t MAX_RETRIES = 3;
    
    // Statistics
    struct Stats {
        alignas(64) uint64_t total_packets{0};
        alignas(64) uint64_t duplicates{0};
        alignas(64) uint64_t gaps_detected{0};
        alignas(64) uint64_t gaps_filled{0};
        alignas(64) uint64_t out_of_order{0};
        alignas(64) uint64_t resequenced{0};
        alignas(64) uint64_t dropped_overflow{0};
    } stats_;
    
    // Callback for gap fill requests
    std::function<void(const GapFillRequest&)> gap_fill_callback_;

public:
    PacketManager() = default;
    
    /**
     * Set callback for gap fill requests
     * In production: sends UDP request to recovery feed or TCP retransmission service
     */
    void set_gap_fill_callback(std::function<void(const GapFillRequest&)> callback) {
        gap_fill_callback_ = std::move(callback);
    }
    
    /**
     * Process incoming packet - main entry point
     * 
     * @param sequence Packet sequence number
     * @param data Packet data (optional, for buffering)
     * @param timestamp Current timestamp for gap timeout checking
     * @return true if packet should be processed now, false if duplicate/buffered
     */
    [[nodiscard]] bool process_packet(uint64_t sequence, 
                                       const uint8_t* data = nullptr, 
                                       size_t data_size = 0,
                                       uint64_t timestamp = 0) {
        stats_.total_packets++;
        
        // Update highest seen
        if (sequence > highest_seq_seen_) {
            highest_seq_seen_ = sequence;
        }
        
        // Check for duplicate - O(1) lookup in hash set
        if (is_duplicate(sequence)) {
            stats_.duplicates++;
            return false; // Drop duplicate
        }
        
        // Mark as seen in duplicate detection window
        mark_seen(sequence);
        
        // Handle based on current state
        switch (state_) {
            case FeedState::INITIAL:
                return handle_initial_state(sequence);
                
            case FeedState::LIVE:
                return handle_live_state(sequence, data, data_size, timestamp);
                
            case FeedState::RECOVERING:
                return handle_recovering_state(sequence, data, data_size);
                
            case FeedState::STALE:
                // In stale state, drop all incremental updates until resync
                return false;
        }
        
        return false;
    }
    
    /**
     * Check if we have buffered packets ready to process
     * Called after processing a packet to drain resequence buffer
     */
    std::vector<std::vector<uint8_t>> get_ready_packets() {
        std::vector<std::vector<uint8_t>> ready;
        
        // Check if next expected packet is in buffer
        while (!resequence_buffer_.empty()) {
            auto it = resequence_buffer_.find(next_expected_seq_);
            if (it == resequence_buffer_.end()) {
                break; // Next packet not available yet
            }
            
            // Found next packet in sequence
            ready.push_back(std::move(it->second));
            resequence_buffer_.erase(it);
            next_expected_seq_++;
            stats_.resequenced++;
        }
        
        return ready;
    }
    
    /**
     * Process gap fill response
     * Called when recovery feed provides missing packets
     */
    void process_gap_fill(uint64_t start_seq, uint64_t end_seq) {
        // Remove from pending gaps
        pending_gaps_.erase(
            std::remove_if(pending_gaps_.begin(), pending_gaps_.end(),
                [start_seq, end_seq](const GapFillRequest& req) {
                    return req.start_seq == start_seq && req.end_seq == end_seq;
                }),
            pending_gaps_.end()
        );
        
        stats_.gaps_filled++;
        
        // If all gaps filled, return to LIVE state
        if (pending_gaps_.empty() && state_ == FeedState::RECOVERING) {
            state_ = FeedState::LIVE;
        }
    }
    
    /**
     * Periodic maintenance - check for gap timeouts and retries
     * Should be called periodically (e.g., every 100ms)
     */
    void periodic_maintenance(uint64_t current_time_ns) {
        for (auto& gap : pending_gaps_) {
            if (current_time_ns - gap.request_time_ns > GAP_TIMEOUT_NS) {
                if (gap.retry_count < MAX_RETRIES) {
                    // Retry gap fill request
                    gap.retry_count++;
                    gap.request_time_ns = current_time_ns;
                    
                    if (gap_fill_callback_) {
                        gap_fill_callback_(gap);
                    }
                } else {
                    // Too many retries - transition to STALE
                    state_ = FeedState::STALE;
                    // In production: trigger full snapshot refresh
                }
            }
        }
    }
    
    /**
     * Force resync - used after STALE or on startup
     * Clears all buffers and resets state
     */
    void trigger_resync() {
        state_ = FeedState::INITIAL;
        resequence_buffer_.clear();
        pending_gaps_.clear();
        recent_sequences_.clear();
        recent_seq_set_.clear();
    }
    
    /**
     * Get current state
     */
    [[nodiscard]] FeedState get_state() const noexcept {
        return state_;
    }
    
    /**
     * Get statistics
     */
    const Stats& get_stats() const noexcept {
        return stats_;
    }
    
    /**
     * Get expected sequence number
     */
    [[nodiscard]] uint64_t get_next_expected() const noexcept {
        return next_expected_seq_;
    }

private:
    /**
     * Initial state - waiting for first packet or snapshot
     * Accept any sequence number and start from there
     */
    bool handle_initial_state(uint64_t sequence) {
        next_expected_seq_ = sequence + 1;
        state_ = FeedState::LIVE;
        return true; // Process this packet
    }
    
    /**
     * Live state - normal operation with gap detection
     */
    bool handle_live_state(uint64_t sequence, const uint8_t* data, size_t data_size, uint64_t timestamp) {
        if (sequence == next_expected_seq_) {
            // Perfect - in sequence
            next_expected_seq_++;
            return true;
            
        } else if (sequence < next_expected_seq_) {
            // Old packet (already processed or duplicate)
            // Duplicate check should have caught this, but double check
            return false;
            
        } else {
            // Gap detected: sequence > next_expected_seq_
            const uint64_t gap_size = sequence - next_expected_seq_;
            stats_.gaps_detected++;
            
            if (gap_size > MAX_GAP_SIZE) {
                // Gap too large - transition to STALE
                state_ = FeedState::STALE;
                // In production: request snapshot refresh
                return false;
            }
            
            // Create gap fill request
            GapFillRequest gap_req{
                .start_seq = next_expected_seq_,
                .end_seq = sequence - 1,
                .request_time_ns = timestamp,
                .retry_count = 0
            };
            
            pending_gaps_.push_back(gap_req);
            
            // Send gap fill request
            if (gap_fill_callback_) {
                gap_fill_callback_(gap_req);
            }
            
            // Transition to RECOVERING state
            state_ = FeedState::RECOVERING;
            
            // Buffer this packet if data provided
            if (data && data_size > 0) {
                buffer_packet(sequence, data, data_size);
                stats_.out_of_order++;
            }
            
            return false; // Don't process yet, wait for gap fill
        }
    }
    
    /**
     * Recovering state - buffering out-of-order packets while waiting for gap fill
     */
    bool handle_recovering_state(uint64_t sequence, const uint8_t* data, size_t data_size) {
        if (sequence == next_expected_seq_) {
            // Gap was filled! Process this packet
            next_expected_seq_++;
            
            // Check if all gaps filled
            if (pending_gaps_.empty()) {
                state_ = FeedState::LIVE;
            }
            
            return true;
            
        } else if (sequence > next_expected_seq_) {
            // Still ahead - buffer it
            if (data && data_size > 0) {
                buffer_packet(sequence, data, data_size);
                stats_.out_of_order++;
            }
            return false;
            
        } else {
            // Old packet - likely from gap fill
            // Check if this fills a gap
            for (auto it = pending_gaps_.begin(); it != pending_gaps_.end(); ) {
                if (sequence >= it->start_seq && sequence <= it->end_seq) {
                    // This is part of a gap being filled
                    if (sequence == it->end_seq) {
                        // Gap completely filled
                        process_gap_fill(it->start_seq, it->end_seq);
                    }
                    return true; // Process gap fill packet
                }
                ++it;
            }
            
            return false; // Old packet, not part of gap
        }
    }
    
    /**
     * Check if sequence number is a duplicate
     * Uses sliding window with hash set for O(1) lookup
     */
    bool is_duplicate(uint64_t sequence) const noexcept {
        return recent_seq_set_.find(sequence) != recent_seq_set_.end();
    }
    
    /**
     * Mark sequence as seen in duplicate detection window
     * Maintains sliding window of recent sequences
     */
    void mark_seen(uint64_t sequence) {
        // Add to set
        recent_seq_set_.insert(sequence);
        recent_sequences_.push_back(sequence);
        
        // Maintain window size
        if (recent_sequences_.size() > DUPLICATE_WINDOW_SIZE) {
            uint64_t old_seq = recent_sequences_.front();
            recent_sequences_.pop_front();
            recent_seq_set_.erase(old_seq);
        }
    }
    
    /**
     * Buffer out-of-order packet for later processing
     */
    void buffer_packet(uint64_t sequence, const uint8_t* data, size_t data_size) {
        // Check buffer size limit
        if (resequence_buffer_.size() >= MAX_RESEQUENCE_BUFFER_SIZE) {
            // Buffer full - drop oldest or this packet
            // Strategy: drop oldest to make room
            resequence_buffer_.erase(resequence_buffer_.begin());
            stats_.dropped_overflow++;
        }
        
        // Copy packet data into buffer
        std::vector<uint8_t> packet_copy(data, data + data_size);
        resequence_buffer_[sequence] = std::move(packet_copy);
    }
};

/**
 * Recovery Feed Manager
 * Handles connection to separate recovery/snapshot feed
 * 
 * Most exchanges provide:
 * 1. Primary incremental feed (low latency, UDP multicast)
 * 2. Recovery feed (TCP or UDP, for gap fills)
 * 3. Snapshot feed (full book state, periodic or on-demand)
 */
class RecoveryFeedManager {
private:
    // In production: TCP connection to recovery server
    // For example: CME MDP 3.0 has separate TCP Replay channel
    
public:
    /**
     * Request retransmission of specific sequence range
     * 
     * Example protocol (simplified):
     * - Send: "RETRANSMIT <start_seq> <end_seq>\n"
     * - Receive: Requested packets via TCP or dedicated UDP stream
     */
    void request_retransmission(uint64_t start_seq, uint64_t end_seq) {
        // In production: send TCP request to exchange recovery service
        // Format varies by exchange:
        // - CME: Binary request via MDP Replay
        // - NASDAQ: MOLD UDP retransmit request
        // - NYSE: Pillar Retransmission
        
        (void)start_seq;
        (void)end_seq;
        // TODO: Implement actual recovery protocol
    }
    
    /**
     * Request snapshot (full book state)
     * Used when gaps are too large or feed becomes stale
     */
    void request_snapshot(uint32_t symbol_id = 0) {
        // In production: 
        // - Connect to snapshot server
        // - Request full order book dump
        // - Apply incremental updates after snapshot sequence
        
        (void)symbol_id;
        // TODO: Implement snapshot protocol
    }
};

} // namespace hft

