# HFT System Architecture

## File Organization

### Core Components

```
trading_engine_cpp/
├── Core Data Structures
│   ├── spsc_queue.hpp          Lock-free SPSC ring buffer
│   ├── memory_pool.hpp         Lock-free memory pool with huge page support
│   └── logger.hpp              Async logger with SPSC queue
│
├── Type Definitions
│   ├── types.hpp               Market data types and structures
│   └── utils.hpp               Utility classes (timing, threading, spin wait)
│
├── Network & Protocol
│   ├── udp_receiver.hpp        Optimized UDP receiver (kernel bypass ready)
│   └── packet_manager.hpp      Gap/duplicate handling, resequencing
│
├── Business Logic
│   ├── feed_handler_impl.hpp   Feed handler implementation
│   └── trading_engine.hpp      Trading engine implementation
│
├── Application
│   ├── main.cpp                Main entry point
│   ├── tick_to_trade.cpp       (DEPRECATED - use main.cpp)
│   └── test_feed_generator.cpp Test data generator
│
├── Build & Documentation
│   ├── Makefile                Build system
│   ├── README.md               User guide
│   └── ARCHITECTURE.md         This file
│
└── Logs
    └── hft_system.log          Runtime logs (auto-created)
```

## Component Details

### 1. Memory Pool (`memory_pool.hpp`)

**Purpose**: Pre-allocated, lock-free memory management for deterministic allocation latency.

**Key Features**:
- Lock-free free list using atomic CAS operations
- Cache-line aligned allocations (64 bytes)
- Huge page support (2MB pages) for TLB optimization
- O(1) allocation and deallocation
- RAII wrapper (`PoolPtr`) for automatic cleanup

**Performance**:
- Allocation latency: 5-10ns (vs 50-100ns for malloc)
- Zero fragmentation
- No syscalls in hot path

**Usage Example**:
```cpp
MemoryPool<MarketEvent, 8192> pool(use_huge_pages);

// Allocate
void* ptr = pool.allocate();

// Construct in place
MarketEvent* event = pool.construct(args...);

// Deallocate
pool.deallocate(ptr);

// Or use RAII
PoolPtr<MarketEvent> ptr = pool.construct(args...);
// Auto-freed on scope exit
```

**Configuration**:
- Pool size: Template parameter (must be power of 2)
- Huge pages: Enable via constructor parameter
- Requires: `echo 1024 > /proc/sys/vm/nr_hugepages`

---

### 2. Async Logger (`logger.hpp`)

**Purpose**: Non-blocking logging system that doesn't impact hot path latency.

**Architecture**:
```
[Hot Path Thread]
      ↓ (20ns)
  Format message
      ↓
  Push to SPSC queue
      ↓
[I/O Thread] ← Dedicated thread
      ↓
  Write to file
```

**Key Features**:
- Fixed-size log entries (512 bytes) - no dynamic allocation
- SPSC queue (64K entries)
- Nanosecond timestamps
- Configurable log levels
- Graceful degradation (drops messages if queue full)

**Performance**:
- Hot path overhead: ~20ns
- Queue push: ~10ns
- No disk I/O in hot path

**Usage Example**:
```cpp
// Initialize (once at startup)
Logger::initialize("hft_system.log", LogLevel::INFO);

// Log from anywhere
LOG_INFO("System started");
LOG_WARN("Gap detected");
LOG_ERROR("Connection lost");

// Shutdown (flushes remaining messages)
Logger::shutdown();
```

**Log Levels**:
- `TRACE`: Verbose debugging
- `DEBUG`: Debug information
- `INFO`: Normal operations
- `WARN`: Warnings
- `ERROR`: Errors
- `CRITICAL`: Critical failures

---

### 3. Type Definitions (`types.hpp`)

**Purpose**: Centralized definition of all market data structures.

**Key Types**:

#### Message Types
```cpp
enum class MessageType : uint8_t {
    TRADE, QUOTE, ORDER_ADD, ORDER_DELETE, ORDER_MODIFY, HEARTBEAT
};
```

#### Market Data Packet
```cpp
struct MarketDataPacket {
    MessageType msg_type;
    uint8_t version;
    uint16_t payload_size;
    uint64_t packet_sequence;
    union { TradeMessage trade; QuoteMessage quote; } payload;
};
```

#### Market Event (Normalized)
```cpp
struct MarketEvent {
    uint64_t recv_timestamp_ns;
    uint64_t exchange_timestamp_ns;
    uint32_t symbol_id;
    MessageType type;
    union { /* trade/quote data */ } data;
};
```

**Design Principles**:
- Packed structures (`__attribute__((packed))`)
- Fixed sizes for predictable cache behavior
- Natural alignment where possible
- No pointers (serializable)

---

### 4. Utilities (`utils.hpp`)

**Purpose**: Low-level utilities for timing, threading, and CPU optimization.

#### LatencyTracker
- `rdtsc()`: Fast timestamp (TSC register)
- `rdtscp()`: Serializing timestamp
- `tsc_to_ns()`: Convert TSC to nanoseconds

#### ThreadUtils
- `pin_to_core()`: CPU affinity
- `set_realtime_priority()`: SCHED_FIFO priority

#### SpinWait
- `pause()`: CPU pause instruction
- `spin()`: Busy-wait loops

---

### 5. Packet Manager (`packet_manager.hpp`)

**Purpose**: Industry-standard packet loss and duplicate handling.

**State Machine**:
```
INITIAL → LIVE ⇄ RECOVERING → STALE
```

**Features**:
- **Gap Detection**: Immediate sequence number checking
- **Duplicate Filtering**: 10K sliding window with O(1) lookup
- **Resequencing**: 1K buffer for out-of-order packets
- **Gap Fill**: Automatic retransmission requests
- **Recovery**: Integration with recovery feed

**Statistics Tracked**:
- Total packets processed
- Duplicates filtered
- Gaps detected/filled
- Out-of-order packets
- Resequenced packets
- Buffer overflows

---

### 6. UDP Receiver (`udp_receiver.hpp`)

**Purpose**: Optimized UDP socket receiver with kernel bypass readiness.

**Optimizations**:
- Non-blocking mode (O_NONBLOCK)
- Large receive buffers (16MB+)
- Socket timestamping (SO_TIMESTAMPNS)
- Multicast support
- TOS/DSCP QoS markings

**Kernel Bypass Integration Points**:
```cpp
// Current: Standard POSIX socket
ssize_t bytes = recvfrom(socket_fd_, ...);

// Future: Solarflare ef_vi
ef_event events[32];
int n_ev = ef_eventq_poll(&vi, events, 32);
// Process packets from DMA buffer (zero copy)
```

---

## Threading Model

### Thread Layout

```
Core 0: Feed Handler Thread (RT priority 99)
  ├─ Busy poll UDP socket
  ├─ Parse and normalize packets
  ├─ Gap/duplicate handling
  └─ Push to SPSC queue

Core 1: Trading Engine Thread (RT priority 99)
  ├─ Pop from SPSC queue
  ├─ Update order book state
  ├─ Run trading strategies
  └─ Generate orders

Core 2: Order Gateway Thread (not implemented)
  └─ Send orders to exchange

Background: Logger I/O Thread (normal priority)
  └─ Drain log queue to disk
```

### Synchronization

- **Feed → Trading**: Lock-free SPSC queue
- **Hot Path → Logger**: Lock-free SPSC queue
- **No mutexes in hot path**

---

## Memory Layout

### Cache Line Optimization

```
[Cache Line 0] SPSC Queue: write_pos
[Cache Line 1] SPSC Queue: read_pos
[Cache Line 2] SPSC Queue: cached_read_pos
[Cache Line 3] SPSC Queue: cached_write_pos
[Cache Lines 4+] SPSC Queue: ring buffer

[Cache Line N] Memory Pool: free_list head
[Cache Line N+1] Memory Pool: statistics
```

**Prevents False Sharing**: Producer and consumer never touch same cache line.

---

## Performance Characteristics

### Latency Breakdown (3GHz CPU)

| Component | Latency | Notes |
|-----------|---------|-------|
| NIC → User Space | 200-500ns | With kernel bypass (Solarflare) |
| UDP recvfrom() | 5-10μs | Standard kernel socket |
| Packet parsing | 50-100ns | Binary protocol, zero-copy |
| Gap/dup check | 10-20ns | Hash set lookup |
| SPSC queue push | 10-20ns | Lock-free atomic ops |
| SPSC queue pop | 10-20ns | Lock-free atomic ops |
| Memory pool alloc | 5-10ns | CAS from free list |
| Logger push | 10-20ns | SPSC queue + format |
| **Total Tick-to-Trade** | **1-2μs** | With kernel bypass |
| **Total (kernel sockets)** | **5-10μs** | Standard Linux |

### Throughput

- **Packet processing**: 1M+ packets/sec per core
- **SPSC queue**: 10M+ ops/sec
- **Memory pool**: 20M+ alloc/sec
- **Logger**: 100K+ messages/sec

---

## Production Deployment

### System Configuration

```bash
# 1. Isolate CPUs
grubby --update-kernel=ALL --args="isolcpus=0-3 nohz_full=0-3"

# 2. Huge pages
echo 1024 > /proc/sys/vm/nr_hugepages

# 3. Disable CPU frequency scaling
cpupower frequency-set -g performance

# 4. Disable RT throttling
echo -1 > /proc/sys/kernel/sched_rt_runtime_us

# 5. Increase locked memory limit
ulimit -l unlimited

# 6. Set capabilities
setcap cap_sys_nice,cap_net_admin,cap_ipc_lock=+ep ./tick_to_trade
```

### Compiler Flags

```makefile
CXXFLAGS = -std=c++20 -O3 -march=native -mtune=native
           -flto -funroll-loops -ffast-math
           -fno-exceptions -fno-rtti  # Critical for deterministic latency
           -DNDEBUG  # Disable asserts in production
```

**Rationale for `-fno-exceptions` and `-fno-rtti`:**
- **No Exceptions**: Exception handling adds unpredictable overhead due to stack unwinding, exception tables, and branch mispredictions. HFT systems fail fast with error codes instead.
- **No RTTI**: Runtime type information is not needed and adds code size/overhead.
- Both are standard practice in ultra-low-latency systems (Jane Street, Citadel, Jump Trading)

### Monitoring

**Metrics to Track**:
- Packet processing rate (packets/sec)
- Average/P50/P99/P999 latency
- Gap rate (gaps/hour)
- Duplicate rate
- Memory pool utilization
- Log queue depth

**Tools**:
- `perf`: CPU profiling
- `ftrace`: Kernel tracing
- `numastat`: NUMA statistics
- Custom dashboards (Grafana)

---

## Comparison with Industry

### This Implementation

| Feature | This Codebase | Production HFT |
|---------|---------------|----------------|
| SPSC Queue | ✓ Lock-free | ✓ Same |
| Memory Pool | ✓ Lock-free | ✓ Same |
| Async Logger | ✓ SPSC-based | ✓ Similar |
| Gap Handling | ✓ Full state machine | ✓ Same |
| Duplicate Filter | ✓ Sliding window | ✓ Same |
| Kernel Bypass | ⚠ Ready (standard sockets) | ✓ ef_vi/DPDK |
| Hardware Timestamps | ⚠ SO_TIMESTAMPNS | ✓ NIC timestamps |
| NUMA Awareness | ✗ Not implemented | ✓ Critical |
| Order Book | ⚠ Simplified | ✓ Highly optimized |

---

## Future Enhancements

### High Priority
1. **Kernel Bypass**: Integrate Solarflare ef_vi or DPDK
2. **Hardware Timestamps**: Use NIC timestamping (SO_TIMESTAMPING)
3. **NUMA Awareness**: Pin memory and threads to same NUMA node

### Medium Priority
4. **Optimized Order Book**: Hash map + price level tree
5. **Recovery Feed**: Implement TCP recovery channel
6. **Snapshot Support**: Full book refresh protocol
7. **Symbol Manager**: Efficient symbol → ID mapping

### Low Priority
8. **JIT Compilation**: Runtime code generation for strategies
9. **FPGA Offload**: Ultra-low latency packet processing
10. **Persistent State**: Fast checkpoint/restore

---

## References

### Books
- "The Art of Multiprocessor Programming" - Herlihy & Shavit
- "C++ Concurrency in Action" - Anthony Williams
- "Systems Performance" - Brendan Gregg

### Papers
- "Disruptor: High-Performance Inter-Thread Messaging" - LMAX
- "Mechanical Sympathy" - Martin Thompson

### Exchange Protocols
- NASDAQ TotalView-ITCH 5.0
- CME MDP 3.0
- NYSE Pillar Protocol
- OPRA FAST Protocol

### Vendors
- Solarflare (ef_vi): https://docs.xilinx.com/
- Intel DPDK: https://www.dpdk.org/
- Exablaze: https://exablaze.com/

