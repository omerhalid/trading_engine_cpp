# High-Frequency Trading Feed Handler

- **Lock-free SPSC Queue**: Single Producer Single Consumer ring buffer with cache-line alignment
- **Lock-free Memory Pool**: Pre-allocated memory with huge page support (5-10ns allocation)
- **Async Logger**: Non-blocking logging with dedicated I/O thread (~20ns hot path)
- **Kernel Bypass UDP**: Optimized UDP receiver (ready for Solarflare/DPDK integration)
- **Non-blocking Architecture**: Busy polling, no system calls in hot path
- **Nanosecond Timing**: RDTSC-based timestamping for latency measurement
- **CPU Affinity**: Thread pinning to dedicated cores
- **Real-time Priority**: SCHED_FIFO scheduling
- **Gap Detection & Recovery**: Industry-standard packet loss handling
- **Duplicate Filtering**: Sliding window duplicate detection (10K window)
- **Out-of-Order Buffering**: Automatic packet resequencing (1K buffer)
- **Feed State Machine**: INITIAL → LIVE → RECOVERING → STALE transitions
- **Modular Design**: Professional file organization with clean separation of concerns

## Architecture

```
[NIC/Multicast Feed] 
      ↓
[Feed Handler Thread] ← Core 0, RT Priority
      ↓ (RDTSC timestamp, LOG_INFO)
[Packet Manager] ← Gap/Dup detection
      ↓
[Parse & Normalize] ← Memory Pool (optional)
      ↓
[Lock-free SPSC Queue] ← 64K slots, cache-line aligned
      ↓
[Trading Engine Thread] ← Core 1, RT Priority
      ↓
[Trading Logic / Signals]
      ↓
[Order Gateway] ← Core 2 (not implemented)

[Async Logger I/O Thread] ← Background, drains log queue
```

### File Organization

- **Core Data Structures**: `spsc_queue.hpp`, `memory_pool.hpp`, `logger.hpp`
- **Types & Utils**: `types.hpp`, `utils.hpp`, `error_handling.hpp`
- **Network**: `udp_receiver.hpp`, `packet_manager.hpp`
- **Business Logic**: `feed_handler_impl.hpp`, `trading_engine.hpp`
- **Application**: `main.cpp`, `test_feed_generator.cpp`

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed documentation.

## Key Performance Features

### 1. Lock-Free SPSC Queue
- Producer and consumer on separate cache lines (prevents false sharing)
- Power-of-2 size for fast modulo (bitwise AND vs %)
- Memory ordering optimizations (acquire/release semantics)
- Cached positions to reduce atomic operations

### 2. Latency Optimization
- RDTSC for sub-nanosecond timestamps
- Non-blocking busy polling (no context switches)
- CPU affinity pinning (dedicated cores)
- Real-time priority scheduling
- Aligned data structures (cache efficiency)

### 3. Network Optimization
- Non-blocking UDP sockets
- Large receive buffers (16MB+)
- Socket timestamping support
- Multicast support
- Ready for kernel bypass (Solarflare, DPDK)

### 4. Memory Pool

**Lock-free memory pool** for deterministic allocation latency:
- **Pre-allocated**: No malloc in hot path
- **Lock-free**: Atomic CAS operations on free list
- **Huge Pages**: Optional 2MB pages for TLB optimization
- **Performance**: 5-10ns per allocation (vs 50-100ns for malloc)

```cpp
MemoryPool<MarketEvent, 8192> pool(use_huge_pages);
MarketEvent* event = pool.construct(args...);
pool.destroy(event);
```

### 5. Async Logger

**Non-blocking logger** that doesn't impact hot path:
- **SPSC Queue**: 64K message buffer
- **I/O Thread**: Dedicated thread for disk writes
- **Hot Path**: ~20ns overhead
- **Fixed Size**: 512-byte messages, no dynamic allocation

```cpp
Logger::initialize("hft_system.log", LogLevel::INFO);
LOG_INFO("System started");
LOG_WARN("Gap detected");
Logger::shutdown();  // Flushes remaining messages
```

### 6. Packet Loss & Duplicate Handling (Industry Standard)

#### Gap Detection & Recovery
- **Sequence Number Tracking**: Every packet has a sequence number
- **Gap Detection**: Immediate detection when sequence jumps
- **State Machine**: 
  - `INITIAL`: Waiting for first packet/snapshot
  - `LIVE`: Normal operation, in-sequence processing
  - `RECOVERING`: Gap detected, buffering out-of-order packets
  - `STALE`: Too many gaps, need full snapshot
- **Gap Fill Requests**: Automatic retransmission requests
- **Retry Logic**: 3 retries with 1-second timeout
- **Configurable Thresholds**: Max gap size (1000 packets default)

#### Duplicate Detection
- **Sliding Window**: 10,000 recent sequence numbers tracked
- **O(1) Lookup**: Hash set for instant duplicate detection
- **Memory Efficient**: Deque + unordered_set = minimal overhead
- **Production Pattern**: Same as NASDAQ, CME implementations

#### Out-of-Order Handling
- **Resequence Buffer**: Holds up to 1,000 out-of-order packets
- **Automatic Resequencing**: Processes buffered packets when gap fills
- **Overflow Protection**: Drops oldest when buffer full
- **Zero Latency Impact**: No sorting needed, natural order from sequence numbers

#### Recovery Feed Integration
- **Gap Fill Request**: Callback-based architecture
- **Recovery Manager**: Ready for TCP/UDP recovery channel
- **Snapshot Refresh**: Triggered on STALE state
- **Exchange-Specific Protocols**: 
  - CME MDP 3.0: TCP Replay channel
  - NASDAQ ITCH: MOLD UDP retransmit
  - NYSE Pillar: Retransmission request
  - OPRA: FAST recovery protocol

## Building

```bash
# Production build (with optimizations)
make

# Debug build
make debug

# View assembly (optimization verification)
make asm
```

## Running

```bash
# Basic run
./tick_to_trade

# With real-time priority (requires capabilities)
sudo setcap cap_sys_nice,cap_net_admin=+ep ./tick_to_trade
./tick_to_trade

# Or with sudo
sudo ./tick_to_trade
```

## Configuration

Edit `tick_to_trade.cpp` main() function:

```cpp
const std::string MULTICAST_IP = "233.54.12.1";  // Your multicast group
const uint16_t PORT = 15000;                      // Your port
const int FEED_HANDLER_CORE = 0;                  // CPU core for feed handler
const int TRADING_ENGINE_CORE = 1;                // CPU core for trading
const bool USE_HUGE_PAGES = false;                // Enable if huge pages configured
```

### Huge Pages Setup (Optional)

For memory pool optimization:
```bash
# Configure huge pages
echo 1024 > /proc/sys/vm/nr_hugepages

# Verify
cat /proc/meminfo | grep HugePages

# Enable in config
const bool USE_HUGE_PAGES = true;
```

## Expected Performance

On modern hardware (3GHz CPU):

| Component | Latency | Notes |
|-----------|---------|-------|
| Feed handler (parse + queue) | 50-200ns | Binary protocol |
| SPSC queue (push/pop) | 10-20ns | Lock-free atomic ops |
| Memory pool allocation | 5-10ns | Lock-free free list |
| Logger (hot path) | ~20ns | Format + queue push |
| **Total tick-to-trade** | **1-2μs** | With kernel bypass |
| **Total (kernel sockets)** | **5-10μs** | Standard Linux |

### Throughput
- Packet processing: 1M+ packets/sec
- SPSC queue ops: 10M+ ops/sec
- Memory pool allocs: 20M+ allocs/sec

## Production Enhancements

To achieve sub-microsecond latency in production:

### 1. Kernel Bypass
```cpp
// Replace standard sockets with Solarflare ef_vi:
ef_vi vi;
ef_driver_handle dh;
ef_vi_alloc_from_pd(&vi, dh, &pd, ...);

// Polling loop:
ef_event events[32];
int n_ev = ef_eventq_poll(&vi, events, 32);
```

### 2. System Configuration
```bash
# Isolate CPUs from scheduler
grubby --update-kernel=ALL --args="isolcpus=0-3 nohz_full=0-3"

# Disable CPU frequency scaling
cpupower frequency-set -g performance

# Use huge pages
echo 1024 > /proc/sys/vm/nr_hugepages

# Set real-time throttling
echo -1 > /proc/sys/kernel/sched_rt_runtime_us
```

### 3. Compiler Optimization
```makefile
CXXFLAGS = -O3 -march=native -mtune=native -flto
           -ffast-math -funroll-loops
           -fno-exceptions -fno-rtti  # Disable for low latency
```

**Why disable exceptions?**
- Exception handling adds non-deterministic overhead
- Stack unwinding is unpredictable
- Branch mispredictions from exception paths
- Prefer error codes and early returns for predictable latency

### 4. Memory Optimization
- Use huge pages (2MB pages vs 4KB)
- Pre-allocate all memory
- Lock memory with mlock()
- NUMA-aware allocation

### 5. Network Hardware
- Solarflare X2522 / X3522 (200-500ns)
- Mellanox ConnectX-6 Dx
- Exablaze ExaNIC X25 / X100
- Hardware timestamping

## Testing

A comprehensive test feed generator is included to validate gap detection, duplicate filtering, and resequencing.

### Test Feed Generator

The `test_feed_generator` simulates a realistic market data feed with configurable anomalies:

**Features:**
- Configurable packet rate (packets per second)
- **Gap injection**: Randomly skips sequences (default 0.1% probability)
- **Duplicate injection**: Resends old packets (default 0.2% probability)
- **Reorder injection**: Delivers packets out of order (default 0.5% probability)

**Build and Run:**

```bash
# Build everything including test generator
make

# Terminal 1: Start the feed handler
./tick_to_trade

# Terminal 2: Start the test generator
./test_feed_generator 233.54.12.1 15000 10000 100000
# Args: [multicast_ip] [port] [packets_per_sec] [total_packets]

# Or use the automated test target
make test
```

**Example Output:**

```
[Generator] Starting packet generation at 10000 packets/sec
[Generator] INJECTING GAP: skipping 5 sequences (from 1234 to 1239)
[Generator] SENDING DUPLICATE: seq 2456
[Generator] SENDING REORDERED: seq 3789 (should be before 3791)
[Generator] Sent: 10000, Rate: 9987 pps, Gaps: 15, Duplicates: 23, Reordered: 48
```

**Feed Handler Detection:**

```
[FeedHandler] GAP DETECTED: sequences 1234 to 1239 (gap size: 5)
[FeedHandler] Feed state: RECOVERING
[PacketMgr] Stats - Duplicates: 23, Gaps Detected: 15, Out-of-Order: 48
```

### Custom Test Scenarios

Modify probabilities in `test_feed_generator.cpp`:

```cpp
generator.set_gap_probability(0.01);       // 1% gaps
generator.set_duplicate_probability(0.02);  // 2% duplicates  
generator.set_reorder_probability(0.05);    // 5% reorders
```

### Benchmarking

To measure pure processing latency without network effects:

```bash
# High rate, short duration
./test_feed_generator 233.54.12.1 15000 100000 1000000

# Monitor latency statistics from feed handler output
```

## Monitoring

The feed handler prints comprehensive statistics:

### Feed Handler Stats
- Packets received/processed/dropped
- Sequence gaps detected
- Average/Min/Max processing latency

### Packet Manager Stats
- **Duplicates**: Number of duplicate packets filtered
- **Gaps Detected**: Count of sequence gaps found
- **Gaps Filled**: Successfully recovered gaps
- **Out-of-Order**: Packets that arrived early
- **Resequenced**: Buffered packets processed in order
- **Overflow Drops**: Packets dropped due to buffer full
- **Next Expected**: Current sequence number expected
- **Feed State**: INITIAL/LIVE/RECOVERING/STALE

### Example Output
```
[FeedHandler] Stats - Recv: 1000000, Proc: 999850, Drop: 0, Gaps: 5, 
  Avg Latency: 145ns, Min: 87ns, Max: 2341ns
[PacketMgr] Stats - Duplicates: 12, Gaps Detected: 5, Gaps Filled: 5, 
  Out-of-Order: 143, Resequenced: 143, Overflow Drops: 0, Next Expected: 1000001
[FeedHandler] Feed state: LIVE
```

### Gap Detection Example
```
[FeedHandler] GAP DETECTED: sequences 50000 to 50005 (gap size: 6)
[FeedHandler] Feed state: RECOVERING
... (recovery requests sent) ...
[PacketMgr] Gap filled: 50000-50005
[FeedHandler] Feed state: LIVE
```

For production monitoring, integrate with:
- **Prometheus**: Metrics export via push gateway
- **InfluxDB**: Time-series data storage
- **Grafana**: Real-time visualization dashboards
- **Alerting**: Alert on gap rate, drop rate, state transitions

## Common Pitfalls

1. **Context Switches**: Use `isolcpus` and CPU pinning
2. **Cache Misses**: Align data structures, use prefetching
3. **TLB Misses**: Use huge pages
4. **System Calls**: Avoid in hot path (kernel bypass)
5. **Memory Allocation**: Pre-allocate everything
6. **Lock Contention**: Use lock-free data structures
7. **Branch Misprediction**: Profile with `perf` and optimize

## License

Educational/reference implementation. Use at your own risk in production.

## References

- "The Art of Multiprocessor Programming" - Herlihy & Shavit
- "Linux Kernel Development" - Robert Love
- "C++ Concurrency in Action" - Anthony Williams
- Solarflare OpenOnload Documentation
- Intel DPDK Documentation

