/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
#ifndef _SGI_L1_IOCTL_H
#define _SGI_L1_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SGIL1_VENDOR_ID		0x065e
#define SGIL1_PRODUCT_ID	0x1234

#define SGIL1_MAX_DEVICES	40
#define SGIL1_MAX_LEVEL		6

struct sgil1_cfg {
	__u8 bus;
	__u8 level;
	__u8 dev;
	__u8 path[SGIL1_MAX_LEVEL];
};

typedef struct sgil1_cfg sgil1_cfg_t;

#define SGIL1_IOCTL_BASE	'S'

#define SGIL1_RESET_READ	_IO(SGIL1_IOCTL_BASE, 1)
#define SGIL1_RESET_WRITE	_IO(SGIL1_IOCTL_BASE, 2)
#define SGIL1_RESET_DEVICE	_IO(SGIL1_IOCTL_BASE, 3)
#define SGIL1_READ_CFG		_IOR(SGIL1_IOCTL_BASE, 4, struct sgil1_cfg)
#define SGIL1_RESET_PIPES	_IO(SGIL1_IOCTL_BASE, 5)

#define SGIL1_ST_READ_REV	_IOR(SGIL1_IOCTL_BASE, 6, char[64])
/*
 * Original SGI L2/L3 binaries were built against an older header that encoded
 * this status ioctl with sizeof(int).  Keep the distinct command number
 * available so the driver can accept those binaries without changing the
 * modern, bounded revision-buffer ABI above.
 */
#define SGIL1_ST_READ_REV_LEGACY	_IOR(SGIL1_IOCTL_BASE, 6, int)
#define SGIL1_ST_READ_DEV_CFG	_IOWR(SGIL1_IOCTL_BASE, 7, struct sgil1_cfg)

#endif /* _SGI_L1_IOCTL_H */
