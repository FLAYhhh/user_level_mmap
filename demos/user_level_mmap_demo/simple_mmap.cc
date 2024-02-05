#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "user_level_mmap.h"

int main(int argc, char *argv[]) {
    int s;
    char c;
    char *addr;    /* Start of region handled by userfaultfd */
    long uffd;     /* userfaultfd file descriptor */
    size_t len, l; /* Length of region handled by userfaultfd */

    if (argc != 2) {
        fprintf(stderr, "Usage: %s num-pages\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int page_size = 4096;

    len = strtoull(argv[1], NULL, 0) * page_size;

    addr = (char *)ul_mmap((void *)NULL, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        printf("mmap failed\n");
        exit(1);
    }

    printf("Address returned by mmap() = %p\n", addr);

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
