#include <atomic>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

struct Page {
    char head_padding[16];
    std::atomic<Page*> next;
    char data_padding[0];
};

class MemoryPool {
   public:
    // default pool size = 1GiB = 262144 * 4096GiB
    // default page size = 4KiB
    MemoryPool(size_t num_pools = 8, size_t pagesPerPool = 262144,
               size_t page_size = 4096)
        : num_pools_(num_pools), page_size_(page_size) {
        local_pools_.resize(num_pools);
        local_remain_pages_.resize(num_pools);
        pages_per_pool_.resize(num_pools, 0);
        for (size_t i = 0; i < num_pools; i++) {
            local_pools_[i] = new std::atomic<Page*>();
            local_remain_pages_[i] = new std::atomic<size_t>(0);
        }
        for (size_t pid = 0; pid < num_pools * pagesPerPool; pid++) {
            Page* page =
                static_cast<Page*>(std::aligned_alloc(page_size, page_size));
            page->next.store(nullptr);
            assert((size_t)page % page_size == 0);
            if (page != nullptr) {
                size_t pool_idx = ((size_t)page / page_size) % num_pools;
                pages_per_pool_[pool_idx]++;
                page->next.store(
                    local_pools_[pool_idx]->load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                local_pools_[pool_idx]->store(page, std::memory_order_relaxed);
                (*local_remain_pages_[pool_idx])++;
            } else {
                throw std::bad_alloc();
            }
        }

        for (size_t i = 0; i < num_pools; ++i) {
            assert(*local_remain_pages_[i] == pages_per_pool_[i]);
        }
    }

    ~MemoryPool() {
        size_t i = 0;
        for (size_t i = 0; i < num_pools_; i++) {
            assert(*local_remain_pages_[i] == pages_per_pool_[i]);
        }

        for (std::atomic<Page*>* localPool : local_pools_) {
            Page* page = localPool->load(std::memory_order_seq_cst);
            size_t free_cnt = 0;
            while (page != nullptr) {
                // printf("pool[%zu]: free page, cnt(%zu): %p\n", i, free_cnt,
                // page);
                free_cnt++;
                Page* next = page->next.load(std::memory_order_seq_cst);
                std::free(page);
                page = next;
            }

            // printf("actual free: %zu, expected free: %zu", free_cnt,
            // local_remain_pages_[i]->load());
            assert(free_cnt == local_remain_pages_[i]->load());
            i++;
        }
        for (size_t i = 0; i < num_pools_; i++) {
            delete local_pools_[i];
            delete local_remain_pages_[i];
        }
    }

    int get_rough_richest_pool() {
        size_t richest_pool_idx = 0;
        size_t max_remain_pages = 0;
        // get a approximate richest pool
        bool has_page = false;
        for (size_t i = 0; i < num_pools_; i++) {
            if (*local_remain_pages_[i] > max_remain_pages) {
                has_page = true;
                max_remain_pages = *local_remain_pages_[i];
                richest_pool_idx = i;
            }
        }

        if (has_page == false) {
            return -1;
        } else {
            return richest_pool_idx;
        }
    }

    void* allocate() {
        size_t pool_idx =
            std::hash<std::thread::id>{}(std::this_thread::get_id()) %
            num_pools_;

    retry:
        Page *page = nullptr, *next = nullptr;

        do {
            page = local_pools_[pool_idx]->load(std::memory_order_seq_cst);
        } while (page != nullptr &&
                 !local_pools_[pool_idx]->compare_exchange_weak(
                     page, page->next.load(std::memory_order_seq_cst),
                     std::memory_order_seq_cst));

        // assert((size_t)page != 0x2a2a2a2a2a2a2a2a);  // in test, we write
        // 0x2a to page
        /*
        while (page != nullptr &&
               !local_pools_[richest_pool_idx]->compare_exchange_weak(
                   page, page->next.load(std::memory_order_relaxed),
                   std::memory_order_acquire, std::memory_order_relaxed))
            ;
            */

        /*
        while (page != nullptr && (size_t)page != 0x2a2a2a2a2a2a2a2a &&
               !local_pools_[pool_idx]->compare_exchange_strong(
                   page, next)) {
            //page =
        local_pools_[richest_pool_idx]->load(std::memory_order_seq_cst); next =
        page->next.load(std::memory_order_seq_cst);
        }
        */

        (*local_remain_pages_[pool_idx])--;

        // printf("page: %p, next: %p\n", page, next);
        // assert((size_t)page != 0x2a2a2a2a2a2a2a2a);
        // assert((size_t)next != 0x2a2a2a2a2a2a2a2a);
    end:
        if (page == nullptr) {
            int ret = get_rough_richest_pool();
            if (ret != -1) {
                pool_idx = ret;
                goto retry;
            }
        }

        return page;
    }

    void deallocate(void* ptr) {
        size_t idx = (size_t)ptr / page_size_ % num_pools_;

        if (ptr == nullptr) {
            return;
        }

        Page *page = nullptr, *expected = nullptr;
        do {
            page = static_cast<Page*>(ptr);
            expected = local_pools_[idx]->load(std::memory_order_seq_cst);
            page->next.store(expected, std::memory_order_seq_cst);
        } while (!local_pools_[idx]->compare_exchange_weak(
            expected, page, std::memory_order_seq_cst));
        /*
        while (!local_pools_[idx]->compare_exchange_weak(
            expected, page, std::memory_order_acquire,
            std::memory_order_relaxed))
            ;
            */

        // assert((size_t)expected != 0x2a2a2a2a2a2a2a2a);
        // assert((size_t)(page->next.load()) != 0x2a2a2a2a2a2a2a2a);

        /*
        while (!local_pools_[idx]->compare_exchange_strong(expected, page)) {
            page->next.store(expected, std::memory_order_seq_cst);
            //assert((size_t)expected != 0x2a2a2a2a2a2a2a2a);
            //assert((size_t)(page->next.load()) != 0x2a2a2a2a2a2a2a2a);
        }
        */

        // assert((size_t)(local_pools_[idx]->load(std::memory_order_seq_cst))!=
        // 0x2a2a2a2a2a2a2a2a);
        // assert((size_t)(local_pools_[idx]->load(std::memory_order_seq_cst)->next.load())
        // != 0x2a2a2a2a2a2a2a2a); printf("dealloc page %p\n", ptr);
        (*local_remain_pages_[idx])++;
    }

   private:
    size_t num_pools_;
    size_t page_size_;
    std::vector<size_t> pages_per_pool_;
    std::vector<std::atomic<Page*>*> local_pools_;
    std::vector<std::atomic<size_t>*> local_remain_pages_;
};
