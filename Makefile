# Makefile for HFT Tick-to-Trade Feed Handler
# Production-grade compilation flags

CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -pthread

# Optimization flags - critical for HFT
# -O3: Aggressive optimization
# -march=native: Use all CPU instructions available on this machine
# -mtune=native: Tune for this specific CPU
# -flto: Link-time optimization
# -ffast-math: Fast floating point (careful: may break IEEE compliance)
# -funroll-loops: Unroll loops for speed
# -fno-exceptions: Disable exception handling (non-deterministic overhead)
# -fno-rtti: Disable runtime type information (not needed, reduces code size)
OPTFLAGS = -O3 -march=native -mtune=native -flto -funroll-loops -fno-exceptions -fno-rtti

# Debug flags
DEBUGFLAGS = -g -O0 -DDEBUG

# Default target
TARGET = tick_to_trade
TEST_GEN = test_feed_generator

# Learning modules
LESSONS = lesson1_basics lesson2_spsc lesson3_mempool lesson4_udp lesson5_gaps lesson6_simple_system \
          lesson7_orderbook lesson8_cpu lesson9_branches lesson10_protocol lesson11_logging \
          lesson12_errors lesson13_ipc lesson14_bypass

# Source files and headers
HEADERS = spsc_queue.hpp types.hpp utils.hpp udp_receiver.hpp packet_manager.hpp \
          logger.hpp memory_pool.hpp feed_handler_impl.hpp trading_engine.hpp

# Build everything
all: $(TARGET) $(TEST_GEN) $(LESSONS)

# Production build
production: $(TARGET) $(TEST_GEN)

$(TARGET): main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o $(TARGET) main.cpp

$(TEST_GEN): test_feed_generator.cpp types.hpp utils.hpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o $(TEST_GEN) test_feed_generator.cpp

# Learning modules (optimized for demonstration)
lesson1_basics: 01_basics.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson1_basics 01_basics.cpp

lesson2_spsc: 02_spsc_queue.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson2_spsc 02_spsc_queue.cpp

lesson3_mempool: 03_memory_pool.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson3_mempool 03_memory_pool.cpp

lesson4_udp: 04_udp_networking.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson4_udp 04_udp_networking.cpp

lesson5_gaps: 05_gap_detection.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson5_gaps 05_gap_detection.cpp

lesson6_simple_system: 06_tick_to_trade_simple.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson6_simple_system 06_tick_to_trade_simple.cpp

lesson7_orderbook: 07_order_book.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson7_orderbook 07_order_book.cpp

lesson8_cpu: 08_cpu_pinning.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson8_cpu 08_cpu_pinning.cpp

lesson9_branches: 09_branch_prediction.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson9_branches 09_branch_prediction.cpp

lesson10_protocol: 10_binary_protocol.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson10_protocol 10_binary_protocol.cpp

lesson11_logging: 11_async_logging.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson11_logging 11_async_logging.cpp

lesson12_errors: 12_error_handling.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson12_errors 12_error_handling.cpp

lesson13_ipc: 13_shared_memory_ipc.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson13_ipc 13_shared_memory_ipc.cpp

lesson14_bypass: 14_kernel_bypass.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o lesson14_bypass 14_kernel_bypass.cpp

# Debug build
debug: main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(TARGET)_debug main.cpp

# Clean
clean:
	rm -f $(TARGET) $(TARGET)_debug $(TEST_GEN) $(LESSONS)

# Run with real-time priority (requires sudo/capabilities)
run: $(TARGET)
	@echo "Note: For real-time priority, run with sudo or set capabilities:"
	@echo "  sudo setcap cap_sys_nice,cap_net_admin=+ep ./$(TARGET)"
	./$(TARGET)

# Performance analysis with perf
perf: $(TARGET)
	sudo perf record -g ./$(TARGET)
	sudo perf report

# Check assembly output (useful for optimization verification)
asm: main.cpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -S -o main.s main.cpp

# Test with generated feed (runs both generator and receiver)
test: $(TARGET) $(TEST_GEN)
	@echo "Starting feed handler in background..."
	@./$(TARGET) &
	@sleep 2
	@echo "Starting test feed generator..."
	@./$(TEST_GEN) 233.54.12.1 15000 1000 10000
	@echo "Test complete. Kill feed handler manually if still running."

# Run learning modules
learn: $(LESSONS)
	@echo "=== HFT LEARNING PATH ===\n"
	@echo "Lesson 1: Low-Latency Basics"
	@./lesson1_basics
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 2: SPSC Queue"
	@./lesson2_spsc
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 3: Memory Pool"
	@./lesson3_mempool
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 4: UDP Networking"
	@./lesson4_udp
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 5: Gap Detection"
	@./lesson5_gaps
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 6: Simple Tick-to-Trade System"
	@./lesson6_simple_system
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 7: Order Book"
	@./lesson7_orderbook
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 8: CPU Pinning"
	@./lesson8_cpu
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 9: Branch Prediction"
	@./lesson9_branches
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 10: Binary Protocols"
	@./lesson10_protocol
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 11: Async Logging"
	@./lesson11_logging
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 12: Error Handling (No Exceptions)"
	@./lesson12_errors
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 13: Shared Memory IPC"
	@echo "  (Run manually: ./lesson13_ipc producer / consumer)"
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "Lesson 14: Kernel Bypass Concepts"
	@./lesson14_bypass
	@echo "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
	@echo "✓ All lessons complete! Now try: make run"

.PHONY: all production debug clean run perf asm test learn

