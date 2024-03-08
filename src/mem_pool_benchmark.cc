#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <cassert>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdio>
#include "phy_page_pool.h"

constexpr size_t PAGE_SIZE = 4096;

void memory_pool_test(MemoryPool& pool, size_t ops) {
    void *ptr[ops];
    
    for (size_t i = 0; i < ops; ++i) {
        void* page = pool.allocate();
        ptr[i] = page;
        assert(page != nullptr);
        std::memset(page, 42, PAGE_SIZE);
    }

    for (size_t i = 0; i < ops; ++i) {
        pool.deallocate(ptr[i]);
    }
}

void malloc_test(size_t ops) {
    void *ptr[ops];
    for (size_t i = 0; i < ops; ++i) {
        void* page = std::malloc(PAGE_SIZE);
        ptr[i] = page;
        assert(page != nullptr);
        std::memset(page, 42, PAGE_SIZE);
    }

    for (size_t i = 0; i < ops; ++i) {
        free(ptr[i]);
    }
}

void bench(size_t num_pools, size_t pages_per_pool, size_t num_threads, size_t iterations) {
    printf("=========num pools: %zu, num threads: %zu, ops / thread: %zu\n", num_pools, num_threads, iterations);
    MemoryPool pool(num_pools, pages_per_pool);
    std::vector<std::thread> threads;

    assert(iterations <= pages_per_pool);

    // malloc
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back(malloc_test, iterations);
    }
    for (auto& t : threads) {
        t.join();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "malloc time: " << duration << " ms\n";
    double ops_per_sec = num_threads * iterations / duration * 1000;
    std::cout << "malloc throuputs: " << ops_per_sec << "/s\n";

    start = std::chrono::high_resolution_clock::now();
    threads.clear();
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back(memory_pool_test, std::ref(pool), iterations);
    }
    for (auto& t : threads) {
        t.join();
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "MemoryPool time: " << duration << " ms\n";
    ops_per_sec = num_threads * iterations / duration * 1000;
    std::cout << "MemoryPool throuputs: " << ops_per_sec << "/s\n";
}

int main() {
    // poolsize = 16GiB * 1;
    // max access size if not free = 12G;
    // it's enough
    size_t total_pages = 4194304 / 2; // 8G

    for (int pools = 2; pools <= 32; pools *= 2) {
		//bench(pools, total_pages/pools, 1, 10*1000);
		//bench(pools, total_pages/pools, 2, 10*1000);
		bench(pools, total_pages/pools, 4, 10*1000);
		bench(pools, total_pages/pools, 8, 10*1000);
		bench(pools, total_pages/pools, 16, 10*1000);
		bench(pools, total_pages/pools, 32, 10*1000);
    }

	return 0;
}
