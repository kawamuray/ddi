/*
 * Copyright (C) 2005-2007 Red Hat GmbH
 *
 * A target that delays reads and/or writes and can send
 * them to different devices.
 *
 * This file is released under the GPL.
 *
 *
 * ddi - Disk Delay Injection
 * A DM driver which injects delay in block device I/O, allowing dynamic parameter control.
 * Based on dm-delay from the kernel upstream.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>

#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "ddi"

struct delay_c {
	struct timer_list delay_timer;
	struct mutex timer_lock;
	struct workqueue_struct *kdelayd_wq;
	struct work_struct flush_expired_bios;
	struct list_head delayed_bios;
	atomic_t may_delay;

	struct dm_dev *dev_read;
	sector_t start_read;
	unsigned read_delay;
	unsigned reads;

	struct dm_dev *dev_write;
	sector_t start_write;
	unsigned write_delay;
	unsigned writes;

	struct kobject *kobj;
	struct kobj_attribute read_delay_attr;
	struct kobj_attribute write_delay_attr;
};

struct dm_delay_info {
	struct delay_c *context;
	struct list_head list;
	unsigned long expires;
};

static DEFINE_MUTEX(delayed_bios_lock);

/* Sysfs implementation for dynamic parameter control.*/
static struct kobject *ddi_kobj;

static ssize_t show_delay(unsigned delay, char *buf)
{
	/* The buffer allocation size is PAGE_SIZE(=4k typically) so it is safe to print an int
	 * without limiting length.
	 * ref: https://www.kernel.org/doc/Documentation/filesystems/sysfs.txt
	 */
	return sprintf(buf, "%u\n", delay);
}

static void queue_timeout(struct delay_c *dc, unsigned long expires);

static ssize_t store_delay(struct delay_c *dc, unsigned *delay, const char *buf, size_t count)
{
	unsigned new_delay;
	unsigned long expires;

	/* buf is null-terminated. */
	sscanf(buf, "%d", &new_delay);
	if (new_delay < 0) {
		printk(KERN_WARNING "Not setting an invalid delay: %d\n", new_delay);
		return count;
	}

	printk(KERN_DEBUG "Updating delay %u => %u\n", *delay, new_delay);
	*delay = new_delay;
	smp_wmb();

	/* Update timer to cancel possibly existing too long timeout. */
	expires = jiffies + msecs_to_jiffies(new_delay);
	queue_timeout(dc, expires);

	return count;
}

static ssize_t read_delay_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct delay_c *dc = container_of(attr, struct delay_c, read_delay_attr);
	return show_delay(dc->read_delay, buf);
}

static ssize_t read_delay_store(struct kobject *kobj, struct kobj_attribute *attr,
								const char *buf, size_t count)
{
	struct delay_c *dc = container_of(attr, struct delay_c, read_delay_attr);
	return store_delay(dc, &dc->read_delay, buf, count);
}

static ssize_t write_delay_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct delay_c *dc = container_of(attr, struct delay_c, write_delay_attr);
	return show_delay(dc->write_delay, buf);
}

static ssize_t write_delay_store(struct kobject *kobj, struct kobj_attribute *attr,
								 const char *buf, size_t count)
{
	struct delay_c *dc = container_of(attr, struct delay_c, write_delay_attr);
	if (!dc->dev_write) {
		printk(KERN_WARNING "Write device is not configured\n");
		return count;
	}
	return store_delay(dc, &dc->write_delay, buf, count);
}

static int init_dev_kobject(struct delay_c *dc)
{
	int ret = 0;
	static struct attribute *attrs[3];
	static struct attribute_group attr_group = { .attrs = attrs };
	attrs[0] = &dc->read_delay_attr.attr;
	attrs[1] = &dc->write_delay_attr.attr;
	attrs[2] = NULL;

	dc->kobj = kobject_create_and_add(dc->dev_read->name, ddi_kobj);
	if (!dc->kobj)
		return -ENOMEM;

	dc->read_delay_attr = (struct kobj_attribute)__ATTR(read_delay, 0666, read_delay_show, read_delay_store);
	dc->write_delay_attr = (struct kobj_attribute)__ATTR(write_delay, 0666, write_delay_show, write_delay_store);

	ret = sysfs_create_group(dc->kobj, &attr_group);
	if (ret)
		kobject_put(dc->kobj);

	return ret;
}

static void destroy_dev_kobject(struct delay_c *dc)
{
	kobject_put(dc->kobj);
}

/* Device Mapper implementation. */

static void handle_delayed_timer(unsigned long data)
{
	struct delay_c *dc = (struct delay_c *)data;

	queue_work(dc->kdelayd_wq, &dc->flush_expired_bios);
}

static void queue_timeout(struct delay_c *dc, unsigned long expires)
{
	mutex_lock(&dc->timer_lock);

	if (!timer_pending(&dc->delay_timer) || expires < dc->delay_timer.expires)
		mod_timer(&dc->delay_timer, expires);

	mutex_unlock(&dc->timer_lock);
}

static void flush_bios(struct bio *bio)
{
	struct bio *n;

	while (bio) {
		n = bio->bi_next;
		bio->bi_next = NULL;
		generic_make_request(bio);
		bio = n;
	}
}

static struct bio *flush_delayed_bios(struct delay_c *dc, int flush_all)
{
	struct dm_delay_info *delayed, *next;
	unsigned long next_expires = 0;
	int start_timer = 0;
	struct bio_list flush_bios = { };

	mutex_lock(&delayed_bios_lock);
	list_for_each_entry_safe(delayed, next, &dc->delayed_bios, list) {
		if (flush_all || time_after_eq(jiffies, delayed->expires)) {
			struct bio *bio = dm_bio_from_per_bio_data(delayed,
						sizeof(struct dm_delay_info));
			list_del(&delayed->list);
			bio_list_add(&flush_bios, bio);
			if ((bio_data_dir(bio) == WRITE))
				delayed->context->writes--;
			else
				delayed->context->reads--;
			continue;
		}

		if (!start_timer) {
			start_timer = 1;
			next_expires = delayed->expires;
		} else
			next_expires = min(next_expires, delayed->expires);
	}

	mutex_unlock(&delayed_bios_lock);

	if (start_timer)
		queue_timeout(dc, next_expires);

	return bio_list_get(&flush_bios);
}

static void flush_expired_bios(struct work_struct *work)
{
	struct delay_c *dc;

	dc = container_of(work, struct delay_c, flush_expired_bios);
	flush_bios(flush_delayed_bios(dc, 0));
}

/*
 * Mapping parameters:
 *    <device> <offset> <delay> [<write_device> <write_offset> <write_delay>]
 *
 * With separate write parameters, the first set is only used for reads.
 * Offsets are specified in sectors.
 * Delays are specified in milliseconds.
 */
static int delay_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct delay_c *dc;
	unsigned long long tmpll;
	char dummy;
	int ret;

	if (argc != 3 && argc != 6) {
		ti->error = "Requires exactly 3 or 6 arguments";
		return -EINVAL;
	}

	dc = kmalloc(sizeof(*dc), GFP_KERNEL);
	if (!dc) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	dc->reads = dc->writes = 0;

	ret = -EINVAL;
	if (sscanf(argv[1], "%llu%c", &tmpll, &dummy) != 1) {
		ti->error = "Invalid device sector";
		goto bad;
	}
	dc->start_read = tmpll;

	if (sscanf(argv[2], "%u%c", &dc->read_delay, &dummy) != 1) {
		ti->error = "Invalid delay";
		goto bad;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &dc->dev_read);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	ret = -EINVAL;
	dc->dev_write = NULL;
	if (argc == 3)
		goto out;

	if (sscanf(argv[4], "%llu%c", &tmpll, &dummy) != 1) {
		ti->error = "Invalid write device sector";
		goto bad_dev_read;
	}
	dc->start_write = tmpll;

	if (sscanf(argv[5], "%u%c", &dc->write_delay, &dummy) != 1) {
		ti->error = "Invalid write delay";
		goto bad_dev_read;
	}

	ret = dm_get_device(ti, argv[3], dm_table_get_mode(ti->table),
			    &dc->dev_write);
	if (ret) {
		ti->error = "Write device lookup failed";
		goto bad_dev_read;
	}

out:
	ret = -EINVAL;
	dc->kdelayd_wq = alloc_workqueue("kddid", WQ_MEM_RECLAIM, 0);
	if (!dc->kdelayd_wq) {
		DMERR("Couldn't start kdelayd");
		goto bad_queue;
	}

	setup_timer(&dc->delay_timer, handle_delayed_timer, (unsigned long)dc);

	INIT_WORK(&dc->flush_expired_bios, flush_expired_bios);
	INIT_LIST_HEAD(&dc->delayed_bios);
	mutex_init(&dc->timer_lock);
	atomic_set(&dc->may_delay, 1);

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->per_io_data_size = sizeof(struct dm_delay_info);
	ti->private = dc;

	ret = init_dev_kobject(dc);
	if (ret) {
		DMERR("Failed to setup sysfs");
		goto bad_sysfs;
	}

	return 0;

bad_sysfs:
	destroy_workqueue(dc->kdelayd_wq);
bad_queue:
	if (dc->dev_write)
		dm_put_device(ti, dc->dev_write);
bad_dev_read:
	dm_put_device(ti, dc->dev_read);
bad:
	kfree(dc);
	return ret;
}

static void delay_dtr(struct dm_target *ti)
{
	struct delay_c *dc = ti->private;

	destroy_dev_kobject(dc);

	if (dc->kdelayd_wq)
		destroy_workqueue(dc->kdelayd_wq);

	dm_put_device(ti, dc->dev_read);

	if (dc->dev_write)
		dm_put_device(ti, dc->dev_write);

	kfree(dc);
}

static int delay_bio(struct delay_c *dc, int delay, struct bio *bio)
{
	struct dm_delay_info *delayed;
	unsigned long expires = 0;

	if (!delay || !atomic_read(&dc->may_delay))
		return DM_MAPIO_REMAPPED;

	delayed = dm_per_bio_data(bio, sizeof(struct dm_delay_info));

	delayed->context = dc;
	delayed->expires = expires = jiffies + msecs_to_jiffies(delay);

	mutex_lock(&delayed_bios_lock);

	if (bio_data_dir(bio) == WRITE)
		dc->writes++;
	else
		dc->reads++;

	list_add_tail(&delayed->list, &dc->delayed_bios);

	mutex_unlock(&delayed_bios_lock);

	queue_timeout(dc, expires);

	return DM_MAPIO_SUBMITTED;
}

static void delay_presuspend(struct dm_target *ti)
{
	struct delay_c *dc = ti->private;

	atomic_set(&dc->may_delay, 0);
	del_timer_sync(&dc->delay_timer);
	flush_bios(flush_delayed_bios(dc, 1));
}

static void delay_resume(struct dm_target *ti)
{
	struct delay_c *dc = ti->private;

	atomic_set(&dc->may_delay, 1);
}

static int delay_map(struct dm_target *ti, struct bio *bio)
{
	struct delay_c *dc = ti->private;

	if ((bio_data_dir(bio) == WRITE) && (dc->dev_write)) {
		bio->bi_bdev = dc->dev_write->bdev;
		if (bio_sectors(bio))
			bio->bi_sector = dc->start_write +
					 dm_target_offset(ti, bio->bi_sector);

		return delay_bio(dc, dc->write_delay, bio);
	}

	bio->bi_bdev = dc->dev_read->bdev;
	bio->bi_sector = dc->start_read + dm_target_offset(ti, bio->bi_sector);

	return delay_bio(dc, dc->read_delay, bio);
}

static void delay_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	struct delay_c *dc = ti->private;
	int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%u %u", dc->reads, dc->writes);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s %llu %u", dc->dev_read->name,
		       (unsigned long long) dc->start_read,
		       dc->read_delay);
		if (dc->dev_write)
			DMEMIT(" %s %llu %u", dc->dev_write->name,
			       (unsigned long long) dc->start_write,
			       dc->write_delay);
		break;
	}
}

static int delay_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct delay_c *dc = ti->private;
	int ret = 0;

	ret = fn(ti, dc->dev_read, dc->start_read, ti->len, data);
	if (ret)
		goto out;

	if (dc->dev_write)
		ret = fn(ti, dc->dev_write, dc->start_write, ti->len, data);

out:
	return ret;
}

static struct target_type delay_target = {
	.name	     = "ddi",
	.version     = {1, 2, 1},
	.module      = THIS_MODULE,
	.ctr	     = delay_ctr,
	.dtr	     = delay_dtr,
	.map	     = delay_map,
	.presuspend  = delay_presuspend,
	.resume	     = delay_resume,
	.status	     = delay_status,
	.iterate_devices = delay_iterate_devices,
};

static int __init dm_delay_init(void)
{
	int r;

	r = dm_register_target(&delay_target);
	if (r < 0) {
		DMERR("register failed %d", r);
		goto bad_register;
	}

	ddi_kobj = kobject_create_and_add("ddi", fs_kobj);
	if (!ddi_kobj)
		return -ENOMEM;

	return 0;

bad_register:
	return r;
}

static void __exit dm_delay_exit(void)
{
	kobject_put(ddi_kobj);
	dm_unregister_target(&delay_target);
}

/* Module hooks */
module_init(dm_delay_init);
module_exit(dm_delay_exit);

MODULE_DESCRIPTION(DM_NAME " delay target");
MODULE_AUTHOR("Heinz Mauelshagen <mauelshagen@redhat.com>");
MODULE_LICENSE("GPL");
