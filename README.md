UNVMe - A User Space NVMe Driver
================================

UNVMe is a user space NVMe driver developed at Micron Technology.


To build and install, run:

    $ make install

This will install unvme.h in the /usr/local/include directory
and libunvme.a in the /usr/local/lib directory.


The APIs are designed with application ease of use in mind.

    unvme_open()    -   This function must be invoked first to establish a
                        connection to the specified PCI device (e.g. 05:00.0).
                        The qcount and qsize parameters provide the application 
                        the ability to adjust its performance based on the
                        device characteristics.  For best performance, the
                        number of queues should be used to associate with the
                        number of application threads.  The number of queues,
                        however, will be limited by the device support.
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


Prior to using the UNVMe driver, the script test/unvme-setup needs to be run
once.  By default, it will bind all NVMe devices in the system to be used with
UNVMe driver.  For usage info, run:

    $ test/unvme-setup help
    Usage:
        unvme-setup                 # enable all NVMe devices for UNVMe
        unvme-setup [BB:DD.F]...    # enable specific NVMe devices for UNVMe
        unvme-setup reset           # reset all NVMe devices to kernel driver
        unvme-setup show            # show all NVMe devices mapping info


Note that the previous release with implementation of various models is now
archived in the "all_models" git branch.  This implementation is the
result of such experimental work and porting applications.  The primary
feature here is that it allows an application to allocate any size buffer
(limited only by available memory) and to read/write any number of blocks.



System Requirements
===================

UNVMe depends on features provided by the VFIO module in the Linux kernel
(introduced since 3.6).  The code has been built and tested on CentOS 6
and 7 running on x86_64 CPU based systems.  

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


