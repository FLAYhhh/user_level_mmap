#include "user_level_mmap.h"

#include <err.h>
#include <fcntl.h>
#include <jemalloc/jemalloc.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <ptedit_header.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "phy_page_pool.h"

#define COLOR_YELLOW "\x1b[33m"
#define COLOR_RESET "\x1b[0m"
#define TAG_PROGRESS COLOR_YELLOW "[~]" COLOR_RESET " "

static int PAGE_SIZE;

// page fault handler arguments
struct PFhandle_args {
    PFhandle_args(long uffd_, int fd_, off_t offset_, void *base_addr_)
        : uffd(uffd_), fd(fd_), offset(offset_), base_addr(base_addr_) {}

    long uffd = 0;  // need to get events from uffd
    int fd = -1;    // for file backed up mmap
    off_t offset = 0;
    void *base_addr = NULL;  // mmap base address

    std::thread thread;
    bool finish = false;

    /*statistics*/
    int fault_cnt = 0;
};

std::mutex mmap_regions_mu;
std::unordered_map<void *, std::shared_ptr<PFhandle_args>> mmap_regions;

static void page_fault_handler(std::shared_ptr<PFhandle_args> pfh_args) {
    int nready;
    ssize_t nread;
    struct pollfd pollfd;
    // struct uffdio_copy uffdio_copy;
    struct uffdio_range uffdio_range;
    static struct uffd_msg msg; /* Data read from userfaultfd */

    long uffd = pfh_args->uffd; /* userfaultfd file descriptor */

    /* Loop, handling incoming events on the userfaultfd
       file descriptor. */

    for (;;) {
        /* See what poll() tells us about the userfaultfd. */

        pollfd.fd = uffd;
        pollfd.events = POLLIN;
        nready = poll(&pollfd, 1, -1);
        if (nready == -1) err(EXIT_FAILURE, "poll");

        /*
        printf("\nfault_handler_thread():\n");
        printf(
            "    poll() returns: nready = %d; "
            "POLLIN = %d; POLLERR = %d\n",
            nready, (pollfd.revents & POLLIN) != 0,
            (pollfd.revents & POLLERR) != 0);
        */

        /* Read an event from the userfaultfd. */

        nread = read(uffd, &msg, sizeof(msg));
        if (nread == 0) {
            printf("EOF on userfaultfd!\n");
            exit(EXIT_FAILURE);
        }

        if (nread == -1) err(EXIT_FAILURE, "read");

        /* We expect only one kind of event; verify that assumption. */

        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            fprintf(stderr, "Unexpected event on userfaultfd\n");
            exit(EXIT_FAILURE);
        }

        /* Display info about the page-fault event. */

        /*
        printf("    UFFD_EVENT_PAGEFAULT event: ");
        printf("flags = %llx; ", msg.arg.pagefault.flags);
        printf("address = %llx\n", msg.arg.pagefault.address);
        */

        /* Copy the page pointed to by 'page' into the faulting
           region. Vary the contents that are copied in, so that it
           is more obvious that each fault is handled separately. */

        void *given_page = malloc(PAGE_SIZE);
        assert(given_page != nullptr);

        if (pfh_args->fd == -1) {
            memset(given_page, 'A' + pfh_args->fault_cnt % 26, PAGE_SIZE);
        } else {
            auto region_offset =
                msg.arg.pagefault.address - (__u64)pfh_args->base_addr;
            auto bytes_read = pread(pfh_args->fd, given_page, PAGE_SIZE,
                                    pfh_args->offset + region_offset);
            assert(bytes_read == PAGE_SIZE);
        }
        pfh_args->fault_cnt++;

        // 1. get the pfd of given_page
        size_t given_page_pfn = ptedit_pte_get_pfn(given_page, 0);
        // debug: print given_page
        // {
        //     ptedit_entry_t given_page_vm = ptedit_resolve((void *)given_page,
        //     0); ptedit_print_entry_t(given_page_vm); printf(TAG_PROGRESS
        //     "given page vm %zx\n", (size_t)(ptedit_cast(given_page_vm.pte,
        //     ptedit_pte_t).pfn));
        // }
        // 2. get the ptedit_entry of fault address
        ptedit_entry_t vm =
            ptedit_resolve((void *)msg.arg.pagefault.address, 0);
        // debug:print page fault address
        // {
        //     ptedit_print_entry_t(vm);
        //     printf(TAG_PROGRESS "fault address %zx\n",
        //     (size_t)(ptedit_cast(vm.pte, ptedit_pte_t).pfn));
        // }
        // 3. update to pfn of fault address, and set valid bit, and update
        vm.pte = ptedit_set_pfn(vm.pte, given_page_pfn);
        vm.pte = ptedit_pte_entry_set_bit(vm.pte, PTEDIT_PAGE_BIT_PRESENT);
        vm.pte = ptedit_pte_entry_set_bit(vm.pte, PTEDIT_PAGE_BIT_RW);
        vm.pte = ptedit_pte_entry_set_bit(vm.pte, PTEDIT_PAGE_BIT_USER);
        vm.valid = PTEDIT_VALID_MASK_PTE;
        ptedit_update((void *)msg.arg.pagefault.address, 0, &vm);

        // debug: print updated page fault address
        // {
        //     ptedit_entry_t vm = ptedit_resolve(msg.arg.pagefault.address, 0);
        //     ptedit_print_entry_t(vm);
        //     printf(TAG_PROGRESS "updated fault address %zx\n",
        //     (size_t)(ptedit_cast(vm.pte, ptedit_pte_t).pfn));
        // }

        // debug: try to access the page
        /*
        {
            char ch = *(char *)(msg.arg.pagefault.address);
            printf(TAG_PROGRESS "Try to access page fault address: %c\n", ch);
        }
        */

        /* We need to handle page faults in units of pages(!).
            So, round faulting address down to page boundary. */
        uffdio_range.start =
            (unsigned long)msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
        uffdio_range.len = PAGE_SIZE;
        if (ioctl(uffd, UFFDIO_WAKE, &uffdio_range) == -1)
            err(EXIT_FAILURE, "ioctl-UFFDIO_WAKE");

        // printf("       uffdio_wake returned\n");
    }
}

/**
 * use kernel's mmap implementation to reserve vm area.
 * then, use userfaultfd to delegate page fault handle to user space.
 */
void *ul_mmap(void *addr, size_t length, int prot, int flags, int fd,
              off_t offset) {
    static bool pteditor_init_flag = false;
    if (pteditor_init_flag == false) {
        // FIXME(Priority: Low): race condition: may lead to leak of ptedit_fd
        /*init PTEditor*/
        if (ptedit_init()) {
            printf(
                "Error: Could not initalize PTEditor, did you load the kernel "
                "module?\n");
            exit(1);
        }

        ptedit_use_implementation(PTEDIT_IMPL_USER);
        pteditor_init_flag = true;
    }

    PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
    /* 1. alloc vm area by anonymous mmap syscall */
    addr = mmap(addr, length, prot, MAP_ANONYMOUS | MAP_PRIVATE, -1, offset);
    if (addr == MAP_FAILED) err(EXIT_FAILURE, "mmap");

    /* 2. make vm area's page-faults handled by user level: register userfaultfd
     */
    /* Create and enable userfaultfd object. */
    pthread_t thr; /* ID of thread that handles page faults */
    struct uffdio_api uffdio_api;
    struct uffdio_register uffdio_register;

    const long uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd == -1) err(EXIT_FAILURE, "userfaultfd");

    uffdio_api.api = UFFD_API;
    uffdio_api.features = 0;
    if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
        err(EXIT_FAILURE, "ioctl-UFFDIO_API");

    /* Register the memory range of the mapping we just created for
       handling by the userfaultfd object. we request to track
       missing pages (i.e., pages that have not yet been faulted in). */

    uffdio_register.range.start = (unsigned long)addr;
    uffdio_register.range.len = length;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
        err(EXIT_FAILURE, "ioctl-UFFDIO_REGISTER");

    /* Create a thread that will process the userfaultfd events. */

    int dup_fd = -1;
    if (fd != -1) {
        dup_fd = dup(fd);
        if (dup_fd < 0) {
            printf("Failed to duplicate fd\n");
            exit(1);
        }
    }
    auto pfh_args = std::make_shared<PFhandle_args>(uffd, dup_fd, offset, addr);
    std::thread thread(page_fault_handler, pfh_args);
    pfh_args->thread = std::move(thread);

    std::lock_guard<std::mutex> guard(mmap_regions_mu);
    mmap_regions[addr] = pfh_args;

    return addr;
}

// FIXME
int ul_munmap(void *addr, size_t length) {
    std::lock_guard<std::mutex> guard(mmap_regions_mu);
    auto pfh_args = mmap_regions.find(addr);
    if (pfh_args != mmap_regions.end()) {
        // in final version, should use region's buffer pool.
        // Then Write back dirty page and release physical mems will be simple
        //
        // if is file-backed mmap, write back dirty pages
        // release physical mems

        // munmap: delete vma
        munmap(addr, length);
        // clear corresponse PTEs (does munmap help us clear PTEs?)

        // release uffdio
        struct uffdio_range uffdio_range;
        uffdio_range.start = (__u64)addr;
        uffdio_range.len = length;
        ioctl(pfh_args->second->uffd, UFFDIO_UNREGISTER, &uffdio_range);

        pfh_args->second->finish = true;
        pfh_args->second->thread.join();
        mmap_regions.erase(pfh_args);
    } else {
        printf("ul_munmap: get none exist mmaping address %p\n", addr);
    }
    return 0;
}
