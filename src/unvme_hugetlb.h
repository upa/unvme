
#ifndef _UNVME_HUGETLB_H
#define _UNVME_HUGETLB_H


#include "unvme.h"
#include "unvme_vfio.h"

typedef struct _hugetlb_ctx {
    int		size;		///< number of hugepages
    int		assigned;	///< number of assigned hugepages

    void	**vaddrs;	///< virtual addresses
    u64		*paddrs;	///< physical addresses

    int		used_size;	///< size of 'used'
    u64		*used;		///< each bit indicates which addr is used
    int		used_num;	///< number of used hugepages
} hugetlb_ctx;


int hugetlb_init(void);
int hugetlb_mem_alloc(vfio_dma_t *dma, size_t size);
int hugetlb_mem_free(vfio_dma_t *dma);


#endif /* _UNVME_HUGETLB_H */
