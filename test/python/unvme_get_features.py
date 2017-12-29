#!/bin/python
# Example to invoke admin get features command.
#

import sys, struct, ctypes, unvme
from unvme import *

# Main
try:
    pci = sys.argv[1]
except:
    print('Usage: python %s PCINAME\n' % sys.argv[0])
    sys.exit(1)

features = [ '',
             '1)  Arbitration:',
             '2)  Power Management:',
             '3)  LBA Range Type:', 
             '4)  Temperature Threshold:',
             '5)  Error Recovery:', 
             '6)  Volatile Write Cache:', 
             '7)  Number of Queues:', 
             '8)  Interrupt Coalescing:',
             '9)  Interrupt Vector Config:',
             '10) Write Atomicity:',
             '11) Async Event Config:'
           ]

unvme = Unvme(pci)
buf = unvme.alloc(4096)
res = ctypes.c_uint32()
dw6 = ctypes.c_uint32 * 6

for fid in range(1, 12):
    cdw10_15 = dw6(fid, 0, 0, 0, 0, 0)
    err = unvme.cmd(-1, 10, 1, buf, 4096, cdw10_15, ctypes.byref(res))

    if err:
        print('%-30s <feature not supported>' % features[fid])
    elif fid == 1:
        arb = struct.unpack('<BBBB', res)
        print('%-30s hpw=%u mpw=%u lpw=%u ab=%u' % (features[fid], arb[3], arb[2], arb[1], arb[0] & 3))
    elif fid == 2:
        print('%-30s ps=%u' % (features[fid], res.value & 0x1f))
    elif fid == 3:
        print('%-30s num=%u' % (features[fid], res.value & 0x3f))
    elif fid == 4:
        print('%-30s tmpth=%u' % (features[fid], res.value & 0xffff))
    elif fid == 5:
        print('%-30s tler=%u' % (features[fid], res.value & 0xffff))
    elif fid == 6:
        print('%-30s wce=%u' % (features[fid], res.value & 1))
    elif fid == 7:
        print('%-30s nsq=%u ncq=%u' % (features[fid], res.value & 0xffff, res.value >> 16))
    elif fid == 8:
        print('%-30s time=%u thr=%u' % (features[fid], res.value & 0xff, (res.value >> 8) & 0xff))
    elif fid == 9:
        print('%-30s iv=%u cd=%u' % (features[fid], res.value & 0xffff, (res.value >> 16) & 1))
    elif fid == 10:
        print('%-30s dn=%u' % (features[fid], res.value & 1))
    elif fid == 11:
        print('%-30s smart=%u' % (features[fid], res.value & 0xff))

unvme.free(buf)
unvme.close()

