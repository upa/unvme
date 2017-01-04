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
 * @brief User space NVMe client header file.
 */

#ifndef _UNVME_H
#define _UNVME_H

#include <stdint.h>


#ifndef _U_TYPE
#define _U_TYPE                     ///< bit size data types
typedef int8_t          s8;         ///< 8-bit signed
typedef int16_t         s16;        ///< 16-bit signed
typedef int32_t         s32;        ///< 32-bit signed
typedef int64_t         s64;        ///< 64-bit signed
typedef uint8_t         u8;         ///< 8-bit unsigned
typedef uint16_t        u16;        ///< 16-bit unsigned
typedef uint32_t        u32;        ///< 32-bit unsigned
typedef uint64_t        u64;        ///< 64-bit unsigned
#endif // _U_TYPE

#define UNVME_TIMEOUT   60          ///< I/O timeout in seconds


/// Namespace attributes structure
typedef struct _unvme_ns {
    u16                 id;         ///< namespace id
    u16                 vid;        ///< vendor id
    u32                 sid;        ///< session id
    u32                 qcount;     ///< number of I/O queues
    u32                 qsize;      ///< I/O queue size
    char                sn[20];     ///< device serial number
    char                mn[40];     ///< namespace model number
    char                fr[8];      ///< namespace firmware revision
    u64                 blockcount; ///< total number of logical blocks
    u32                 pagesize;   ///< page size
    u32                 blocksize;  ///< logical block size
    u16                 pageshift;  ///< page size shift value
    u16                 blockshift; ///< block size shift value
    u32                 nbpp;       ///< number of blocks per page
    u32                 maxppio;    ///< max number of pages per I/O
    u32                 maxbpio;    ///< max number of blocks per I/O
    u16                 maxiopq;    ///< max concurrent I/O per queue
    u16                 maxqsize;   ///< max queue size supported
    void*               ses;        ///< associated session
} unvme_ns_t;

/// I/O descriptor
typedef void*           unvme_iod_t;

// Export functions
const unvme_ns_t* unvme_open(const char* pciname, int nsid, int qcount, int qsize);
int unvme_close(const unvme_ns_t* ns);

void* unvme_alloc(const unvme_ns_t* ns, u64 size);
int unvme_free(const unvme_ns_t* ns, void* buf);

int unvme_write(const unvme_ns_t* ns, int qid, const void* buf, u64 slba, u32 nlb);
int unvme_read(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb);

unvme_iod_t unvme_awrite(const unvme_ns_t* ns, int qid, const void* buf, u64 slba, u32 nlb);
unvme_iod_t unvme_aread(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb);
int unvme_apoll(unvme_iod_t iod, int timeout);

#endif // _UNVME_H

