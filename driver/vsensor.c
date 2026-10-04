// SPDX-License-Identifier: GPL-2.0
/*
 * vsensor.c - Virtual temperature/humidity sensor, as a character device.
 *
 * What it does
 *   - A kernel timer fires every `interval_ms` and produces a new simulated
 *     reading (a bounded random walk).
 *   - User space reads struct vsensor_reading from /dev/vsensor. read()
 *     BLOCKS until a reading newer than the one this file descriptor last
 *     saw is available, so readers never need to busy-poll.
 *   - ioctl() changes / queries the sampling interval.
 *   - `interval_ms` is also a module parameter, visible and writable at
 *     /sys/module/vsensor/parameters/interval_ms.
 *
 * Key design points (good interview material)
 *   1. The timer callback runs in softirq (atomic) context: it must NOT
 *      sleep, so it cannot take a mutex. We protect shared data with a
 *      spinlock and use spin_lock_irqsave() in both contexts.
 *   2. copy_to_user()/copy_from_user() may fault and sleep, so they are
 *      NEVER called while holding the spinlock: we snapshot the data into a
 *      local variable under the lock, drop the lock, then copy out.
 *   3. Blocking read uses a wait queue + wait_event_interruptible(), so a
 *      signal (Ctrl-C) can interrupt a sleeping reader.
 *   4. Per-open state (last_seq) lives in file->private_data, so several
 *      readers each get every sample independently.
 *   5. A timer that re-arms itself must be stopped carefully on unload:
 *      set `stopping`, then timer_delete_sync(), so it cannot re-arm after
 *      we have waited for it.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "vsensor_ioctl.h"

#define DRV_NAME        VSENSOR_DEV_NAME
#define MIN_INTERVAL_MS 10U
#define MAX_INTERVAL_MS 60000U

#define TEMP_MIN_MC   15000
#define TEMP_MAX_MC   45000
#define HUM_MIN_MPCT  20000
#define HUM_MAX_MPCT  90000

/* ---- Kernel API differences between versions --------------------------- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define vs_timer_delete_sync(t) timer_delete_sync(t)
#else
#define vs_timer_delete_sync(t) del_timer_sync(t)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define vs_class_create(name) class_create(name)
#else
#define vs_class_create(name) class_create(THIS_MODULE, name)
#endif

/* ---- Module parameter --------------------------------------------------- */
static unsigned int interval_ms = 1000;
module_param(interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms, "Sampling interval in ms (10..60000, default 1000)");

/* ---- Driver state ------------------------------------------------------- */
struct vsensor_dev {
	dev_t devt;
	struct cdev cdev;
	struct class *cls;
	struct timer_list timer;
	spinlock_t lock;               /* protects `latest` and `rng` */
	wait_queue_head_t wq;          /* readers sleep here          */
	struct vsensor_reading latest;
	u32 rng;
	bool stopping;
};

static struct vsensor_dev vs;

/* One of these per open() of the device. */
struct vsensor_file {
	u32 last_seq;  /* sequence number of the last reading this fd consumed */
};

static unsigned int vs_get_interval(void)
{
	/* interval_ms may be changed from sysfs at any time, so validate on
	 * every use instead of trusting it. */
	return clamp_t(unsigned int, READ_ONCE(interval_ms),
		       MIN_INTERVAL_MS, MAX_INTERVAL_MS);
}

/* Tiny linear congruential generator. Caller must hold vs.lock. */
static u32 vs_rand(void)
{
	vs.rng = vs.rng * 1664525u + 1013904223u;
	return vs.rng >> 16;
}

/* Produce the next simulated sample. Caller must hold vs.lock. */
static void vs_generate(void)
{
	s32 t = vs.latest.temp_mc + ((s32)(vs_rand() % 2001) - 1000);
	s32 h = vs.latest.hum_mpct + ((s32)(vs_rand() % 2001) - 1000);

	vs.latest.temp_mc  = clamp_t(s32, t, TEMP_MIN_MC, TEMP_MAX_MC);
	vs.latest.hum_mpct = clamp_t(s32, h, HUM_MIN_MPCT, HUM_MAX_MPCT);
	vs.latest.seq++;
}

/* ---- Timer callback (softirq context: no sleeping!) ---------------------- */
static void vsensor_timer_cb(struct timer_list *t)
{
	unsigned long flags;

	spin_lock_irqsave(&vs.lock, flags);
	vs_generate();
	spin_unlock_irqrestore(&vs.lock, flags);

	wake_up_interruptible(&vs.wq);          /* wake any blocked readers */

	if (!READ_ONCE(vs.stopping))
		mod_timer(&vs.timer, jiffies + msecs_to_jiffies(vs_get_interval()));
}

/* ---- File operations ----------------------------------------------------- */
static int vsensor_open(struct inode *inode, struct file *filp)
{
	struct vsensor_file *priv = kzalloc(sizeof(*priv), GFP_KERNEL);

	if (!priv)
		return -ENOMEM;
	filp->private_data = priv;
	return nonseekable_open(inode, filp);
}

static int vsensor_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static ssize_t vsensor_read(struct file *filp, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct vsensor_file *priv = filp->private_data;
	struct vsensor_reading snap;
	unsigned long flags;
	int ret;

	if (count < sizeof(snap))
		return -EINVAL;

	/* Wait for a reading this fd has not seen yet. */
	if (READ_ONCE(vs.latest.seq) == priv->last_seq) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(vs.wq,
			READ_ONCE(vs.latest.seq) != priv->last_seq);
		if (ret)
			return ret;     /* -ERESTARTSYS: interrupted by a signal */
	}

	/* Snapshot under the lock ... */
	spin_lock_irqsave(&vs.lock, flags);
	snap = vs.latest;
	spin_unlock_irqrestore(&vs.lock, flags);

	priv->last_seq = snap.seq;

	/* ... and copy to user space with the lock released. */
	if (copy_to_user(buf, &snap, sizeof(snap)))
		return -EFAULT;

	return sizeof(snap);
}

static long vsensor_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	u32 val;

	if (_IOC_TYPE(cmd) != VSENSOR_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case VSENSOR_IOC_SET_INTERVAL:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
			return -EFAULT;
		if (val < MIN_INTERVAL_MS || val > MAX_INTERVAL_MS)
			return -EINVAL;
		WRITE_ONCE(interval_ms, val);
		pr_info("interval set to %u ms\n", val);
		return 0;

	case VSENSOR_IOC_GET_INTERVAL:
		val = vs_get_interval();
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations vsensor_fops = {
	.owner          = THIS_MODULE,
	.open           = vsensor_open,
	.release        = vsensor_release,
	.read           = vsensor_read,
	.unlocked_ioctl = vsensor_ioctl,
};

/* ---- Module init / exit --------------------------------------------------- */
static int __init vsensor_init(void)
{
	struct device *dev;
	int ret;

	WRITE_ONCE(interval_ms, vs_get_interval());

	spin_lock_init(&vs.lock);
	init_waitqueue_head(&vs.wq);
	vs.rng = 0x1234abcdu;
	vs.latest.temp_mc  = 25000;
	vs.latest.hum_mpct = 50000;
	vs.stopping = false;
	vs_generate();                      /* seq becomes 1: data from the start */

	ret = alloc_chrdev_region(&vs.devt, 0, 1, DRV_NAME);
	if (ret)
		return ret;

	cdev_init(&vs.cdev, &vsensor_fops);
	vs.cdev.owner = THIS_MODULE;
	ret = cdev_add(&vs.cdev, vs.devt, 1);
	if (ret)
		goto err_region;

	vs.cls = vs_class_create(DRV_NAME);
	if (IS_ERR(vs.cls)) {
		ret = PTR_ERR(vs.cls);
		goto err_cdev;
	}

	dev = device_create(vs.cls, NULL, vs.devt, NULL, DRV_NAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_class;
	}

	/* Start the timer last: everything it touches is ready now. */
	timer_setup(&vs.timer, vsensor_timer_cb, 0);
	mod_timer(&vs.timer, jiffies + msecs_to_jiffies(vs_get_interval()));

	pr_info("loaded: /dev/%s (major %d), interval %u ms\n",
		DRV_NAME, MAJOR(vs.devt), vs_get_interval());
	return 0;

err_class:
	class_destroy(vs.cls);
err_cdev:
	cdev_del(&vs.cdev);
err_region:
	unregister_chrdev_region(vs.devt, 1);
	return ret;
}

static void __exit vsensor_exit(void)
{
	WRITE_ONCE(vs.stopping, true);   /* 1: forbid the callback to re-arm   */
	vs_timer_delete_sync(&vs.timer); /* 2: wait for a running callback     */

	device_destroy(vs.cls, vs.devt);
	class_destroy(vs.cls);
	cdev_del(&vs.cdev);
	unregister_chrdev_region(vs.devt, 1);
	pr_info("unloaded\n");
}

module_init(vsensor_init);
module_exit(vsensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vivek Kumar");
MODULE_DESCRIPTION("Virtual temperature/humidity sensor character driver");
MODULE_VERSION("1.0");
