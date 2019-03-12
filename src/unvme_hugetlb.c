
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>

#include "unvme_core.h"
#include "unvme_vfio.h"
#include "unvme_hugetlb.h"


static uintptr_t phy_addr(void* virt) {
    int fd;
    long pagesize;
    off_t ret;
    ssize_t rc;
    uintptr_t entry = 0;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0)
        FATAL("open /proc/self/pagemap:", strerror(errno));

    pagesize = sysconf(_SC_PAGESIZE);

    ret = lseek(fd, (uintptr_t)virt / pagesize * sizeof(uintptr_t), SEEK_SET);
    if (ret < 0)
        FATAL("lseek for /proc/self/pagemap: %s\n", strerror(errno));


    rc = read(fd, &entry, sizeof(entry));
    if (rc < 1 || entry == 0)
        FATAL("read for /proc/self/pagemap: %s\n", strerror(errno));

    close(fd);

    return (entry & 0x7fffffffffffffULL) * pagesize +
           ((uintptr_t)virt) % pagesize;
}


int hugetlb_mem_alloc(vfio_dma_t *dma, size_t size)
{
    void *buf;

    buf = mmap(0, size, PROT_READ | PROT_WRITE,
	       MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED | MAP_HUGETLB, -1, 0);
    if (buf == MAP_FAILED)
	FATAL("mmap %lu-byte in HUGETLB failed");

    dma->size = size;
    dma->buf = buf;
    dma->addr = phy_addr(buf);

    DEBUG_FN("%s: %lu-byte allocated in hugepage", __func__, size);

    return 0;
}

int hugetlb_mem_free(vfio_dma_t *dma)
{
    return 0;	/* how to unmap hugepage? */
}
