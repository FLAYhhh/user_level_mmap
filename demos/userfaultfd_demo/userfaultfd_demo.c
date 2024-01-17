/* userfaultfd_demo.c
Licensed under the GNU General Public License version 2 or later.
*/
#define _GNU_SOURCE
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ptedit_header.h>

#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

#define TAG_OK COLOR_GREEN "[+]" COLOR_RESET " "
#define TAG_FAIL COLOR_RED "[-]" COLOR_RESET " "
#define TAG_PROGRESS COLOR_YELLOW "[~]" COLOR_RESET " "

static int page_size;

static void *
fault_handler_thread(void *arg) {
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
            int result = posix_memalign((void **)&bufpool[i], page_size, page_size);
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
        printf("flags = %"PRIx64"; ", msg.arg.pagefault.flags);
        printf("address = %"PRIx64"\n", msg.arg.pagefault.address);

        /* Copy the page pointed to by 'page' into the faulting
           region. Vary the contents that are copied in, so that it
           is more obvious that each fault is handled separately. */

        assert(fault_cnt < BUFPOOLSIZE);
        const char *given_page = bufpool[fault_cnt++];
        memset((void *)given_page, 'A' + fault_cnt % 20, page_size);

        // 1. get the pfd of given_page
        size_t given_page_pfn = ptedit_pte_get_pfn((void *)given_page, 0);
        // debug: print given_page
        // {
        //     ptedit_entry_t given_page_vm = ptedit_resolve((void *)given_page, 0);
        //     ptedit_print_entry_t(given_page_vm);
        //     printf(TAG_PROGRESS "given page vm %zx\n", (size_t)(ptedit_cast(given_page_vm.pte, ptedit_pte_t).pfn));
        // }
        // 2. get the ptedit_entry of fault address
        ptedit_entry_t vm = ptedit_resolve(msg.arg.pagefault.address, 0);
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
        ptedit_update(msg.arg.pagefault.address, 0, &vm);

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
        uffdio_range.start = (unsigned long) msg.arg.pagefault.address & ~(page_size - 1);
        uffdio_range.len = page_size;
        if (ioctl(uffd, UFFDIO_WAKE, &uffdio_range) == -1)
            err(EXIT_FAILURE, "ioctl-UFFDIO_WAKE");

        printf("       uffdio_wake returned\n");
    }
}

int
main(int argc, char *argv[]) {
    int s;
    char c;
    char *addr; /* Start of region handled by userfaultfd */
    long uffd; /* userfaultfd file descriptor */
    size_t len, l; /* Length of region handled by userfaultfd */
    pthread_t thr; /* ID of thread that handles page faults */
    struct uffdio_api uffdio_api;
    struct uffdio_register uffdio_register;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s num-pages\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    page_size = sysconf(_SC_PAGE_SIZE);
    len = strtoull(argv[1], NULL, 0) * page_size;

    /* Create and enable userfaultfd object. */

    uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd == -1)
        err(EXIT_FAILURE, "userfaultfd");

    uffdio_api.api = UFFD_API;
    uffdio_api.features = 0;
    if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
        err(EXIT_FAILURE, "ioctl-UFFDIO_API");

    /* Create a private anonymous mapping. The memory will be
       demand-zero paged--that is, not yet allocated. When we
       actually touch the memory, it will be allocated via
       the userfaultfd. */

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED)
        err(EXIT_FAILURE, "mmap");

    printf("Address returned by mmap() = %p\n", addr);

    /* Register the memory range of the mapping we just created for
       handling by the userfaultfd object. In mode, we request to track
       missing pages (i.e., pages that have not yet been faulted in). */

    uffdio_register.range.start = (unsigned long) addr;
    uffdio_register.range.len = len;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
        err(EXIT_FAILURE, "ioctl-UFFDIO_REGISTER");

    /* Create a thread that will process the userfaultfd events. */

    s = pthread_create(&thr, NULL, fault_handler_thread, (void *) uffd);
    if (s != 0) {
        errx(EXIT_FAILURE, s, "pthread_create");
    }

    /* Main thread now touches memory in the mapping, touching
       locations 1024 bytes apart. This will trigger userfaultfd
       events for all pages in the region. */

    l = 0xf; /* Ensure that faulting address is not on a page
                          boundary, in order to test that we correctly
                          handle that case in fault_handling_thread(). */
    while (l < len) {
        c = addr[l];
        printf("Read address %p in %s(): ", addr + l, __func__);
        printf("%c\n", c);
        l += 1024;
        usleep(100000); /* Slow things down a little */
    }

    exit(EXIT_SUCCESS);
}
