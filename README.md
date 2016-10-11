UNVMe - A User Space NVMe Driver
================================

UNVMe is a user space NVMe driver developed at Micron Technology.


To build and install, run:

    $ make install

This will compile and then copy:

    unvme.h         to  /usr/local/include directory
    libunvme.a      to  /usr/local/lib directory


The UNVMe APIs are designed with application ease of use in mind, and as
defined in unvme.h, composed of the following supported functions:

    unvme_open()    -   This function must be invoked first to establish a
                        connection to the specified PCI device (e.g. 05:00.0).
                        The qcount and qsize parameters provide the application 
                        the ability to adjust its performance based on the
                        device characteristics.  For best practice and best
                        application performance, the number of queues parameter
                        should be the same as the number of threads used by
                        the application.  The number of queues, however, is
                        limited by the device support.

    unvme_close()   -   Close a device connection.

    unvme_alloc()   -   Allocate an I/O memory buffer of a given size.

    unvme_free()    -   Free the allocated buffer.


    unvme_write()   -   Write the specified number of blocks (nlb) to the device
                        starting at logical block address (slba).  The buffer
                        must be from unvme_alloc() and may be specified at
                        512-byte offset granularity.  The qid (range from 0 to
                        qcount - 1) may be used for thread safety.  Each queue
                        must only be accessed by a single thread at any one time.

    unvme_read()    -   Read from the device (like unvme_write).


    unvme_awrite()  -   Send a write command to the device asynchronously and
                        return immediately.  The returned descriptor is used to
                        poll for completion.

    unvme_aread()   -   Send an asynchronous read (like unvme_awrite).

    unvme_apoll()   -   Poll an asynchronous read/write for completion.


Prior to using the UNVMe driver, the script test/unvme-setup needs to be run once.
It will bind all (or a specified list of) NVMe devices in the system to be used with
the UNVMe driver.  For usage info, run:

    $ test/unvme-setup help

    Usage:
        unvme-setup                 # enable all NVMe devices for UNVMe
        unvme-setup [BB:DD.F]...    # enable specific NVMe devices for UNVMe
        unvme-setup show            # show all NVMe devices mapping info
        unvme-setup reset           # reset all NVMe devices to kernel driver


To run UNVMe basic tests (after unvme-setup):

    $ test/unvme-test PCINAME       # PCINAME as 01:00.0



I/O Benchmark Tests
===================

To run fio benchmark tests against UNVMe:

    1) Compile the fio source code (available on https://github.com/axboe/fio).

    2) Edit Makefile.def and set FIO_DIR to the compiled fio source code.
       Setting FIO_DIR will enable ioengine/unvme_fio to be built.

    3) Recompile the UNVMe code to include fio engine code, run:
    
       $ make

       Note that the unvme_fio.c engine has been verified to work with the fio
       versions 2.7 through 2.14 (as fio source code is constantly changing).

    4) Launch the test script:
    
       $ test/unvme-benchmark PCINAME

       Note the benchmark test will, by default, run random read and write tests
       with queue count 1, 4, 8, 16 and queue depth of 1, 4, 8, 16, 32, 64.
       Each test will be run for 7 minutes which includes a 2-minute ramp time.
       These default settings can be overridden from the shell command line, e.g.:

       $ RAMPTIME=60 RUNTIME=120 QCOUNT="1 4" QDEPTH="4 8" test/unvme-benchmark 05:00.0

       If the specified PCINAME argument begins with /dev/nvme, the test will assume
       the SSD is bound to the kernel space driver and thus use the "libaio" engine.
       Otherwise, if the PCINAME matches the device format BB:DD:F, the test will assume
       the SSD is bound to UNVMe driver and thus will use the "ioengine/unvme_fio" engine.
       The benchmark results will be saved in test/out directory.

       For comparison, unvme-benchmark should be run on the same device binding to the
       kernel space driver as well as the user space UVNMe driver.  For example, run:

       $ test/unvme-setup
       $ test/unvme-benchmark 01:00.0
       $ test/unvme-setup reset
       $ test/unvme-benchmark /dev/nvme0n1



System Requirements
===================

UNVMe depends on features provided by the VFIO module in the Linux kernel
(introduced since 3.6).  UNVMe code has been built and tested with CentOS 6 and 7
running on x86_64 CPU based systems.

UNVMe requires the following hardware and software support:

    VT-d    -   CPU must support VT-d (Virtualization Technology for Directed I/O).
                Check <http://ark.intel.com/> for Intel product specifications.

    VFIO    -   Linux kernel 3.6 or later compiled with the following configurations:

                    CONFIG_IOMMU_API=y
                    CONFIG_IOMMU_SUPPORT=y
                    CONFIG_INTEL_IOMMU=y
                    CONFIG_VFIO=m
                    CONFIG_VFIO_PCI=m
                    CONFIG_VFIO_IOMMU_TYPE1=m

                The boot command line must set "intel_iommu=on" argument.

                To verify the system correctly configured with VFIO support,
                check that /sys/kernel/iommu_groups directory is not empty but
                contains other subdirectories (i.e. group numbers).

                On CentOS 6, which comes with kernel version 2.6, the user must
                compile and boot a newer kernel that has the VFIO module.
                The user must also copy the header file from the kernel source
                directory include/uapi/linux/vfio.h to /usr/include/linux.



Design Notes
============

The UNVMe source code, under the src directory, contains four independent modules:

    VFIO    -   VFIO supported device and I/O memory wrapper functions
                (unvme_vfio.h unvme_vfio.c)

    NVMe    -   NVMe supported functions
                (unvme_nvme.h unvme_nvme.c)

    Log     -   Simple logging supported functions
                (unvme_log.h unvme_log.c)

    UNVMe   -   User Space driver interface built on top of the other three modules
                (unvme.h unvme.c unvme_core.h unvme_core.c)


The test/nvme directory contains a few examples of NVMe admin commands constructed
using the VFIO, NVMe, and Log modules (without depending on the UNVMe module).
The user can build more NVMe admin commands based on those examples.

This UNVMe API design is a result of a more extensive work done previously
which is now archived in the "all_models" branch.  The revised APIs are designed
to make the tasks of application development and porting easier.  UNVMe allows an
application to allocate any size buffer (limited by available memory) as well as
read/write any number of blocks (beyond hardware limitations) in a single request
thus eliminating the need for applications to be aware and build logic around
hardware limitations.

