#!/bin/python
# Example to open a device and print its namespace info.
#
# This program shows 2 ways to invoke Python UNVMe interfaces:
#   1) To use direct functions, specify device with /NSID, e.g. 01:00.0/1
#   2) To use Unvme class, specify device without slash, e.g. 01:00.0
#

import sys, unvme
from unvme import *

# Print device info
def print_info(ns):
    print("PCI device:          %06x" % ns.pci)
    print("Namespace:           %d (of %d)" % (ns.id, ns.nscount))
    print("Vendor ID:           %#x" % ns.vid)
    print("Model number:        %.40s" % str(ns.mn))
    print("Serial number:       %.20s" % str(ns.sn))
    print("FW revision:         %.8s" % str(ns.fr))
    print("Block count:         %#lx" % ns.blockcount)
    print("Page count:          %#lx" % ns.pagecount)
    print("Block size:          %d" % ns.blocksize)
    print("Page size :          %d" % ns.pagesize)
    print("Blocks per page:     %d" % ns.nbpp)
    print("Max blocks per IO:   %d" % ns.maxbpio)
    print("Max IO queue count:  %d" % ns.maxqcount)
    print("Max IO queue size:   %d" % ns.maxqsize)


# Main
try:
    pci = sys.argv[1]
except:
    print("Usage: python %s PCINAME\n" % sys.argv[0])
    print("1) To invoke functions, specify PCINAME with /NSID, e.g. 01:00.0/1")
    print("2) To invoke Unvme class, specify PCINAME without slash, e.g. 01:00.0")
    sys.exit(1);

if ('/' in pci):
    print("USING UNVMe FUNCTIONS")
    print("=====================")
    ns = unvme_open(pci)
    if not ns:
        sys.exit(1)
    print_info(ns.contents)
    unvme_close(ns)
else:
    print("USING UNVMe CLASS")
    print("=================")
    unvme = Unvme(pci)
    print_info(unvme.info())

