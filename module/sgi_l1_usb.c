// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Modern Linux USB transport driver for SGI L1/L3 system controllers.
 *
 * The original SGI driver exposed a raw bulk transport.  It was not a tty:
 * user space writes complete IRouter frames, and the driver overwrites the
 * first two bytes with the big-endian frame length before sending them.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/wait.h>

#include <sgi_l1_ioctl.h>

#define SGI_L1_DRIVER_VERSION	"sgi-l1-usb 0.1.0"
#define SGI_L1_MINOR_BASE	208
#define SGI_L1_STATUS_MINOR	249
#define SGI_L1_MAX_TRANSFER	4096
#define SGI_L1_WRITE_TIMEOUT_MS	3000

struct sgi_l1_device {
	struct usb_device *udev;
	struct usb_interface *interface;
	struct kref kref;

	struct mutex io_mutex;
	struct mutex open_mutex;
	spinlock_t state_lock;
	wait_queue_head_t read_wait;

	struct urb *read_urb;
	u8 *read_buf;
	dma_addr_t read_dma;
	u8 *write_buf;

	unsigned int bulk_in_pipe;
	unsigned int bulk_out_pipe;
	u8 bulk_in_endpoint;
	u8 bulk_out_endpoint;
	size_t read_size;
	size_t write_size;

	struct sgil1_cfg cfg;
	u8 minor;
	u8 index;

	bool present;
	bool open;
	bool read_pending;
	bool read_ready;
	int read_status;
	int read_count;
};

struct sgi_l1_status_file {
	long seen_generation;
};

static DEFINE_MUTEX(sgi_l1_table_lock);
static struct sgi_l1_device *sgi_l1_devices[SGIL1_MAX_DEVICES];
static DECLARE_WAIT_QUEUE_HEAD(sgi_l1_status_wait);
static atomic_long_t sgi_l1_status_generation = ATOMIC_LONG_INIT(1);

static bool reset_on_close;
module_param(reset_on_close, bool, 0644);
MODULE_PARM_DESC(reset_on_close,
		 "reset the USB device when the char device is closed; disabled by default");

static int sgi_l1_probe(struct usb_interface *interface,
			const struct usb_device_id *id);
static void sgi_l1_disconnect(struct usb_interface *interface);

static const struct usb_device_id sgi_l1_id_table[] = {
	{ USB_DEVICE(SGIL1_VENDOR_ID, SGIL1_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, sgi_l1_id_table);

static void sgi_l1_status_changed(void)
{
	atomic_long_inc(&sgi_l1_status_generation);
	wake_up_interruptible(&sgi_l1_status_wait);
}

static void sgi_l1_delete(struct kref *kref)
{
	struct sgi_l1_device *dev = container_of(kref, struct sgi_l1_device,
						 kref);

	usb_free_urb(dev->read_urb);

	if (dev->read_buf)
		usb_free_coherent(dev->udev, dev->read_size, dev->read_buf,
				  dev->read_dma);

	kfree(dev->write_buf);
	usb_put_dev(dev->udev);
	kfree(dev);
}

static void sgi_l1_read_complete(struct urb *urb)
{
	struct sgi_l1_device *dev = urb->context;
	unsigned long flags;

	spin_lock_irqsave(&dev->state_lock, flags);
	dev->read_pending = false;
	dev->read_status = urb->status;
	dev->read_count = urb->actual_length;
	dev->read_ready = true;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	wake_up_interruptible(&dev->read_wait);
}

static int sgi_l1_submit_read_locked(struct sgi_l1_device *dev, gfp_t gfp)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->state_lock, flags);
	if (!dev->present) {
		spin_unlock_irqrestore(&dev->state_lock, flags);
		return -ENODEV;
	}
	if (dev->read_pending || dev->read_ready) {
		spin_unlock_irqrestore(&dev->state_lock, flags);
		return 0;
	}
	dev->read_pending = true;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	usb_fill_bulk_urb(dev->read_urb, dev->udev, dev->bulk_in_pipe,
			  dev->read_buf, dev->read_size, sgi_l1_read_complete,
			  dev);
	dev->read_urb->transfer_dma = dev->read_dma;
	dev->read_urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

	ret = usb_submit_urb(dev->read_urb, gfp);
	if (ret) {
		spin_lock_irqsave(&dev->state_lock, flags);
		dev->read_pending = false;
		dev->read_status = ret;
		dev->read_count = 0;
		dev->read_ready = true;
		spin_unlock_irqrestore(&dev->state_lock, flags);
		wake_up_interruptible(&dev->read_wait);
	}

	return ret;
}

static int sgi_l1_open(struct inode *inode, struct file *file)
{
	struct sgi_l1_device *dev;
	unsigned long flags;
	int index = iminor(inode) - SGI_L1_MINOR_BASE;
	int ret = 0;

	if (index < 0 || index >= SGIL1_MAX_DEVICES)
		return -ENODEV;

	mutex_lock(&sgi_l1_table_lock);
	dev = sgi_l1_devices[index];
	if (dev && !kref_get_unless_zero(&dev->kref))
		dev = NULL;
	mutex_unlock(&sgi_l1_table_lock);

	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->open_mutex);
	if (dev->open) {
		mutex_unlock(&dev->open_mutex);
		kref_put(&dev->kref, sgi_l1_delete);
		return -EBUSY;
	}

	dev->open = true;
	file->private_data = dev;
	mutex_unlock(&dev->open_mutex);

	mutex_lock(&dev->io_mutex);
	spin_lock_irqsave(&dev->state_lock, flags);
	dev->read_pending = false;
	dev->read_ready = false;
	dev->read_status = 0;
	dev->read_count = 0;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	if (!dev->present)
		ret = -ENODEV;
	else
		ret = sgi_l1_submit_read_locked(dev, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);

	if (ret) {
		mutex_lock(&dev->open_mutex);
		dev->open = false;
		file->private_data = NULL;
		mutex_unlock(&dev->open_mutex);
		kref_put(&dev->kref, sgi_l1_delete);
		return ret;
	}

	stream_open(inode, file);
	return 0;
}

static int sgi_l1_release(struct inode *inode, struct file *file)
{
	struct sgi_l1_device *dev = file->private_data;
	unsigned long flags;
	bool do_reset = false;

	if (!dev)
		return 0;

	mutex_lock(&dev->io_mutex);
	usb_kill_urb(dev->read_urb);

	spin_lock_irqsave(&dev->state_lock, flags);
	dev->read_pending = false;
	dev->read_ready = false;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	do_reset = reset_on_close && dev->present;
	mutex_unlock(&dev->io_mutex);

	if (do_reset) {
		int ret = usb_reset_device(dev->udev);

		if (ret)
			dev_warn(&dev->interface->dev,
				 "USB reset on close failed: %d\n", ret);
	}

	mutex_lock(&dev->open_mutex);
	dev->open = false;
	file->private_data = NULL;
	mutex_unlock(&dev->open_mutex);

	kref_put(&dev->kref, sgi_l1_delete);
	return 0;
}

static ssize_t sgi_l1_read(struct file *file, char __user *buffer,
			   size_t count, loff_t *ppos)
{
	struct sgi_l1_device *dev = file->private_data;
	unsigned long flags;
	int status;
	int actual;
	int ret;
	bool ready;
	bool present;

	if (!dev)
		return -ENODEV;

	for (;;) {
		spin_lock_irqsave(&dev->state_lock, flags);
		ready = dev->read_ready;
		present = dev->present;
		spin_unlock_irqrestore(&dev->state_lock, flags);

		if (ready)
			break;
		if (!present)
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(dev->read_wait, ({
			bool condition;

			spin_lock_irqsave(&dev->state_lock, flags);
			condition = dev->read_ready || !dev->present;
			spin_unlock_irqrestore(&dev->state_lock, flags);
			condition;
		}));
		if (ret)
			return ret;
	}

	mutex_lock(&dev->io_mutex);

	spin_lock_irqsave(&dev->state_lock, flags);
	status = dev->read_status;
	actual = dev->read_count;
	ready = dev->read_ready;
	present = dev->present;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	if (!ready) {
		mutex_unlock(&dev->io_mutex);
		return -EAGAIN;
	}

	if (status) {
		spin_lock_irqsave(&dev->state_lock, flags);
		dev->read_ready = false;
		spin_unlock_irqrestore(&dev->state_lock, flags);

		if (present)
			sgi_l1_submit_read_locked(dev, GFP_KERNEL);

		mutex_unlock(&dev->io_mutex);
		return status;
	}

	if (actual > count) {
		mutex_unlock(&dev->io_mutex);
		return -EINVAL;
	}

	if (actual && copy_to_user(buffer, dev->read_buf, actual)) {
		mutex_unlock(&dev->io_mutex);
		return -EFAULT;
	}

	spin_lock_irqsave(&dev->state_lock, flags);
	dev->read_ready = false;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	if (present)
		sgi_l1_submit_read_locked(dev, GFP_KERNEL);

	mutex_unlock(&dev->io_mutex);
	return actual;
}

static ssize_t sgi_l1_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *ppos)
{
	struct sgi_l1_device *dev = file->private_data;
	int actual = 0;
	int ret;

	if (!dev)
		return -ENODEV;
	if (count < 2 || count > dev->write_size)
		return -EINVAL;

	mutex_lock(&dev->io_mutex);
	if (!dev->present) {
		mutex_unlock(&dev->io_mutex);
		return -ENODEV;
	}

	if (copy_from_user(dev->write_buf, buffer, count)) {
		mutex_unlock(&dev->io_mutex);
		return -EFAULT;
	}

	dev->write_buf[0] = (count >> 8) & 0xff;
	dev->write_buf[1] = count & 0xff;

	ret = usb_bulk_msg(dev->udev, dev->bulk_out_pipe, dev->write_buf,
			   count, &actual, SGI_L1_WRITE_TIMEOUT_MS);
	mutex_unlock(&dev->io_mutex);

	if (ret)
		return ret;
	if (actual != count)
		return -EIO;

	return count;
}

static __poll_t sgi_l1_poll(struct file *file, poll_table *wait)
{
	struct sgi_l1_device *dev = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	if (!dev)
		return EPOLLHUP;

	poll_wait(file, &dev->read_wait, wait);

	spin_lock_irqsave(&dev->state_lock, flags);
	if (!dev->present)
		mask |= EPOLLHUP;
	if (dev->read_ready)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static int sgi_l1_clear_halt(struct sgi_l1_device *dev, unsigned int pipe)
{
	int ret = usb_clear_halt(dev->udev, pipe);

	if (ret)
		dev_warn(&dev->interface->dev, "clear halt failed: %d\n", ret);

	return ret;
}

static long sgi_l1_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct sgi_l1_device *dev = file->private_data;
	unsigned long flags;
	int ret = 0;

	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->io_mutex);

	if (!dev->present) {
		mutex_unlock(&dev->io_mutex);
		return -ENODEV;
	}

	switch (cmd) {
	case SGIL1_RESET_READ:
		usb_kill_urb(dev->read_urb);
		spin_lock_irqsave(&dev->state_lock, flags);
		dev->read_pending = false;
		dev->read_ready = false;
		spin_unlock_irqrestore(&dev->state_lock, flags);
		ret = sgi_l1_submit_read_locked(dev, GFP_KERNEL);
		break;
	case SGIL1_RESET_WRITE:
		ret = sgi_l1_clear_halt(dev, dev->bulk_out_pipe);
		break;
	case SGIL1_RESET_PIPES:
		ret = sgi_l1_clear_halt(dev, dev->bulk_out_pipe);
		if (!ret)
			ret = sgi_l1_clear_halt(dev, dev->bulk_in_pipe);
		break;
	case SGIL1_RESET_DEVICE:
		ret = usb_reset_device(dev->udev);
		break;
	case SGIL1_READ_CFG:
		if (copy_to_user((void __user *)arg, &dev->cfg,
				 sizeof(dev->cfg)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&dev->io_mutex);
	return ret;
}

static const struct file_operations sgi_l1_fops = {
	.owner = THIS_MODULE,
	.open = sgi_l1_open,
	.release = sgi_l1_release,
	.read = sgi_l1_read,
	.write = sgi_l1_write,
	.poll = sgi_l1_poll,
	.unlocked_ioctl = sgi_l1_ioctl,
	.llseek = noop_llseek,
};

static struct usb_class_driver sgi_l1_class = {
	.name = "sgil1_%d",
	.fops = &sgi_l1_fops,
	.minor_base = SGI_L1_MINOR_BASE,
};

static int sgi_l1_status_open(struct inode *inode, struct file *file)
{
	struct sgi_l1_status_file *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->seen_generation = atomic_long_read(&sgi_l1_status_generation);
	file->private_data = state;

	return stream_open(inode, file);
}

static int sgi_l1_status_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t sgi_l1_status_read(struct file *file, char __user *buffer,
				  size_t count, loff_t *ppos)
{
	struct sgi_l1_status_file *state = file->private_data;
	u8 status[SGIL1_MAX_DEVICES];
	size_t n = min_t(size_t, count, sizeof(status));
	int i;

	memset(status, 0, sizeof(status));

	mutex_lock(&sgi_l1_table_lock);
	for (i = 0; i < SGIL1_MAX_DEVICES; i++)
		status[i] = sgi_l1_devices[i] ? 1 : 0;
	mutex_unlock(&sgi_l1_table_lock);

	if (copy_to_user(buffer, status, n))
		return -EFAULT;

	if (state)
		state->seen_generation =
			atomic_long_read(&sgi_l1_status_generation);

	return n;
}

static __poll_t sgi_l1_status_poll(struct file *file, poll_table *wait)
{
	struct sgi_l1_status_file *state = file->private_data;
	long generation;

	poll_wait(file, &sgi_l1_status_wait, wait);

	generation = atomic_long_read(&sgi_l1_status_generation);
	if (!state || state->seen_generation != generation)
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static long sgi_l1_status_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct sgil1_cfg cfg;
	char rev[64] = SGI_L1_DRIVER_VERSION;
	struct sgi_l1_device *dev = NULL;
	int ret = 0;

	switch (cmd) {
	case SGIL1_ST_READ_REV:
		if (copy_to_user((void __user *)arg, rev, sizeof(rev)))
			ret = -EFAULT;
		break;
	case SGIL1_ST_READ_DEV_CFG:
		if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
			return -EFAULT;
		if (cfg.dev >= SGIL1_MAX_DEVICES)
			return -EINVAL;

		mutex_lock(&sgi_l1_table_lock);
		dev = sgi_l1_devices[cfg.dev];
		if (dev)
			cfg = dev->cfg;
		mutex_unlock(&sgi_l1_table_lock);

		if (!dev)
			return -ENODEV;
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations sgi_l1_status_fops = {
	.owner = THIS_MODULE,
	.open = sgi_l1_status_open,
	.release = sgi_l1_status_release,
	.read = sgi_l1_status_read,
	.poll = sgi_l1_status_poll,
	.unlocked_ioctl = sgi_l1_status_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice sgi_l1_status_miscdev = {
	.minor = SGI_L1_STATUS_MINOR,
	.name = "sgil1_cs",
	.fops = &sgi_l1_status_fops,
	.mode = 0444,
};

static void sgi_l1_fill_cfg(struct sgi_l1_device *dev)
{
	struct usb_device *udev = dev->udev;
	struct usb_device *walk = udev;
	u8 path[SGIL1_MAX_LEVEL];
	int level = 0;
	int i;

	memset(&dev->cfg, 0, sizeof(dev->cfg));
	dev->cfg.bus = udev->bus->busnum;
	dev->cfg.dev = udev->devnum;

	while (walk && walk->parent && level < SGIL1_MAX_LEVEL) {
		path[level++] = walk->portnum;
		walk = walk->parent;
	}

	dev->cfg.level = level;
	for (i = 0; i < level; i++)
		dev->cfg.path[i] = path[level - i - 1];
}

static int sgi_l1_probe(struct usb_interface *interface,
			const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_host_interface *iface_desc = interface->cur_altsetting;
	struct sgi_l1_device *dev;
	int i;
	int ret;
	int index;
	bool have_bulk_in = false;
	bool have_bulk_out = false;

	if (!iface_desc || iface_desc->desc.bNumEndpoints != 2)
		return -ENODEV;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	mutex_init(&dev->io_mutex);
	mutex_init(&dev->open_mutex);
	spin_lock_init(&dev->state_lock);
	init_waitqueue_head(&dev->read_wait);

	dev->udev = usb_get_dev(udev);
	dev->interface = interface;
	dev->read_size = min_t(size_t, SGI_L1_MAX_TRANSFER, PAGE_SIZE);
	dev->write_size = min_t(size_t, SGI_L1_MAX_TRANSFER, PAGE_SIZE);
	dev->present = true;

	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *endpoint =
			&iface_desc->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(endpoint)) {
			dev->bulk_in_endpoint = usb_endpoint_num(endpoint);
			dev->bulk_in_pipe = usb_rcvbulkpipe(udev,
							    dev->bulk_in_endpoint);
			have_bulk_in = true;
		} else if (usb_endpoint_is_bulk_out(endpoint)) {
			dev->bulk_out_endpoint = usb_endpoint_num(endpoint);
			dev->bulk_out_pipe = usb_sndbulkpipe(udev,
							     dev->bulk_out_endpoint);
			have_bulk_out = true;
		}
	}

	if (!have_bulk_in || !have_bulk_out) {
		ret = -ENODEV;
		goto err_put;
	}

	dev->read_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->read_urb) {
		ret = -ENOMEM;
		goto err_put;
	}

	dev->read_buf = usb_alloc_coherent(udev, dev->read_size, GFP_KERNEL,
					   &dev->read_dma);
	if (!dev->read_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	dev->write_buf = kmalloc(dev->write_size, GFP_KERNEL);
	if (!dev->write_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	sgi_l1_fill_cfg(dev);
	usb_set_intfdata(interface, dev);

	ret = usb_register_dev(interface, &sgi_l1_class);
	if (ret) {
		dev_err(&interface->dev, "failed to allocate USB minor: %d\n",
			ret);
		usb_set_intfdata(interface, NULL);
		goto err_put;
	}

	dev->minor = interface->minor;
	index = dev->minor - SGI_L1_MINOR_BASE;
	if (index < 0 || index >= SGIL1_MAX_DEVICES) {
		ret = -ENODEV;
		usb_deregister_dev(interface, &sgi_l1_class);
		usb_set_intfdata(interface, NULL);
		goto err_put;
	}
	dev->index = index;

	mutex_lock(&sgi_l1_table_lock);
	sgi_l1_devices[index] = dev;
	mutex_unlock(&sgi_l1_table_lock);

	sgi_l1_status_changed();

	dev_info(&interface->dev,
		 "SGI L1 connected: /dev/sgil1_%u, bus=%u dev=%u in=0x%02x out=0x%02x\n",
		 dev->index, dev->cfg.bus, dev->cfg.dev,
		 dev->bulk_in_endpoint | USB_DIR_IN, dev->bulk_out_endpoint);

	return 0;

err_put:
	kref_put(&dev->kref, sgi_l1_delete);
	return ret;
}

static void sgi_l1_disconnect(struct usb_interface *interface)
{
	struct sgi_l1_device *dev = usb_get_intfdata(interface);
	unsigned long flags;

	usb_set_intfdata(interface, NULL);
	if (!dev)
		return;

	usb_deregister_dev(interface, &sgi_l1_class);

	mutex_lock(&sgi_l1_table_lock);
	if (dev->index < SGIL1_MAX_DEVICES && sgi_l1_devices[dev->index] == dev)
		sgi_l1_devices[dev->index] = NULL;
	mutex_unlock(&sgi_l1_table_lock);

	mutex_lock(&dev->io_mutex);
	spin_lock_irqsave(&dev->state_lock, flags);
	dev->present = false;
	dev->read_pending = false;
	dev->read_status = -ENODEV;
	dev->read_count = 0;
	dev->read_ready = true;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	usb_kill_urb(dev->read_urb);
	mutex_unlock(&dev->io_mutex);

	wake_up_interruptible(&dev->read_wait);
	sgi_l1_status_changed();

	dev_info(&interface->dev, "SGI L1 disconnected: /dev/sgil1_%u\n",
		 dev->index);

	kref_put(&dev->kref, sgi_l1_delete);
}

static struct usb_driver sgi_l1_driver = {
	.name = "sgi_l1_usb",
	.probe = sgi_l1_probe,
	.disconnect = sgi_l1_disconnect,
	.id_table = sgi_l1_id_table,
};

static int __init sgi_l1_init(void)
{
	int ret;

	ret = misc_register(&sgi_l1_status_miscdev);
	if (ret) {
		pr_err("sgi_l1_usb: failed to register sgil1_cs: %d\n", ret);
		return ret;
	}

	ret = usb_register(&sgi_l1_driver);
	if (ret) {
		misc_deregister(&sgi_l1_status_miscdev);
		pr_err("sgi_l1_usb: failed to register USB driver: %d\n", ret);
		return ret;
	}

	pr_info("%s loaded\n", SGI_L1_DRIVER_VERSION);
	return 0;
}

static void __exit sgi_l1_exit(void)
{
	usb_deregister(&sgi_l1_driver);
	misc_deregister(&sgi_l1_status_miscdev);
	pr_info("sgi_l1_usb unloaded\n");
}

module_init(sgi_l1_init);
module_exit(sgi_l1_exit);

MODULE_AUTHOR("OpenAI Codex; based on SGI sgil1 by Steve Hein and Bob Cutler");
MODULE_DESCRIPTION("USB transport driver for SGI L1 system controllers");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(SGI_L1_STATUS_MINOR);
