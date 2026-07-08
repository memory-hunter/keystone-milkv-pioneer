#include <linux/ioctl.h>

#define XTCMO_DEVICE_NAME "xtcmo"
#define XTCMO_DEVICE_PATH "/dev/" XTCMO_DEVICE_NAME

#define XTCMO_IOCTL_MAGIC 0xf0

struct xtcmo_param {
    unsigned long pa;
    unsigned long value;
};

#define XTCMO_IOCTL_L1D_CALL \
    _IO(XTCMO_IOCTL_MAGIC, 0)

#define XTCMO_IOCTL_L1D_IALL \
    _IO(XTCMO_IOCTL_MAGIC, 1)

#define XTCMO_IOCTL_L1D_CIALL \
    _IO(XTCMO_IOCTL_MAGIC, 7)

#define XTCMO_IOCTL_L1D_IPA \
    _IOW(XTCMO_IOCTL_MAGIC, 2, unsigned long)

#define XTCMO_IOCTL_L1D_IVA \
    _IOW(XTCMO_IOCTL_MAGIC, 3, unsigned long)

#define XTCMO_IOCTL_L1D_ISW \
    _IOW(XTCMO_IOCTL_MAGIC, 4, unsigned long)

#define XTCMO_IOCTL_L2_CALL \
    _IO(XTCMO_IOCTL_MAGIC, 5)

#define XTCMO_IOCTL_L2_IALL \
    _IO(XTCMO_IOCTL_MAGIC, 9)

#define XTCMO_IOCTL_L2_CIALL \
    _IO(XTCMO_IOCTL_MAGIC, 8)

#define XTCMO_IOCTL_L2_ISW \
    _IOW(XTCMO_IOCTL_MAGIC, 10, unsigned long)

#define XTCMO_LEVEL_TEST_KM \
    _IO(XTCMO_IOCTL_MAGIC, 11)

#define XTCMO_IOCTL_FLUSH_ALL \
    _IO(XTCMO_IOCTL_MAGIC, 6)
