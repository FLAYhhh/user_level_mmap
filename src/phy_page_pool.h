#include <atomic>
#include <cassert>
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
    MemoryPool(size_t num_pools, size_t pagesPerPool, size_t page_size)
        : num_pools_(num_pools), page_size_(page_size) {
        m_localPools_.resize(num_pools);
        m_local_remain_pages_.resize(num_pools);
        for (size_t i = 0; i < num_pools; i++) {
            m_localPools_[i] = new std::atomic<Page*>();
            m_local_remain_pages_[i] = new std::atomic<size_t>();
        }
        for (size_t pid = 0; pid < num_pools * pagesPerPool; pid++) {
            Page* page = static_cast<Page*>(std::malloc(sizeof(Page)));
            if (page != nullptr) {
                size_t pool_idx = ((size_t)page / page_size) % num_pools;
                page->next.store(
                    m_localPools_[pool_idx]->load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m_localPools_[pool_idx]->store(page, std::memory_order_relaxed);
                m_local_remain_pages_[pool_idx]++;
            } else {
                throw std::bad_alloc();
            }
        }

        for (size_t i = 0; i < num_pools; ++i) {
            assert(*m_local_remain_pages_[i] == pagesPerPool);
        }
    }

    ~MemoryPool() {
        for (std::atomic<Page*>* localPool : m_localPools_) {
            Page* page = localPool->load(std::memory_order_relaxed);
            while (page != nullptr) {
                Page* next = page->next.load(std::memory_order_relaxed);
                std::free(page);
                page = next;
            }
        }
        for (size_t i = 0; i < num_pools_; i++) {
            delete m_localPools_[i];
            delete m_local_remain_pages_[i];
        }
    }

    void* allocate() {
        size_t richest_pool_idx = 0;
        size_t max_remain_pages = 0;
        // get a approximate richest pool
        for (size_t i = 0; i < num_pools_; i++) {
            if (*m_local_remain_pages_[i] > max_remain_pages) {
                max_remain_pages = *m_local_remain_pages_[i];
                richest_pool_idx = i;
            }
        }

        Page* page =
            m_localPools_[richest_pool_idx]->load(std::memory_order_relaxed);
        while (page != nullptr &&
               !m_localPools_[richest_pool_idx]->compare_exchange_weak(
                   page, page->next.load(std::memory_order_relaxed),
                   std::memory_order_acquire, std::memory_order_relaxed))
            ;
        if (page == nullptr) {
            throw std::bad_alloc();
        }
        m_local_remain_pages_[richest_pool_idx]--;
        return page;
    }

    void deallocate(void* ptr) {
        size_t idx = (size_t)ptr / page_size_ % num_pools_;

        if (ptr == nullptr) {
            return;
        }
        Page* page = static_cast<Page*>(ptr);
        page->next.store(m_localPools_[idx]->load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
        Page* expected = page->next.load(std::memory_order_relaxed);
        while (!m_localPools_[idx]->compare_exchange_weak(
            expected, page, std::memory_order_release,
            std::memory_order_relaxed))
            ;
    }

   private:
    size_t num_pools_;
    size_t page_size_;
    std::vector<std::atomic<Page*>*> m_localPools_;
    std::vector<std::atomic<size_t>*> m_local_remain_pages_;
};
