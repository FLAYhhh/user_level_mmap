#include "user_level_mmap.h"

#include <err.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <unordered_map>
#include <poll.h>
#include <cassert>
#include <ptedit_header.h>

//std::unorderd_map<void *,
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET "\x1b[0m"
#define TAG_PROGRESS COLOR_YELLOW "[~]" COLOR_RESET " "

static int PAGE_SIZE;

static void *
page_fault_handler(void *arg) {
    int nready;
    long uffd; /* userfaultfd file descriptor */
    ssize_t nread;
    struct pollfd pollfd;
    // struct uffdio_copy uffdio_copy;
    struct uffdio_range uffdio_range;

    static int fault_cnt = 0; /* Number of faults so far handled */
#define BUFPOOLSIZE 1024
    static char *bufpool[BUFPOOLSIZE]; // use fault_cnt as index to get buf
    static struct uffd_msg msg; /* Data read from userfaultfd */

    uffd = (long) arg;

    /* Init */
    if (fault_cnt == 0) {
        /* Create a buffer pool that will be allocated to the faulting region. */
        for (int i = 0; i < BUFPOOLSIZE; i++) {
            int result = posix_memalign((void **)&bufpool[i], PAGE_SIZE, PAGE_SIZE);
            if (result != 0) {
                printf("Error: posix_memalign failed with error code %d\n", result);
                exit(1);
            }
        }

        /*init PTEditor*/
        if (ptedit_init()) {
            printf("Error: Could not initalize PTEditor, did you load the kernel module?\n");
            exit(1);
        }

        ptedit_use_implementation(PTEDIT_IMPL_USER);
    }

    /* Loop, handling incoming events on the userfaultfd
       file descriptor. */

    for (;;) {
        /* See what poll() tells us about the userfaultfd. */

        pollfd.fd = uffd;
        pollfd.events = POLLIN;
        nready = poll(&pollfd, 1, -1);
        if (nready == -1)
            err(EXIT_FAILURE, "poll");

        printf("\nfault_handler_thread():\n");
        printf("    poll() returns: nready = %d; "
               "POLLIN = %d; POLLERR = %d\n", nready,
               (pollfd.revents & POLLIN) != 0,
               (pollfd.revents & POLLERR) != 0);

        /* Read an event from the userfaultfd. */

        nread = read(uffd, &msg, sizeof(msg));
        if (nread == 0) {
            printf("EOF on userfaultfd!\n");
            exit(EXIT_FAILURE);
        }

        if (nread == -1)
            err(EXIT_FAILURE, "read");

        /* We expect only one kind of event; verify that assumption. */

        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            fprintf(stderr, "Unexpected event on userfaultfd\n");
            exit(EXIT_FAILURE);
        }

        /* Display info about the page-fault event. */

        printf("    UFFD_EVENT_PAGEFAULT event: ");
        printf("flags = %llx; ", msg.arg.pagefault.flags);
        printf("address = %llx\n", msg.arg.pagefault.address);

        /* Copy the page pointed to by 'page' into the faulting
           region. Vary the contents that are copied in, so that it
           is more obvious that each fault is handled separately. */

        assert(fault_cnt < BUFPOOLSIZE);
        const char *given_page = bufpool[fault_cnt++];
        memset((void *)given_page, 'A' + fault_cnt % 20, PAGE_SIZE);

        // 1. get the pfd of given_page
        size_t given_page_pfn = ptedit_pte_get_pfn((void *)given_page, 0);
        // debug: print given_page
        // {
        //     ptedit_entry_t given_page_vm = ptedit_resolve((void *)given_page, 0);
        //     ptedit_print_entry_t(given_page_vm);
        //     printf(TAG_PROGRESS "given page vm %zx\n", (size_t)(ptedit_cast(given_page_vm.pte, ptedit_pte_t).pfn));
        // }
        // 2. get the ptedit_entry of fault address
        ptedit_entry_t vm = ptedit_resolve((void *)msg.arg.pagefault.address, 0);
        // debug:print page fault address
        // {
        //     ptedit_print_entry_t(vm);
        //     printf(TAG_PROGRESS "fault address %zx\n", (size_t)(ptedit_cast(vm.pte, ptedit_pte_t).pfn));
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
        //     printf(TAG_PROGRESS "updated fault address %zx\n", (size_t)(ptedit_cast(vm.pte, ptedit_pte_t).pfn));
        // }

        // debug: try to access the page
        {
            char ch = *(char*)(msg.arg.pagefault.address);
            printf(TAG_PROGRESS "Try to access page fault address: %c\n", ch);
        }

        /* We need to handle page faults in units of pages(!).
            So, round faulting address down to page boundary. */
        uffdio_range.start = (unsigned long) msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
        uffdio_range.len = PAGE_SIZE;
        if (ioctl(uffd, UFFDIO_WAKE, &uffdio_range) == -1)
            err(EXIT_FAILURE, "ioctl-UFFDIO_WAKE");

        printf("       uffdio_wake returned\n");
    }
}

/**
 * use kernel's mmap implementation to reserve vm area.
 * then, use userfaultfd to delegate page fault handle to user space.
 */
void *ul_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
    /* 1. alloc vm area by anonymous mmap syscall */
    addr = mmap(addr, length, prot, flags, -1, offset);
    if (addr == MAP_FAILED)
        err(EXIT_FAILURE, "mmap");

    /* 2. make vm area's page-faults handled by user level: register userfaultfd */
    /* Create and enable userfaultfd object. */
    pthread_t thr; /* ID of thread that handles page faults */
    struct uffdio_api uffdio_api;
    struct uffdio_register uffdio_register;

    const long uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd == -1)
        err(EXIT_FAILURE, "userfaultfd");

    uffdio_api.api = UFFD_API;
    uffdio_api.features = 0;
    if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
        err(EXIT_FAILURE, "ioctl-UFFDIO_API");


    printf("Address returned by mmap() = %p\n", addr);

    /* Register the memory range of the mapping we just created for
       handling by the userfaultfd object. we request to track
       missing pages (i.e., pages that have not yet been faulted in). */

    uffdio_register.range.start = (unsigned long) addr;
    uffdio_register.range.len = length;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
        err(EXIT_FAILURE, "ioctl-UFFDIO_REGISTER");

    /* Create a thread that will process the userfaultfd events. */

    int ret = pthread_create(&thr, NULL, page_fault_handler, (void *) uffd);
    if (ret != 0) {
        errx(EXIT_FAILURE, "pthread_create failed");
    }

    return addr;
}


int ul_munmap(void *addr, size_t length) {
    return 0 
}
