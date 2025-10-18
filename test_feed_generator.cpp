#include "types.hpp"
#include "utils.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <random>
#include <cstring>

using namespace hft;

/**
 * Test Feed Generator
 * 
 * Simulates market data feed with configurable:
 * - Packet rate
 * - Gap injection (to test gap detection)
 * - Duplicate injection (to test duplicate filtering)
 * - Out-of-order delivery (to test resequencing)
 * 
 * Usage:
 *   ./test_feed_generator [multicast_ip] [port] [packets_per_second]
 */
class TestFeedGenerator {
private:
    int socket_fd_{-1};
    sockaddr_in dest_addr_{};
    uint64_t sequence_{1};
    
    // Configurable parameters
    double gap_probability_{0.001};      // 0.1% chance of gap
    double duplicate_probability_{0.002}; // 0.2% chance of duplicate
    double reorder_probability_{0.005};   // 0.5% chance of reorder
    
    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<double> dist_{0.0, 1.0};

public:
    bool initialize(const std::string& multicast_ip, uint16_t port) {
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd_ < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }
        
        // Set multicast TTL
        int ttl = 1;
        if (setsockopt(socket_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            std::cerr << "Failed to set TTL" << std::endl;
            return false;
        }
        
        // Setup destination address
        dest_addr_.sin_family = AF_INET;
        dest_addr_.sin_port = htons(port);
        inet_pton(AF_INET, multicast_ip.c_str(), &dest_addr_.sin_addr);
        
        std::cout << "[Generator] Initialized. Sending to " 
                  << multicast_ip << ":" << port << std::endl;
        std::cout << "[Generator] Gap probability: " << (gap_probability_ * 100) << "%" << std::endl;
        std::cout << "[Generator] Duplicate probability: " << (duplicate_probability_ * 100) << "%" << std::endl;
        std::cout << "[Generator] Reorder probability: " << (reorder_probability_ * 100) << "%" << std::endl;
        
        return true;
    }
    
    void set_gap_probability(double prob) { gap_probability_ = prob; }
    void set_duplicate_probability(double prob) { duplicate_probability_ = prob; }
    void set_reorder_probability(double prob) { reorder_probability_ = prob; }
    
    /**
     * Send packets at specified rate
     */
    void run(uint32_t packets_per_second, uint32_t total_packets = 0) {
        const auto interval = std::chrono::microseconds(1000000 / packets_per_second);
        
        uint64_t packets_sent = 0;
        uint64_t gaps_injected = 0;
        uint64_t duplicates_sent = 0;
        uint64_t reordered = 0;
        
        MarketDataPacket reorder_buffer;
        bool has_buffered = false;
        
        std::cout << "[Generator] Starting packet generation at " 
                  << packets_per_second << " packets/sec" << std::endl;
        
        auto start_time = std::chrono::steady_clock::now();
        
        while (total_packets == 0 || packets_sent < total_packets) {
            auto send_time = start_time + (interval * packets_sent);
            std::this_thread::sleep_until(send_time);
            
            // Create packet
            MarketDataPacket packet;
            create_market_packet(packet, sequence_);
            
            // Decide on anomalies
            double rand_val = dist_(rng_);
            
            // Gap injection
            if (rand_val < gap_probability_) {
                uint64_t gap_size = 1 + (rng_() % 10); // Gap of 1-10 packets
                std::cout << "[Generator] INJECTING GAP: skipping " << gap_size 
                          << " sequences (from " << sequence_ << " to " 
                          << (sequence_ + gap_size) << ")" << std::endl;
                sequence_ += gap_size;
                gaps_injected++;
                packet.packet_sequence = sequence_;
            }
            // Duplicate injection
            else if (rand_val < gap_probability_ + duplicate_probability_) {
                // Send previous packet again (if not first)
                if (sequence_ > 1) {
                    packet.packet_sequence = sequence_ - 1;
                    std::cout << "[Generator] SENDING DUPLICATE: seq " 
                              << packet.packet_sequence << std::endl;
                    duplicates_sent++;
                }
            }
            // Reorder injection
            else if (rand_val < gap_probability_ + duplicate_probability_ + reorder_probability_) {
                if (has_buffered) {
                    // Send buffered packet (out of order)
                    send_packet(reorder_buffer);
                    std::cout << "[Generator] SENDING REORDERED: seq " 
                              << reorder_buffer.packet_sequence 
                              << " (should be before " << sequence_ << ")" << std::endl;
                    reordered++;
                    has_buffered = false;
                } else {
                    // Buffer this packet, send next one first
                    reorder_buffer = packet;
                    has_buffered = true;
                    sequence_++;
                    packets_sent++;
                    continue; // Don't send yet
                }
            }
            
            // Send packet
            send_packet(packet);
            
            sequence_++;
            packets_sent++;
            
            // Periodic stats
            if (packets_sent % 10000 == 0) {
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                double actual_rate = elapsed_sec > 0 ? static_cast<double>(packets_sent) / elapsed_sec : 0;
                
                std::cout << "[Generator] Sent: " << packets_sent 
                          << ", Rate: " << static_cast<int>(actual_rate) << " pps"
                          << ", Gaps: " << gaps_injected
                          << ", Duplicates: " << duplicates_sent
                          << ", Reordered: " << reordered
                          << std::endl;
            }
        }
        
        // Send any buffered packet
        if (has_buffered) {
            send_packet(reorder_buffer);
        }
        
        std::cout << "[Generator] Complete. Total packets: " << packets_sent 
                  << ", Gaps: " << gaps_injected
                  << ", Duplicates: " << duplicates_sent
                  << ", Reordered: " << reordered
                  << std::endl;
    }
    
    ~TestFeedGenerator() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
        }
    }

private:
    void create_market_packet(MarketDataPacket& packet, uint64_t seq) {
        std::memset(&packet, 0, sizeof(packet));
        
        packet.msg_type = MessageType::TRADE;
        packet.version = 1;
        packet.payload_size = sizeof(TradeMessage);
        packet.packet_sequence = seq;
        
        // Fill in trade data
        auto& trade = packet.payload.trade;
        trade.timestamp_ns = LatencyTracker::rdtsc();
        trade.sequence_num = seq;
        trade.symbol_id = 12345; // AAPL
        trade.trade_id = seq;
        trade.price = 1500000 + (rng_() % 10000); // $150.00 +/- $1.00
        trade.quantity = 100 + (rng_() % 1000);
        trade.side = (rng_() % 2 == 0) ? 'B' : 'S';
    }
    
    void send_packet(const MarketDataPacket& packet) {
        ssize_t bytes = sendto(socket_fd_, &packet, sizeof(packet), 0,
                              (struct sockaddr*)&dest_addr_, sizeof(dest_addr_));
        if (bytes < 0) {
            std::cerr << "[Generator] Send failed: " << strerror(errno) << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║         HFT TEST FEED GENERATOR                             ║
║         Gap & Duplicate Injection                           ║
╚══════════════════════════════════════════════════════════════╝
    )" << std::endl;
    
    // Parse arguments
    std::string multicast_ip = "233.54.12.1";
    uint16_t port = 15000;
    uint32_t packets_per_second = 10000;
    uint32_t total_packets = 0; // 0 = infinite
    
    if (argc > 1) multicast_ip = argv[1];
    if (argc > 2) port = std::stoi(argv[2]);
    if (argc > 3) packets_per_second = std::stoi(argv[3]);
    if (argc > 4) total_packets = std::stoi(argv[4]);
    
    TestFeedGenerator generator;
    
    if (!generator.initialize(multicast_ip, port)) {
        return 1;
    }
    
    // Optional: customize probabilities via command line or config
    // generator.set_gap_probability(0.01); // 1% gaps
    // generator.set_duplicate_probability(0.02); // 2% duplicates
    
    std::cout << "\n[Main] Starting feed generation..." << std::endl;
    std::cout << "[Main] Press Ctrl+C to stop" << std::endl;
    std::cout << "\n[Main] Usage: " << argv[0] 
              << " [multicast_ip] [port] [packets_per_sec] [total_packets]" << std::endl;
    std::cout << "[Main] Example: " << argv[0] << " 233.54.12.1 15000 10000 100000\n" << std::endl;
    
    try {
        generator.run(packets_per_second, total_packets);
    } catch (const std::exception& e) {
        std::cerr << "[Main] Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

