
#include "unvme.h"

pop_mem_t *pop_mem = NULL;

void unvme_register_pop_mem(pop_mem_t *mem)
{
    pop_mem = mem;
}

uintptr_t unvme_pop_virt_to_phys(void *buf)
{
    if (!pop_mem)
	return 0;
	
    return pop_virt_to_phys(pop_mem, buf);
}
