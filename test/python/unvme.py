# UNVMe Python wrapper module

import os, sys, ctypes
from ctypes import *

# Namespace attributes structure
class unvme_ns(ctypes.Structure):
    _fields_ = [
        ("pci", c_uint32),          # PCI device id
        ("id", c_uint16),           # namespace id
        ("vid", c_uint16),          # vendor id
        ("device", c_char*16),      # PCI device name (BB:DD.F/N)
        ("mn", c_char*40),          # model number
        ("sn", c_char*20),          # serial number
        ("fr", c_char*8),           # firmware revision
        ("blockcount", c_uint64),   # total number of available blocks
        ("pagecount", c_uint64),    # total number of available pages
        ("blocksize", c_uint16),    # logical block size
        ("pagesize", c_uint16),     # page size
        ("blockshift", c_uint16),   # block size shift value
        ("pageshift", c_uint16),    # page size shift value
        ("bpshift", c_uint16),      # block to page shift
        ("nbpp", c_uint16),         # number of blocks per page
        ("maxbpio", c_uint16),      # max number of blocks per I/O
        ("maxppio", c_uint16),      # max number of pages per I/O
        ("maxiopq", c_uint16),      # max number of I/O submissions per queue
        ("nscount", c_uint16),      # number of namespaces available
        ("qcount", c_uint32),       # number of I/O queues
        ("maxqcount", c_uint32),    # max number of queues supported
        ("qsize", c_uint32),        # I/O queue size
        ("maxqsize", c_uint32),     # max queue size supported
        ("ses", POINTER(c_uint64))  # associated session
    ]

# I/O descriptor structure
class unvme_iod(ctypes.Structure):
    _fields = [
        ("buf", POINTER(c_uint32)), # data buffer (submitted)
        ("slba", c_uint64),         # starting lba (submitted)
        ("nlb", c_uint32),          # number of blocks (submitted)
        ("qid", c_uint32),          # queue id (submitted)
        ("opc", c_uint32),          # op code
        ("id", c_uint32),           # descriptor id
    ]

# Load libunvme
libso = os.path.dirname(os.path.realpath(sys.argv[0])) + "/../../src/libunvme.so"
if not os.path.exists(libso):
    libso = "/usr/local/lib/libunvme.so"
libunvme = cdll.LoadLibrary(libso)


# UNVMe library functions
def unvme_open(pci):
    open = libunvme.unvme_open
    open.restype = POINTER(unvme_ns)
    return open(pci)

def unvme_close(ns):
    close = libunvme.unvme_close
    close.argtypes = [POINTER(unvme_ns)]
    return close(ns)

def unvme_alloc(ns, size):
    alloc = libunvme.unvme_alloc
    alloc.argtypes = [POINTER(unvme_ns), c_uint64]
    alloc.restype = POINTER(c_uint64)
    return alloc(ns, size)

def unvme_free(ns, buf):
    free = libunvme.unvme_free
    free.argtypes = [POINTER(unvme_ns), POINTER(c_uint64)]
    return free(ns, buf)

def unvme_write(ns, qid, buf, slba, nlb):
    write = libunvme.unvme_write
    write.argtypes = [POINTER(unvme_ns), c_int, POINTER(c_uint64), c_uint64, c_uint32]
    return write(ns, qid, buf, slba, nlb)

def unvme_read(ns, qid, buf, slba, nlb):
    read = libunvme.unvme_read
    read.argtypes = [POINTER(unvme_ns), c_int, POINTER(c_uint64), c_uint64, c_uint32]
    return read(ns, qid, buf, slba, nlb)

def unvme_cmd(ns, qid, opc, nsid, buf, bufsz, cdw10_15, cqe_cs):
    cmd = libunvme.unvme_cmd
    cmd.argtypes = [POINTER(unvme_ns), c_int, c_int, c_int, POINTER(c_uint64), c_uint64, POINTER(c_uint32), POINTER(c_uint32)]
    return cmd(ns, qid, opc, nsid, buf, bufsz, cdw10_15, cqe_cs)

def unvme_awrite(ns, qid, buf, slba, nlb):
    awrite = libunvme.unvme_awrite
    awrite.argtypes = [POINTER(unvme_ns), c_int, POINTER(c_uint64), c_uint64, c_uint32]
    awrite.restype = POINTER(unvme_iod)
    return awrite(ns, qid, buf, slba, nlb)

def unvme_aread(ns, qid, buf, slba, nlb):
    aread = libunvme.unvme_aread
    aread.argtypes = [POINTER(unvme_ns), c_int, POINTER(c_uint64), c_uint64, c_uint32]
    aread.restype = POINTER(unvme_iod)
    return aread(ns, qid, buf, slba, nlb)

def unvme_acmd(ns, qid, opc, nsid, buf, bufsz, cdw10_15):
    acmd = libunvme.unvme_acmd
    acmd.argtypes = [POINTER(unvme_ns), c_int, c_int, c_int, POINTER(c_uint64), c_uint64, POINTER(c_uint32)]
    return acmd(ns, qid, opc, nsid, buf, bufsz, cdw10_15)

def unvme_apoll(iod, timeout):
    apoll = libunvme.unvme_apoll
    apoll.argtypes = [POINTER(unvme_iod), c_int]
    return apoll(iod, timeout)

def unvme_apoll_cs(iod, timeout, cqe_cs):
    apoll = libunvme.unvme_apoll_cs
    apoll.argtypes = [POINTER(unvme_iod), c_int, POINTER(c_uint32)]
    return apoll(iod, timeout, cqe_cs)


# UNVMe class
class Unvme(object):
    def __init__(self, pci=None):
        self.ns = None
        self.open(pci)

    def __del__(self):
        self.close()

    def open(self, pci):
        if self.ns:
            raise Exception("already opened")
        self.ns = unvme_open(pci)
        if not self.ns:
            raise Exception("unvme_open error")

    def close(self):
        if self.ns:
            stat = unvme_close(self.ns)
            if stat:
                raise Exception("unvme_close error")
            self.ns = None

    def info(self):
        return self.ns.contents

    def alloc(self, size):
        return unvme_alloc(self.ns, size)

    def free(self, buf):
        return unvme_free(self.ns, buf)

    def write(self, qid, buf, slba, nlb):
        return unvme_write(self.ns, qid, buf, slba, nlb)

    def read(self, qid, buf, slba, nlb):
        return unvme_read(self.ns, qid, buf, slba, nlb)

    def cmd(self, qid, opc, nsid, buf, bufsz, cdw10_15, cqe_cs):
        return unvme_cmd(self.ns, qid, opc, nsid, buf, bufsz, cdw10_15, cqe_cs)

    def awrite(self, qid, buf, slba, nlb):
        return unvme_awrite(self.ns, qid, buf, slba, nlb)

    def aread(self, qid, buf, slba, nlb):
        return unvme_aread(self.ns, qid, buf, slba, nlb)

    def acmd(self, qid, opc, nsid, buf, bufsz, cdw10_15):
        return unvme_acmd(self.ns, qid, opc, nsid, buf, bufsz, cdw10_15)

    def apoll(self, iod, timeout):
        return unvme_apoll(iod, timeout)

    def apoll_cs(self, iod, timeout, cqe_cs):
        return unvme_apoll_cs(iod, timeout, cqe_cs)

