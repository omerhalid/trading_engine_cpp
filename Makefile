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
OPTFLAGS = -O3 -march=native -mtune=native -flto -funroll-loops

# Debug flags
DEBUGFLAGS = -g -O0 -DDEBUG

# Default target
TARGET = tick_to_trade
TEST_GEN = test_feed_generator

# Source files and headers
HEADERS = spsc_queue.hpp types.hpp utils.hpp udp_receiver.hpp packet_manager.hpp \
          logger.hpp memory_pool.hpp feed_handler_impl.hpp trading_engine.hpp

# Production build
all: $(TARGET) $(TEST_GEN)

$(TARGET): main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o $(TARGET) main.cpp

$(TEST_GEN): test_feed_generator.cpp types.hpp utils.hpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -o $(TEST_GEN) test_feed_generator.cpp

# Debug build
debug: main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(TARGET)_debug main.cpp

# Clean
clean:
	rm -f $(TARGET) $(TARGET)_debug $(TEST_GEN)

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

.PHONY: all debug clean run perf asm test

