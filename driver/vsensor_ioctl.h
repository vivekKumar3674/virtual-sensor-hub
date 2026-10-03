/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vsensor_ioctl.h - interface shared by the kernel driver and user space.
 *
 * Both sides include this file, so the layout of struct vsensor_reading and
 * the ioctl numbers can never get out of sync.
 */
#ifndef VSENSOR_IOCTL_H
#define VSENSOR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VSENSOR_DEV_NAME "vsensor"

/* One sensor sample. Fixed-size, fixed-width types: safe to copy across the
 * user/kernel boundary. Values are in milli-units to avoid floating point in
 * the kernel (floating point must not be used in kernel code). */
struct vsensor_reading {
	__u32 seq;       /* increases by 1 for every new sample            */
	__s32 temp_mc;   /* temperature in milli-degrees Celsius (25000=25C) */
	__s32 hum_mpct;  /* relative humidity in milli-percent (50000=50%)   */
	__u32 reserved;  /* padding / future use, always 0                  */
};

#define VSENSOR_IOC_MAGIC 'v'
/* Set the sampling interval in milliseconds (valid range 10..60000). */
#define VSENSOR_IOC_SET_INTERVAL _IOW(VSENSOR_IOC_MAGIC, 1, __u32)
/* Get the current sampling interval in milliseconds. */
#define VSENSOR_IOC_GET_INTERVAL _IOR(VSENSOR_IOC_MAGIC, 2, __u32)

#endif /* VSENSOR_IOCTL_H */
