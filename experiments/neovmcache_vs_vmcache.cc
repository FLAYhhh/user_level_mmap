#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>

// 假设PageSize和State数组或Header的定义
constexpr uint64_t PageSize = 4096;
constexpr uint64_t NumPages = 1000000;  // 示例数据页数量

class PageState {
   private:
    union {
        struct {
            uint64_t evicted : 1;    // 占用1位，表示页面是否被逐出
            uint64_t marked : 1;     // 占用1位，表示页面是否被标记
            uint64_t locked : 1;     // 占用1位，表示页面是否被锁定
            uint64_t reserved : 61;  // 剩余的61位保留未用
        } bits;
        uint64_t all;  // 允许直接访问所有位作为一个整体
    };

   public:
    PageState() : all(0) {}  // 默认构造函数

    // 检查页面是否被逐出
    bool isEvicted() const { return bits.evicted == 1; }

    // 设置页面为逐出状态
    void setEvicted() {
        bits.evicted = 1;
        bits.marked = 0;  // 逐出时清除标记状态
        bits.locked = 0;
    }

    // 检查页面是否被标记
    bool isMarked() const { return bits.marked == 1; }

    // 设置页面为标记状态
    void setMarked() { bits.marked = 1; }

    // 检查页面是否解锁
    bool isUnlocked() const { return bits.locked == 0; }

    void unsafe_lock() { bits.locked = 1; }

    void setUnlocked() { bits.locked = 0; }

    uint64_t locked_state() {
        auto tmp = *this;
        tmp.unsafe_lock();
        return tmp.all;
    }

    uint64_t state() {
        return all;
    }
};

bool compareAndSwap(std::atomic<uint64_t> *value, uint64_t expected,
                    uint64_t desired) {
    // 尝试以原子方式将value的值从expected更新为desired
    return value->compare_exchange_strong(expected, desired,
                                         std::memory_order_relaxed);
}

PageState state[NumPages];

void *vmcache_virtMem;  // base addr from mmap
// 假设vmcache和neovmcache状态管理的模拟实现
// 这些函数需要根据实际的缓冲区管理器实现进行定义
void *vmcache_fix(uint64_t pid) {
    // 实现细节
    uint64_t ofs = pid * PageSize;
    while (true) {
        auto s = state[pid];
        if (s.isEvicted()) {
            // nerver reached in this experiment
            assert(false);
            if (compareAndSwap(reinterpret_cast<std::atomic<uint64_t> *>(&state[pid]), s.state(), s.locked_state())) {
                // read from disk.
                // return vaddr.
            }
        } else if (s.isMarked() || s.isUnlocked()) {
            //if (compareAndSwap(reinterpret_cast<std::atomic<uint64_t> *>(&state[pid]), s.state(), s.locked_state())) {
            //    return reinterpret_cast<void *>(reinterpret_cast<uint64_t>(vmcache_virtMem) + ofs);
            //}
            s.unsafe_lock();
            return reinterpret_cast<void *>(reinterpret_cast<uint64_t>(vmcache_virtMem) + ofs);
        }
    }
}

void vmcache_unfix(uint64_t pid) { state[pid].setUnlocked(); }

void *neovmcache_virtMem;
void *neovmcache_fix(uint64_t pid) {
    uint64_t ofs = pid * PageSize;
    PageState *s_ptr = reinterpret_cast<PageState *>(reinterpret_cast<uint64_t>(neovmcache_virtMem) + ofs);
    while (true) {
        auto s = *s_ptr;
        if (s.isEvicted()) {
            // never reach
            assert(false);
        } else if (s.isMarked() || s.isUnlocked()) {
            //if (compareAndSwap(reinterpret_cast<std::atomic<uint64_t> *>(s_ptr), s.state(), s.locked_state())) {
            //    return reinterpret_cast<void *>(reinterpret_cast<uint64_t>(neovmcache_virtMem) + ofs);
            //}
            s.unsafe_lock();
            return reinterpret_cast<void *>(reinterpret_cast<uint64_t>(vmcache_virtMem) + ofs);
        }
    }
}

void neovmcache_unfix(uint64_t pid) {
    // 实现细节
    uint64_t ofs = pid * PageSize;
    PageState *s_ptr = reinterpret_cast<PageState *>(reinterpret_cast<uint64_t>(neovmcache_virtMem) + ofs);
    s_ptr->setUnlocked();
}

// 生成随机访问的页ID
uint64_t getRandomPageID() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis(0, NumPages - 1);
    return dis(gen);
}

// 执行N次操作的函数
int acc;

template <typename FixFunc, typename UnfixFunc>
double performTest(FixFunc fix, UnfixFunc unfix, uint64_t N) {
    auto start = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < N; ++i) {
        uint64_t pid = getRandomPageID();
        auto vaddr = fix(pid);
        // read from the data page
        acc += *(int *)vaddr;

        unfix(pid);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    return duration.count();
}

int check_data_file(std::string &data_file);

int main() {
    uint64_t N = 1000000;  // 设置操作次数

    // check if data file is exist
    std::string data_file("experiment1_data_file");

    // make sure data file have at least NumPages pages

    int fd = check_data_file(data_file);
    // 映射虚拟内存
    vmcache_virtMem = mmap(NULL, NumPages * PageSize,
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (vmcache_virtMem == MAP_FAILED) {
        std::cerr << "Failed to mmap data file: " << data_file << std::endl;
        close(fd);
        return -1;
    }
    // 使用madvise通知内核预加载所有页面
    if (madvise(vmcache_virtMem, NumPages * PageSize, MADV_WILLNEED) != 0) {
        std::cerr << "madvise failed" << std::endl;
        munmap(vmcache_virtMem, NumPages * PageSize);
        close(fd);
        return -1;
    }

    neovmcache_virtMem = vmcache_virtMem;

    // 测试vmcache
    double vmcacheTime = performTest(vmcache_fix, vmcache_unfix, N);
    std::cout << "vmcache, N = " << N << ", Time (ms): " << vmcacheTime
              << std::endl;

    std::cout << "vmcache IOPS: " << N / (vmcacheTime / 1000) << std::endl;

    // 测试neovmcache
    double neovmcacheTime = performTest(neovmcache_fix, neovmcache_unfix, N);
    std::cout << "neovmcache, N = " << N << ", Time (ms): " << neovmcacheTime
              << std::endl;

    // 计算IOPS
    std::cout << "neovmcache IOPS: " << N / (neovmcacheTime / 1000)
              << std::endl;

    return 0;
}

int check_data_file(std::string &data_file) {
    // 检查数据文件是否存在
    std::ifstream file(data_file);
    if (!file.good()) {
        std::cerr << "Data file does not exist: " << data_file
                  << ". Creating..." << std::endl;

        // 创建并打开文件
        int fd = open(data_file.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd == -1) {
            std::cerr << "Failed to create data file: " << data_file
                      << std::endl;
            return -1;
        }

        // 扩展文件到所需大小
        off_t fileSize = NumPages * PageSize;
        if (lseek(fd, fileSize - 1, SEEK_SET) == -1) {
            std::cerr << "Failed to extend data file: " << data_file
                      << std::endl;
            close(fd);
            return -1;
        }

        // 写入最后一个字节以扩展文件
        if (write(fd, "", 1) != 1) {
            std::cerr << "Failed to write last byte to data file: " << data_file
                      << std::endl;
            close(fd);
            return -1;
        }

        close(fd);
        std::cout << "Data file created and extended to required size."
                  << std::endl;
    } else {
        file.close();
    }

    // 打开文件准备mmap
    int fd = open(data_file.c_str(), O_RDWR);
    if (fd == -1) {
        std::cerr << "Failed to open data file: " << data_file << std::endl;
        return -1;
    }

    // 获取文件大小
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        std::cerr << "Failed to get file size: " << data_file << std::endl;
        close(fd);
        return -1;
    }

    // 确保文件至少有NumPages页
    if (sb.st_size < NumPages * PageSize) {
        std::cerr << "Data file is too small." << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}
