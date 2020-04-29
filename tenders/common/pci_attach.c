#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/limits.h>
#include <linux/vfio.h>
#include <linux/mman.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <stdlib.h>
#include <ctype.h>
#include <assert.h>

#include "pci_attach.h"

#define check_err(expr, op) ({\
    int64_t result = (int64_t) (expr);\
    if ((int64_t) result == -1LL) {\
        int err = errno;\
        char buf[512];\
        strerror(err);\
        fprintf(stderr, "[ERROR] %s:%d %s(): Failed to %s: %s\n", __FILE__, __LINE__, __func__, op, buf);\
        exit(err);\
    }\
    result;\
})

#define info(fmt, ...) do {\
    fprintf(stdout, "[INFO ] %s:%d %s(): " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__);\
} while(0)

#define warn(fmt, ...) do {\
    fprintf(stderr, "[WARN ] %s:%d %s(): " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__);\
} while(0)

#define error(fmt, ...) do {\
    fprintf(stderr, "[ERROR] %s:%d %s(): " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__);\
    abort();\
} while(0)

volatile int VFIO_CONTAINER_FILE_DESCRIPTOR = -1;

int get_vfio_container()
{
    return VFIO_CONTAINER_FILE_DESCRIPTOR;
}

void set_vfio_container(int fd)
{
    VFIO_CONTAINER_FILE_DESCRIPTOR = fd;
}

size_t MIN_DMA_MEMORY = 4096; // we can not allocate less than page_size memory

void pci_enable_dma(int vfio_fd) {
    // write to the command register (offset 4) in the PCIe config space
    int command_register_offset = 4;
    // bit 2 is "bus master enable", see PCIe 3.0 specification section 7.5.1.1
    int bus_master_enable_bit = 2;
    // Get region info for config region
    struct vfio_region_info conf_reg = {.argsz = sizeof(conf_reg)};
    conf_reg.index = VFIO_PCI_CONFIG_REGION_INDEX;
    check_err(ioctl(vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &conf_reg), "get vfio config region info");
    uint16_t dma = 0;
    assert(pread(vfio_fd, &dma, 2, conf_reg.offset + command_register_offset) == 2);
    dma |= 1 << bus_master_enable_bit;
    assert(pwrite(vfio_fd, &dma, 2, conf_reg.offset + command_register_offset) == 2);
}

// returns the devices file descriptor or -1 on error
int pci_attach(const char *pci_addr) {
    // find iommu group for the device
    // `readlink /sys/bus/pci/device/<segn:busn:devn.funcn>/iommu_group`
    char path[PATH_MAX], iommu_group_path[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/", pci_addr);
    int ret = stat(path, &st);
    if (ret < 0) {
        // No such device
        warn("no such device");
        return -1;
    }
    strncat(path, "iommu_group", sizeof(path) - strlen(path) - 1);

    int len = check_err(readlink(path, iommu_group_path, sizeof(iommu_group_path)), "find the iommu_group for the device");

    iommu_group_path[len] = '\0'; // append 0x00 to the string to end it
    char* group_name = basename(iommu_group_path);
    int groupid;
    check_err(sscanf(group_name, "%d", &groupid), "convert group id to int");

    int firstsetup = 0; // Need to set up the container exactly once
    int cfd = get_vfio_container();
    if (cfd == -1) {
        firstsetup = 1;
        // open vfio file to create new vfio container
        cfd = check_err(open("/dev/vfio/vfio", O_RDWR), "open /dev/vfio/vfio");
        set_vfio_container(cfd);

        // check if the container's API version is the same as the VFIO API's
        check_err((ioctl(cfd, VFIO_GET_API_VERSION) == VFIO_API_VERSION) - 1, "get a valid API version from the container");

        // check if type1 is supported
        check_err((ioctl(cfd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) == 1) - 1, "get Type1 IOMMU support from the IOMMU container");
    }

    // open VFIO group containing the device
    snprintf(path, sizeof(path), "/dev/vfio/%d", groupid);
    int vfio_gfd = check_err(open(path, O_RDWR), "open vfio group");

    // check if group is viable
    struct vfio_group_status group_status = {.argsz = sizeof(group_status)};
    check_err(ioctl(vfio_gfd, VFIO_GROUP_GET_STATUS, &group_status), "get VFIO group status");
    check_err(((group_status.flags & VFIO_GROUP_FLAGS_VIABLE) > 0) - 1, "get viable VFIO group - are all devices in the group bound to the VFIO driver?");

    // Add group to container
    check_err(ioctl(vfio_gfd, VFIO_GROUP_SET_CONTAINER, &cfd), "set container");

    if (firstsetup != 0) {
        // Set vfio type (type1 is for IOMMU like VT-d or AMD-Vi) for the
        // container.
        // This can only be done after at least one group is in the container.
        ret = check_err(ioctl(cfd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU), "set IOMMU type");
    }

    // get device file descriptor
    int vfio_fd = check_err(ioctl(vfio_gfd, VFIO_GROUP_GET_DEVICE_FD, pci_addr), "get device fd");

    return vfio_fd;
}

void *pci_map_region(int vfio_fd, int region_index, size_t *size) {
    struct vfio_region_info region_info = {.argsz = sizeof(region_info)};
    region_info.index = region_index;
    int ret = ioctl(vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &region_info);
    if (ret == -1) {
        // Failed to set iommu type
        return MAP_FAILED;
    }

    void *buffer = (void *) check_err(mmap(NULL, region_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, vfio_fd, region_info.offset), "mmap vfio resource");

    if (buffer == MAP_FAILED) {
        warn("map failed");
        return MAP_FAILED;
    }

    *size = region_info.size;

    return buffer;
}

void *pci_allocate_dma(size_t size) {
    return (void *) check_err(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0), "mmap hugepage");
}
