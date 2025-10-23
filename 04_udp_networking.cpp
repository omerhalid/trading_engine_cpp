/**
 * LESSON 4: UDP Networking for Market Data
 * 
 * Learn how HFT systems receive market data:
 * - UDP multicast (fastest delivery from exchanges)
 * - Non-blocking I/O (never wait for packets)
 * - Socket optimizations (buffer sizes, timestamping)
 * - Busy polling (check for data constantly)
 * 
 * Why UDP not TCP?
 * - TCP: retransmits, ordering, flow control = 10-50us latency
 * - UDP: fire-and-forget = 1-5us latency
 * - Exchanges use UDP multicast for primary feed
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <iostream>
#include <cstdint>
#include <immintrin.h>

// ============================================================================
// CONCEPT: Non-blocking UDP Receiver
// ============================================================================

class UDPReceiver {
private:
    int socket_fd_{-1};
    
    // Aligned receive buffer (helps with DMA)
    alignas(4096) uint8_t recv_buffer_[65536];
    
public:
    /**
     * Initialize UDP socket with HFT optimizations
     */
    bool init(const char* multicast_ip, uint16_t port) {
        // Step 1: Create UDP socket
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd_ < 0) {
            std::cerr << "socket() failed\n";
            return false;
        }
        
        // Step 2: Set NON-BLOCKING mode
        // This is CRITICAL: we never want to block waiting for data
        int flags = fcntl(socket_fd_, F_GETFL, 0);
        fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
        
        // Step 3: Increase receive buffer (reduce packet drops during bursts)
        int buffer_size = 16 * 1024 * 1024;  // 16MB
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
        
        // Step 4: Allow address reuse (multiple processes can listen)
        int reuse = 1;
        setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        // Step 5: Bind to port
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "bind() failed\n";
            close(socket_fd_);
            return false;
        }
        
        // Step 6: Join multicast group (if multicast IP provided)
        if (multicast_ip && strlen(multicast_ip) > 0) {
            struct ip_mreq mreq{};
            inet_pton(AF_INET, multicast_ip, &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = INADDR_ANY;
            
            if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                std::cerr << "Multicast join failed\n";
                close(socket_fd_);
                return false;
            }
        }
        
        std::cout << "[UDP] Initialized on port " << port << "\n";
        return true;
    }
    
    /**
     * Non-blocking receive - returns immediately if no data
     */
    ssize_t receive(uint8_t* buffer, size_t max_size) noexcept {
        // MSG_DONTWAIT = non-blocking (returns immediately)
        ssize_t bytes = recvfrom(socket_fd_, buffer, max_size, MSG_DONTWAIT, nullptr, nullptr);
        
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // No data available (this is normal!)
            }
            return -1;  // Actual error
        }
        
        return bytes;
    }
    
    /**
     * Optimized: receive into internal buffer (avoid copy)
     */
    ssize_t receive_internal(uint8_t*& buffer_ptr) noexcept {
        ssize_t bytes = receive(recv_buffer_, sizeof(recv_buffer_));
        if (bytes > 0) {
            buffer_ptr = recv_buffer_;
        }
        return bytes;
    }
    
    ~UDPReceiver() {
        if (socket_fd_ >= 0) close(socket_fd_);
    }
};

// ============================================================================
// SIMPLE MARKET DATA PACKET
// ============================================================================

struct __attribute__((packed)) MarketPacket {
    uint64_t sequence;   // For gap detection
    uint64_t timestamp;
    uint32_t symbol_id;
    uint64_t price;
    uint32_t quantity;
    uint8_t  side;       // 'B' or 'S'
};

// ============================================================================
// DEMO: Busy-poll UDP receiver
// ============================================================================

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

int main() {
    std::cout << "=== UDP NETWORKING FOR HFT ===\n\n";
    
    // Listen on localhost for testing
    UDPReceiver receiver;
    if (!receiver.init(nullptr, 15000)) {
        return 1;
    }
    
    std::cout << "Listening on port 15000...\n";
    std::cout << "Send test packet with:\n";
    std::cout << "  echo 'test' | nc -u localhost 15000\n\n";
    std::cout << "Busy polling for 5 seconds...\n";
    
    uint64_t iterations = 0;
    uint64_t packets_received = 0;
    uint64_t start_time = rdtsc();
    uint64_t end_time = start_time + (5ULL * 3000000000ULL);  // 5 seconds at 3GHz
    
    while (rdtsc() < end_time) {
        uint8_t* buffer = nullptr;
        ssize_t bytes = receiver.receive_internal(buffer);
        
        if (bytes > 0) {
            packets_received++;
            std::cout << "  Received packet (" << bytes << " bytes)\n";
            
            // In real system: parse packet here
            if (bytes >= sizeof(MarketPacket)) {
                auto* pkt = reinterpret_cast<MarketPacket*>(buffer);
                std::cout << "    Seq: " << pkt->sequence 
                          << ", Symbol: " << pkt->symbol_id 
                          << ", Price: " << pkt->price << "\n";
            }
        } else if (bytes == 0) {
            // No data - spin wait
            _mm_pause();
        }
        
        iterations++;
    }
    
    uint64_t total_time = rdtsc() - start_time;
    double iterations_per_sec = (double)iterations / 5.0;
    
    std::cout << "\nStatistics:\n";
    std::cout << "  Total iterations: " << iterations << "\n";
    std::cout << "  Iterations/sec: " << (uint64_t)iterations_per_sec << "\n";
    std::cout << "  Packets received: " << packets_received << "\n";
    std::cout << "  Cycles/iteration: " << (total_time / iterations) << "\n";
    
    std::cout << "\nKEY LEARNINGS:\n";
    std::cout << "  • UDP = low latency, no retransmits\n";
    std::cout << "  • Non-blocking = never wait for data\n";
    std::cout << "  • Busy polling = check constantly (millions of times/sec)\n";
    std::cout << "  • Multicast = one feed, many receivers\n";
    std::cout << "  • In production: kernel bypass (Solarflare) for 200-500ns\n";
    
    return 0;
}

