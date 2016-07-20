/**
 * Copyright (c) 2015-2016, Micron Technology, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief UNVMe client library common functions.
 */

#include <stddef.h>
#include <sched.h>

#include "unvme_core.h"


/// Global lock for open/close/alloc/free
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

unvme_session_t* unvme_do_open(int vfid, int nsid, int qcount, int qsize);
int unvme_do_close(int sid);
void* unvme_do_alloc(unvme_session_t* ses, u64 size);
int unvme_do_free(unvme_session_t* ses, void* buf);
unvme_desc_t* unvme_do_submit(unvme_queue_t* ioq, int opc, void* buf, u64 slba, u32 nlb);
int unvme_do_poll(unvme_desc_t* iod, int sec);


/**
 * Open a client session to create io queues.
 * @param   pciname     PCI device name (as BB:DD.F format)
 * @param   nsid        namespace id
 * @param   qcount      number of io queues
 * @param   qsize       io queue size
 * @return  namespace pointer or NULL if error.
 */
const unvme_ns_t* unvme_open(const char* pciname, int nsid, int qcount, int qsize)
{
    if (qcount < 1 || qsize < 2) {
        ERROR("qcount must be > 0 and qsize must be > 1");
        return NULL;
    }

    int b, d, f;
    if (sscanf(pciname, "%02x:%02x.%x", &b, &d, &f) != 3) {
        ERROR("invalid PCI device %s (expect BB:DD.F format)", pciname);
        return NULL;
    }
    int pci = (b << 16) + (d << 8) + f;

    pthread_mutex_lock(&lock);
    unvme_session_t* ses = unvme_do_open(pci, nsid, qcount, qsize);
    pthread_mutex_unlock(&lock);
    return ses ? &ses->ns : NULL;
}

/**
 * Close a client session and delete its contained io queues.
 * @param   ns          namespace handle
 * @return  0 if ok else error code.
 */
int unvme_close(const unvme_ns_t* ns)
{
    unvme_session_t* ses = (unvme_session_t*)ns->ses;
    pthread_mutex_lock(&lock);
    unvme_do_close(ses->id);
    pthread_mutex_unlock(&lock);
    return 0;
}

/**
 * Allocate an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   size        buffer size
 * @return  the allocated buffer or NULL if failure.
 */
void* unvme_alloc(const unvme_ns_t* ns, u64 size)
{
    unvme_session_t* ses = (unvme_session_t*)ns->ses;
    return unvme_do_alloc(ses, size);
}

/**
 * Free an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   buf         buffer pointer
 * @return  0 if ok else -1.
 */
int unvme_free(const unvme_ns_t* ns, void* buf)
{
    unvme_session_t* ses = (unvme_session_t*)ns->ses;
    return unvme_do_free(ses, buf);
}

/**
 * Poll for completion status of a previous IO submission.
 * If there's no error, the descriptor will be released.
 * @param   iod         IO descriptor
 * @param   timeout     in seconds
 * @return  0 if ok else error status.
 */
int unvme_apoll(unvme_iod_t iod, int timeout)
{
    return unvme_do_poll(iod, timeout);
}

/**
 * Read data from specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_aread(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb)
{
    unvme_queue_t* ioq = ((unvme_session_t*)(ns->ses))->queues + qid;
    return unvme_do_submit(ioq, NVME_CMD_READ, buf, slba, nlb);
}

/**
 * Write data to specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_awrite(const unvme_ns_t* ns, int qid,
                         const void* buf, u64 slba, u32 nlb)
{
    unvme_queue_t* ioq = ((unvme_session_t*)(ns->ses))->queues + qid;
    return unvme_do_submit(ioq, NVME_CMD_WRITE, (void*)buf, slba, nlb);
}

/**
 * Read data from specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_read(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb)
{
    unvme_queue_t* ioq = ((unvme_session_t*)(ns->ses))->queues + qid;
    unvme_desc_t* desc = unvme_do_submit(ioq, NVME_CMD_READ, buf, slba, nlb);
    if (desc) return unvme_do_poll(desc, UNVME_TIMEOUT);
    return -1;
}

/**
 * Write data to specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_write(const unvme_ns_t* ns, int qid,
                const void* buf, u64 slba, u32 nlb)
{
    unvme_queue_t* ioq = ((unvme_session_t*)(ns->ses))->queues + qid;
    unvme_desc_t* desc = unvme_do_submit(ioq, NVME_CMD_WRITE, (void*)buf, slba, nlb);
    if (desc) return unvme_do_poll(desc, UNVME_TIMEOUT);
    return -1;
}

