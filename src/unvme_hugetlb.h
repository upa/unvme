
#ifndef _UNVME_HUGETLB_H
#define _UNVME_HUGETLB_H


#include "unvme.h"
#include "unvme_vfio.h"

int hugetlb_mem_alloc(vfio_dma_t *dma, size_t size);
int hugetlb_mem_free(vfio_dma_t *dma);


#endif /* _UNVME_HUGETLB_H */
