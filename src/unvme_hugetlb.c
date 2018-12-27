
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

#define NR_HUGEPAGE_PATH					\
    "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
#define HUGEPAGE_SIZE 2 * 1024 * 1024	/* 2048kB */

static hugetlb_ctx hctx;


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

int hugetlb_init(void)
{
    int ret = 0, n, fd;
    char buf[16];

    memset(&hctx, 0, sizeof(hctx));

    /* obtain number of hugepages */
    fd = open(NR_HUGEPAGE_PATH, O_RDONLY);
    if (fd < 0) {
	FATAL("failed to open %s: %s", NR_HUGEPAGE_PATH,
	      strerror(errno));
    }

    ret = read(fd, buf, sizeof(buf));
    if (ret < 0) {
	FATAL("failed to read %s: %s", NR_HUGEPAGE_PATH,
	      strerror(errno));
    }

    hctx.size = atoi(buf);
    if (hctx.size < 0)
	FATAL("invalid number of hugepages %d", hctx.size);
    close(fd);

    /* allocate hugepages */
    hctx.vaddrs = zalloc(sizeof(void*) * hctx.size);
    hctx.paddrs = zalloc(sizeof(u64) * hctx.size);

    for (n = 0; n < hctx.size; n++) {
	hctx.vaddrs[n] = mmap(0, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS |
			      MAP_LOCKED | MAP_HUGETLB, -1, 0);
	if (hctx.vaddrs[n] == MAP_FAILED)
	    FATAL("mmap hugepage failed: %s", strerror(errno));

	hctx.paddrs[n] = phy_addr(hctx.vaddrs[n]);
    }

    /* initialize used bit. each bit in used indicates 
     * a corresponding v/paddr is used or not.
     */
    
    hctx.used_size = ((hctx.size + 63) >> 6) << 3;
    hctx.used = zalloc(hctx.used_size);
    memset(hctx.used, 0, hctx.used_size);

    DEBUG_FN("%d hugepages allocated. usze_size=%d", hctx.size, hctx.used_size);

    return ret;
}


int hugetlb_mem_alloc(vfio_dma_t *dma, size_t size)
{
    int n;
    u64 mask, *ptr;

    if (size > HUGEPAGE_SIZE)
	FATAL("too large memory size to be allocated: %u", size);

    /* find free hugepage */
    mask = 1;
    ptr = hctx.used;

    for (n = 0; n < hctx.size; n++) {
	if ((mask & *ptr) == 0) {
	    *ptr = (*ptr | mask); /* set this bit to used */
	    break;
	}

	if (n && ((n + 1) % 64) == 0) {
	    /* this u64 is checked. go to next u64 in hctx.used */
	    mask = 1;
	    ptr++;
	} else {
	    mask <<= 1;
	}
    }

    if (n == hctx.size)
	FATAL("all hugepages are allocated\n");

    dma->size = HUGEPAGE_SIZE;
    dma->buf = hctx.vaddrs[n];
    dma->addr = hctx.paddrs[n];

    hctx.used_num++;
    DEBUG_FN("%s: %d pages allocaed", __func__, hctx.used_num);

    return 0;
}

int hugetlb_mem_free(vfio_dma_t *dma)
{
    int n;
    u64 mask, *ptr;

    /* find the bit of this address */
    mask = 1;
    ptr = hctx.used;
    for (n = 0; n < hctx.size; n++) {
	if (hctx.vaddrs[n] == dma->buf) {
	    *ptr = (*ptr & ~mask);	/* drop this bit */
	    hctx.used_num--;
	    DEBUG_FN("%s: %d pages allocaed", __func__, hctx.used_num);
	    return 0;
	}

	if (n && ((n + 1) % 64) == 0) {
	    /* this u64 is checked. go to next u64 in hctx.used */
	    mask = 1;
	    ptr++;
	} else {
	    mask <<= 1;
	}
    }


    FATAL("%s: no memory found: vaddr=%#lx paddr=%#lx", dma->buf, dma->addr);
    return -1;	/* not reached */
}
