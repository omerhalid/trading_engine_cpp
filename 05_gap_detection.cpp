/**
 * LESSON 5: Gap Detection & Duplicate Filtering
 * 
 * Network problems in HFT:
 * - Packet loss (gaps in sequence numbers)
 * - Packet duplication (network retransmits)
 * - Out-of-order delivery
 * 
 * Industry standard solutions:
 * - Sequence number tracking (every packet numbered)
 * - Gap detection and recovery (request missing packets)
 * - Duplicate filtering (sliding window)
 * - Packet resequencing (buffer out-of-order packets)
 * 
 * Used by: NASDAQ, NYSE, CME, all major exchanges
 */

#include <cstdint>
#include <iostream>
#include <unordered_set>
#include <deque>
#include <map>
#include <vector>

// ============================================================================
// CONCEPT 1: Simple Gap Detector
// ============================================================================

class SimpleGapDetector {
private:
    uint64_t next_expected_{0};
    bool initialized_{false};
    
public:
    /**
     * Process packet sequence number
     * Returns: true if packet is in-order and should be processed
     */
    bool process(uint64_t sequence) {
        if (!initialized_) {
            // First packet - accept any sequence
            next_expected_ = sequence + 1;
            initialized_ = true;
            return true;
        }
        
        if (sequence == next_expected_) {
            // Perfect - in sequence
            next_expected_++;
            return true;
        }
        else if (sequence < next_expected_) {
            // Old packet (duplicate or late arrival)
            std::cout << "  [DUP] Sequence " << sequence 
                      << " (expected " << next_expected_ << ")\n";
            return false;
        }
        else {
            // Gap detected!
            std::cout << "  [GAP] Expected " << next_expected_ 
                      << ", got " << sequence 
                      << " (gap size: " << (sequence - next_expected_) << ")\n";
            // In production: request retransmission of missing packets
            next_expected_ = sequence + 1;
            return true;
        }
    }
};

// ============================================================================
// CONCEPT 2: Duplicate Filter with Sliding Window
// ============================================================================

class DuplicateFilter {
private:
    static constexpr size_t WINDOW_SIZE = 10000;
    
    std::deque<uint64_t> recent_seqs_;
    std::unordered_set<uint64_t> seq_set_;  // O(1) lookup
    
public:
    /**
     * Check if sequence number is duplicate
     */
    bool is_duplicate(uint64_t sequence) {
        // O(1) lookup in hash set
        if (seq_set_.find(sequence) != seq_set_.end()) {
            return true;  // Duplicate!
        }
        
        // Add to window
        seq_set_.insert(sequence);
        recent_seqs_.push_back(sequence);
        
        // Maintain window size
        if (recent_seqs_.size() > WINDOW_SIZE) {
            uint64_t old = recent_seqs_.front();
            recent_seqs_.pop_front();
            seq_set_.erase(old);
        }
        
        return false;  // Not a duplicate
    }
};

// ============================================================================
// CONCEPT 3: Packet Resequencing Buffer
// ============================================================================

class ResequenceBuffer {
private:
    std::map<uint64_t, std::vector<uint8_t>> buffer_;  // seq -> packet data
    uint64_t next_expected_{0};
    static constexpr size_t MAX_BUFFER_SIZE = 1000;
    
public:
    void set_next_expected(uint64_t seq) {
        next_expected_ = seq;
    }
    
    /**
     * Try to buffer out-of-order packet
     */
    bool buffer_packet(uint64_t sequence, const uint8_t* data, size_t size) {
        if (sequence < next_expected_) {
            return false;  // Too old
        }
        
        if (buffer_.size() >= MAX_BUFFER_SIZE) {
            std::cout << "  [WARN] Resequence buffer full!\n";
            return false;  // Buffer full
        }
        
        // Store packet
        buffer_[sequence] = std::vector<uint8_t>(data, data + size);
        std::cout << "  [BUFFER] Stored seq " << sequence 
                  << " (waiting for " << next_expected_ << ")\n";
        return true;
    }
    
    /**
     * Get ready packets (sequences that can now be processed)
     */
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> get_ready() {
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> ready;
        
        // Check if next expected packet is buffered
        while (!buffer_.empty()) {
            auto it = buffer_.find(next_expected_);
            if (it == buffer_.end()) {
                break;  // Next packet not available yet
            }
            
            std::cout << "  [RESEQUENCE] Processing buffered seq " << next_expected_ << "\n";
            ready.push_back({it->first, std::move(it->second)});
            buffer_.erase(it);
            next_expected_++;
        }
        
        return ready;
    }
};

// ============================================================================
// COMBINED: Full Packet Manager
// ============================================================================

class PacketManager {
private:
    SimpleGapDetector gap_detector_;
    DuplicateFilter dup_filter_;
    ResequenceBuffer reseq_buffer_;
    
    uint64_t packets_processed_{0};
    uint64_t duplicates_filtered_{0};
    uint64_t gaps_detected_{0};
    uint64_t packets_resequenced_{0};
    
public:
    /**
     * Process incoming packet
     * Returns: true if should process now, false if duplicate/buffered
     */
    bool process_packet(uint64_t sequence, const uint8_t* data = nullptr, size_t size = 0) {
        packets_processed_++;
        
        // Step 1: Check for duplicate
        if (dup_filter_.is_duplicate(sequence)) {
            duplicates_filtered_++;
            return false;
        }
        
        // Step 2: Check sequence
        if (!gap_detector_.process(sequence)) {
            return false;  // Duplicate caught by gap detector
        }
        
        // Step 3: If gap was detected, buffer this packet
        if (sequence > reseq_buffer_.next_expected_) {
            gaps_detected_++;
            if (data && size > 0) {
                reseq_buffer_.buffer_packet(sequence, data, size);
            }
            return false;
        }
        
        return true;  // Process this packet
    }
    
    /**
     * Get buffered packets that are now ready
     */
    auto get_ready_packets() {
        auto ready = reseq_buffer_.get_ready();
        packets_resequenced_ += ready.size();
        return ready;
    }
    
    void print_stats() const {
        std::cout << "\nPacket Manager Stats:\n";
        std::cout << "  Processed: " << packets_processed_ << "\n";
        std::cout << "  Duplicates: " << duplicates_filtered_ << "\n";
        std::cout << "  Gaps: " << gaps_detected_ << "\n";
        std::cout << "  Resequenced: " << packets_resequenced_ << "\n";
    }
};

// ============================================================================
// DEMO: Simulate packet arrival patterns
// ============================================================================

int main() {
    std::cout << "=== GAP DETECTION & DUPLICATE FILTERING ===\n\n";
    
    PacketManager mgr;
    
    std::cout << "Simulating packet arrivals:\n\n";
    
    // Normal sequence
    std::cout << "1. Normal sequence (1, 2, 3):\n";
    mgr.process_packet(1);
    mgr.process_packet(2);
    mgr.process_packet(3);
    
    // Duplicate
    std::cout << "\n2. Duplicate packet (2 again):\n";
    mgr.process_packet(2);
    
    // Gap
    std::cout << "\n3. Gap in sequence (4, 5, 10 - missing 6,7,8,9):\n";
    mgr.process_packet(4);
    mgr.process_packet(5);
    mgr.process_packet(10);  // Gap!
    
    // Late arrivals (fill the gap)
    std::cout << "\n4. Late arrivals (6, 7, 8, 9):\n";
    uint8_t dummy_data[64] = {0};
    mgr.process_packet(6, dummy_data, sizeof(dummy_data));
    mgr.process_packet(7, dummy_data, sizeof(dummy_data));
    mgr.process_packet(8, dummy_data, sizeof(dummy_data));
    mgr.process_packet(9, dummy_data, sizeof(dummy_data));
    
    // Check for resequenced packets
    std::cout << "\n5. Get ready packets:\n";
    auto ready = mgr.get_ready_packets();
    std::cout << "  Retrieved " << ready.size() << " buffered packets\n";
    
    // More packets
    std::cout << "\n6. Continue sequence (11, 12):\n";
    mgr.process_packet(11);
    mgr.process_packet(12);
    
    // Duplicate again
    std::cout << "\n7. Another duplicate (11):\n";
    mgr.process_packet(11);
    
    mgr.print_stats();
    
    std::cout << "\nKEY LEARNINGS:\n";
    std::cout << "  • Every packet has sequence number\n";
    std::cout << "  • Gap = missing sequence(s)\n";
    std::cout << "  • Duplicate = sequence seen before\n";
    std::cout << "  • Buffering allows resequencing\n";
    std::cout << "  • In production: request retransmission on gap\n";
    std::cout << "  • Pattern used by ALL major exchanges\n";
    
    return 0;
}

