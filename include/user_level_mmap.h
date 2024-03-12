#pragma once
#include <fcntl.h>
#include <stddef.h>
#include "sys/mman.h"

/**
 * \brief Create a memory mapping. Currently, only two usages are supported:
 *        1. Anonymous memory mapping
 *        2. File-backed memory mapping
 *
 * \param addr Specifies the starting address of the mapping area. Typically set to NULL, allowing to automatically choose a suitable address.
 * \param length The size of the file or device to be mapped (in bytes).
 * \param prot Specifies the access permissions of the mapped area, which can be a combination of PROT_READ, PROT_WRITE
 * \param flags Specifies the type and attributes of the mapping, which can be a combination of MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS, etc.
 * \param fd The file descriptor of the file or device to be mapped. If using MAP_ANONYMOUS, set it to -1.
 * \param offset The offset of the file or device, starting the mapping from this offset. It must be a multiple of the system page size.
 * \return On success, returns the starting address of the mapped area; on failure, returns MAP_FAILED (usually (void *)-1) and sets errno.
 */
#define INTERRUPT_MODE 0 
#define NO_INTERRUPT_MODE 1
void *ul_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset, int mode);

/**
 * \brief ul_munmap() deletes the mappings for the specified address range, and causes further references to addresses within the range to generate invalid memory references.  The  region is also automatically unmapped when the process is terminated. On the other hand, closing the file descriptor does not unmap the region.
 * Warnning: ul_munmap is not same with munmap, you must unmap all address range you get from ul_mmap.
 *
 * \param addr range start, Must be a multiple of the page size
 * \param length range length (must be the same as you gived in mmap)
 */
int ul_munmap(void *addr, size_t length);

int ul_msync(void *addr, size_t length, int flags);
int ul_mprotect(void *addr, size_t length, int prot);
int ul_madvise(void *addr, size_t length, int advice);
void touch_page(void *addr);
