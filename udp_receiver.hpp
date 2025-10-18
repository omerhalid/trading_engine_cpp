#pragma once

#include "feed_handler.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <string>

namespace hft {

/**
 * Kernel Bypass UDP Receiver
 * 
 * In production HFT systems, you would use:
 * - Solarflare OpenOnload (ef_vi API) - most common in HFT
 * - Intel DPDK - more common in networking/NFV
 * - Mellanox/NVIDIA VMA
 * - Exablaze ExaNIC
 * 
 * These bypass the kernel network stack entirely, providing:
 * - ~200-500ns latency (vs 5-10us for kernel)
 * - Zero-copy DMA directly to user space
 * - No system calls in hot path
 * - No interrupts (poll mode)
 * 
 * This implementation uses standard POSIX sockets with optimizations.
 * Comments indicate where kernel bypass APIs would differ.
 */
class UDPReceiver {
private:
    int socket_fd_{-1};
    sockaddr_in local_addr_{};
    sockaddr_in remote_addr_{};
    
    // Receive buffer - aligned for DMA
    // In kernel bypass, this would be memory-mapped from NIC
    alignas(4096) uint8_t recv_buffer_[65536];
    
    static constexpr int RECV_BUFFER_SIZE = sizeof(recv_buffer_);

public:
    /**
     * Initialize UDP socket with optimizations
     * 
     * @param multicast_ip Multicast group (e.g., "233.54.12.1")
     * @param port Port number
     * @param interface_ip Local interface IP (for multicast join)
     */
    bool initialize(const std::string& multicast_ip, 
                   uint16_t port,
                   const std::string& interface_ip = "0.0.0.0") noexcept {
        
        // Create UDP socket
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd_ < 0) {
            return false;
        }
        
        // Set socket to non-blocking mode - critical for low latency
        // No blocking system calls in hot path
        int flags = fcntl(socket_fd_, F_GETFL, 0);
        if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(socket_fd_);
            return false;
        }
        
        // Enable SO_REUSEADDR - allow multiple processes to bind
        int reuse = 1;
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, 
                      &reuse, sizeof(reuse)) < 0) {
            close(socket_fd_);
            return false;
        }
        
        // Increase socket receive buffer size
        // Reduces packet drops during bursts
        // In production: 64MB+ buffers are common
        int buffer_size = 16 * 1024 * 1024; // 16MB
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, 
                  &buffer_size, sizeof(buffer_size));
        
        // Set SO_TIMESTAMP or SO_TIMESTAMPNS for kernel timestamps
        // Even better: hardware timestamps (SO_TIMESTAMPING)
        // Kernel bypass: NIC provides hardware timestamps in packet descriptor
        int timestamp = 1;
        setsockopt(socket_fd_, SOL_SOCKET, SO_TIMESTAMPNS, 
                  &timestamp, sizeof(timestamp));
        
        // Bind to local address
        local_addr_.sin_family = AF_INET;
        local_addr_.sin_port = htons(port);
        local_addr_.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socket_fd_, (struct sockaddr*)&local_addr_, 
                sizeof(local_addr_)) < 0) {
            close(socket_fd_);
            return false;
        }
        
        // Join multicast group if multicast IP provided
        if (!multicast_ip.empty() && multicast_ip != "0.0.0.0") {
            struct ip_mreq mreq{};
            inet_pton(AF_INET, multicast_ip.c_str(), &mreq.imr_multiaddr);
            inet_pton(AF_INET, interface_ip.c_str(), &mreq.imr_interface);
            
            if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                          &mreq, sizeof(mreq)) < 0) {
                close(socket_fd_);
                return false;
            }
        }
        
        // Additional optimizations:
        
        // 1. Disable IP fragmentation (DF flag)
        int dont_fragment = 1;
        setsockopt(socket_fd_, IPPROTO_IP, IP_MTU_DISCOVER, 
                  &dont_fragment, sizeof(dont_fragment));
        
        // 2. Set TOS/DSCP for QoS (if network supports it)
        int tos = 0xB8; // EF (Expedited Forwarding)
        setsockopt(socket_fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        
        return true;
    }
    
    /**
     * Non-blocking receive
     * 
     * In kernel bypass (e.g., Solarflare ef_vi):
     * ```cpp
     * ef_event events[32];
     * int n_ev = ef_eventq_poll(vi, events, 32);
     * for (int i = 0; i < n_ev; i++) {
     *     if (EF_EVENT_TYPE(events[i]) == EF_EVENT_TYPE_RX) {
     *         // Process packet from pre-mapped buffer
     *         // No memcpy, no syscall
     *     }
     * }
     * ```
     * 
     * @param buffer Output buffer
     * @param max_size Maximum bytes to receive
     * @return Number of bytes received, 0 if would block, -1 on error
     */
    [[nodiscard]] ssize_t receive(uint8_t* buffer, size_t max_size) noexcept {
        // In kernel bypass, this would be a memory read from DMA buffer
        // No system call at all
        
        ssize_t bytes = recvfrom(socket_fd_, buffer, max_size, 
                                MSG_DONTWAIT, nullptr, nullptr);
        
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; // No data available (expected in non-blocking mode)
            }
            return -1; // Actual error
        }
        
        return bytes;
    }
    
    /**
     * Optimized receive into internal buffer
     * Avoids extra copy for small packets
     */
    [[nodiscard]] ssize_t receive_internal(uint8_t*& buffer_ptr) noexcept {
        ssize_t bytes = receive(recv_buffer_, RECV_BUFFER_SIZE);
        if (bytes > 0) {
            buffer_ptr = recv_buffer_;
        }
        return bytes;
    }
    
    /**
     * Poll for data availability
     * In kernel bypass: always poll, never block
     */
    [[nodiscard]] bool has_data() const noexcept {
        // With MSG_PEEK we can check without consuming
        char dummy;
        ssize_t result = recv(socket_fd_, &dummy, 1, MSG_DONTWAIT | MSG_PEEK);
        return result > 0;
    }
    
    /**
     * Get socket file descriptor
     * Useful for integrating with event loops (epoll/io_uring)
     * Though in HFT, you typically busy-poll instead
     */
    [[nodiscard]] int fd() const noexcept {
        return socket_fd_;
    }
    
    ~UDPReceiver() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
        }
    }
    
    // Non-copyable, movable
    UDPReceiver() = default;
    UDPReceiver(const UDPReceiver&) = delete;
    UDPReceiver& operator=(const UDPReceiver&) = delete;
    UDPReceiver(UDPReceiver&& other) noexcept : socket_fd_(other.socket_fd_) {
        other.socket_fd_ = -1;
    }
    UDPReceiver& operator=(UDPReceiver&& other) noexcept {
        if (this != &other) {
            if (socket_fd_ >= 0) close(socket_fd_);
            socket_fd_ = other.socket_fd_;
            other.socket_fd_ = -1;
        }
        return *this;
    }
};

} // namespace hft

