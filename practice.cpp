#include <iostream>
#include <thread>
#include <optional>
#include <atomic>
#include <chrono>

#ifdef __x86_64__
#include <immintrin.h>
#endif

class Timer
{

    public:
        static inline uint64_t rdtsc() noexcept
        {
#ifdef __x86_64__
            uint32_t lo, hi;
            __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
            return(static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
            // ARM doesn't have rdtsc, use system counter
            uint64_t val;
            __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
            return val;
#else
            return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
        }

        static inline uint64_t cycles_to_ns(uint64_t cycles, double cpu_ghz = 3.0)
        {
            return static_cast<uint64_t>(cycles / cpu_ghz);
        }

};


class SpinWait
{
    public:
        static void bad_wait()
        {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }

        static void good_wait()
        {
#ifdef __x86_64__
            _mm_pause();
#elif defined(__aarch64__)
            __asm__ __volatile__("yield");
#else
            std::this_thread::yield();
#endif
        }
};

template<typename T, size_t SIZE>
class SPSCQueue
{
    private:
        alignas(64) T buffer[SIZE];
        alignas(64) std::atomic<size_t> head_;
        alignas(64) std::atomic<size_t> tail_;
    
    public:
        
        bool try_push(const T& value)
        {
            size_t current_head = head_.load(std::memory_order_relaxed);
            size_t next_head    = (current_head + 1) % SIZE;

            if(next_head == tail_.load(std::memory_order_acquire))
            {
                return false; // full
            }

            buffer[current_head] = value;
            head_.store(next_head, std::memory_order_release);

            return true;
        }

        std::optional<T> try_pop()
        {
            size_t current_tail = tail_.load(std::memory_order_relaxed);
            size_t next_tail    = (current_tail + 1) % SIZE;

            if(current_tail == head_.load(std::memory_order_acquire))
            {
                return std::nullopt;
            }

            T value = buffer[current_tail];
            tail_.store(next_tail, std::memory_order_release);

            return value;
        }

        size_t size()
        {
            return (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire) + SIZE) % SIZE;  
        }
};

template<typename T, size_t PoolSize>
class MemoryPool
{
    private:
        struct FreeNode
        {
            FreeNode* next;
        };

        alignas(64) uint8_t memory_[sizeof(T) * PoolSize]; // why uint8_t?

        alignas(64) std::atomic<FreeNode*> free_list_{nullptr};

    public:
        
        MemoryPool()
        {
            FreeNode* head = nullptr;

            for(size_t i = PoolSize; i > 0; --i)
            {
                FreeNode* node = reinterpret_cast<FreeNode*>(
                    memory_ + ((i - 1) * sizeof(T))
                );
                node->next = head;
                head = node;
            }
            free_list_.store(head, std::memory_order_release);
        }

        [[nodiscard]] void* allocate() noexcept
        {
            FreeNode* old_head = free_list_.load(std::memory_order_acquire);

            while(old_head != nullptr)
            {
                FreeNode* next_head = old_head->next;

                if(free_list_.compare_exchange_weak(
                    old_head, next_head, std::memory_order_release, std::memory_order_acquire
                ))
                {
                    return static_cast<void*>(old_head);
                }
                // CAS failed (another thread changed it), retry
            }

            return nullptr;
        }

        void deallocate(void* ptr) noexcept
        {
            if(!ptr) return;

            FreeNode* node = static_cast<FreeNode*> (ptr);
            FreeNode* old_head = free_list_.load(std::memory_order_acquire);

            do
            {
                node->next = old_head;
            }
            while(!free_list_.compare_exchange_weak(
                old_head, node, std::memory_order_release, std::memory_order_acquire
            ));
        }

        template<typename... Args>
        [[nodiscard]] T* construct(Args&&... args) noexcept
        {
            void* ptr = allocate();

            if(!ptr) return nullptr;

            return new(ptr) T(std::forward<Args>(args)...);
        }

        void destroy(T* ptr) noexcept
        {
            if(!ptr) return;
            ptr->~T();
            deallocate(ptr);
        }
};

struct MarketData
{
    uint64_t timestamp;
    uint64_t symbol_id;
    uint64_t price;
    uint64_t quantity;

    MarketData() = default;
    MarketData(uint64_t ts, uint32_t sym, uint64_t p, uint32_t q)
        : timestamp(ts), symbol_id(sym), price(p), quantity(q) {}
};

static inline uint64_t rdtsc()
{
#ifdef __x86_64__
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}

void benchmark_malloc()
{
    constexpr int ITERATIONS = 100000;
    
    // Warmup
    for(int i = 0; i < 1000; ++i) {
        MarketData* event = new MarketData(123, 456, 789, 100);
        delete event;
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for(int i = 0; i < ITERATIONS; ++i)
    {
        MarketData* event = new MarketData(123, 456, 789, 100);
        // Use the pointer to prevent optimization
        __asm__ __volatile__("" : : "r,m"(event) : "memory");
        delete event;
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    std::cout << "malloc/delete: " << (duration / ITERATIONS) << " ns/op\n"; 
}

void benchmark_pool()
{
    constexpr int ITERATIONS = 100000;
    MemoryPool<MarketData, 1024> pool;
    
    // Warmup
    for(int i = 0; i < 1000; ++i) {
        MarketData* event = pool.construct(123,456,789,100);
        pool.destroy(event);
    }
    
    auto start = std::chrono::high_resolution_clock::now();

    for(int i = 0; i < ITERATIONS; ++i)
    {
        MarketData* event = pool.construct(123,456,789,100);
        // Use the pointer to prevent optimization
        __asm__ __volatile__("" : : "r,m"(event) : "memory");
        pool.destroy(event);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::cout << "pool alloc/free: " << (duration / ITERATIONS) << " ns/op\n";
}

int main()
{
    std::cout << "=== MEMORY POOL vs MALLOC ===\n\n";
    
    std::cout << "Benchmark (100,000 allocations):\n";
    benchmark_malloc();
    benchmark_pool();
    
    return 0;
}