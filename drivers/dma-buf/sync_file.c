// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/dma-buf/sync_file.c
 *
 * Copyright (C) 2012 Google, Inc.
 */

#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>
#include <linux/sync_file.h>
#include <uapi/linux/sync_file.h>

static const struct file_operations sync_file_fops;

static struct sync_file *sync_file_alloc(void)
{
	struct sync_file *sync_file;

	sync_file = kzalloc(sizeof(*sync_file), GFP_KERNEL);
	if (!sync_file)
		return NULL;

	sync_file->file = anon_inode_getfile("sync_file", &sync_file_fops,
					     sync_file, 0);
	if (IS_ERR(sync_file->file))
		goto err;

	init_waitqueue_head(&sync_file->wq);

	INIT_LIST_HEAD(&sync_file->cb.node);

	return sync_file;

err:
	kfree(sync_file);
	return NULL;
}

static void fence_check_cb_func(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct sync_file *sync_file;

	sync_file = container_of(cb, struct sync_file, cb);

	wake_up_all(&sync_file->wq);
}

/**
 * sync_file_create() - creates a sync file
 * @fence:	fence to add to the sync_fence
 *
 * Creates a sync_file containg @fence. This function acquires and additional
 * reference of @fence for the newly-created &sync_file, if it succeeds. The
 * sync_file can be released with fput(sync_file->file). Returns the
 * sync_file or NULL in case of error.
 */
struct sync_file *sync_file_create(struct dma_fence *fence)
{
	struct sync_file *sync_file;

	sync_file = sync_file_alloc();
	if (!sync_file)
		return NULL;

	sync_file->fence = dma_fence_get(fence);

	return sync_file;
}
EXPORT_SYMBOL(sync_file_create);

static struct sync_file *sync_file_fdget(int fd)
{
	struct file *file = fget(fd);

	if (!file)
		return NULL;

	if (file->f_op != &sync_file_fops)
		goto err;

	return file->private_data;

err:
	fput(file);
	return NULL;
}

/**
 * sync_file_get_fence - get the fence related to the sync_file fd
 * @fd:		sync_file fd to get the fence from
 *
 * Ensures @fd references a valid sync_file and returns a fence that
 * represents all fence in the sync_file. On error NULL is returned.
 */
struct dma_fence *sync_file_get_fence(int fd)
{
	struct sync_file *sync_file;
	struct dma_fence *fence;

	sync_file = sync_file_fdget(fd);
	if (!sync_file)
		return NULL;

	fence = dma_fence_get(sync_file->fence);
	fput(sync_file->file);

	return fence;
}
EXPORT_SYMBOL(sync_file_get_fence);

/**
 * sync_file_set_name - assign a userspace-visible name to a sync_file
 * @fd: sync_file file descriptor
 * @name: name to assign
 *
 * NVIDIA R32 userspace sets fence names through an nvhost control ioctl.
 * The legacy Android sync framework stored the name directly in the fence,
 * while the dma-fence based sync_file framework stores it in user_name.
 */
int sync_file_set_name(int fd, const char *name)
{
	struct sync_file *sync_file;

	sync_file = sync_file_fdget(fd);
	if (!sync_file)
		return -EINVAL;

	strlcpy(sync_file->user_name, name, sizeof(sync_file->user_name));
	fput(sync_file->file);

	return 0;
}
EXPORT_SYMBOL(sync_file_set_name);

/**
 * sync_file_get_name - get the name of the sync_file
 * @sync_file:		sync_file to get the fence from
 * @buf:		destination buffer to copy sync_file name into
 * @len:		available size of destination buffer.
 *
 * Each sync_file may have a name assigned either by the user (when merging
 * sync_files together) or created from the fence it contains. In the latter
 * case construction of the name is deferred until use, and so requires
 * sync_file_get_name().
 *
 * Returns: a string representing the name.
 */
char *sync_file_get_name(struct sync_file *sync_file, char *buf, int len)
{
	if (sync_file->user_name[0]) {
		strlcpy(buf, sync_file->user_name, len);
	} else {
		struct dma_fence *fence = sync_file->fence;

		snprintf(buf, len, "%s-%s%llu-%lld",
			 fence->ops->get_driver_name(fence),
			 fence->ops->get_timeline_name(fence),
			 fence->context,
			 fence->seqno);
	}

	return buf;
}

static int sync_file_set_fence(struct sync_file *sync_file,
			       struct dma_fence **fences, int num_fences)
{
	struct dma_fence_array *array;

	/*
	 * The reference for the fences in the new sync_file and held
	 * in add_fence() during the merge procedure, so for num_fences == 1
	 * we already own a new reference to the fence. For num_fence > 1
	 * we own the reference of the dma_fence_array creation.
	 */
	if (num_fences == 1) {
		sync_file->fence = fences[0];
		kfree(fences);
	} else {
		array = dma_fence_array_create(num_fences, fences,
					       dma_fence_context_alloc(1),
					       1, false);
		if (!array)
			return -ENOMEM;

		sync_file->fence = &array->base;
	}

	return 0;
}

static struct dma_fence **get_fences(struct sync_file *sync_file,
				     int *num_fences)
{
	if (dma_fence_is_array(sync_file->fence)) {
		struct dma_fence_array *array = to_dma_fence_array(sync_file->fence);

		*num_fences = array->num_fences;
		return array->fences;
	}

	*num_fences = 1;
	return &sync_file->fence;
}

static void add_fence(struct dma_fence **fences,
		      int *i, struct dma_fence *fence)
{
	fences[*i] = fence;

	if (!dma_fence_is_signaled(fence)) {
		dma_fence_get(fence);
		(*i)++;
	}
}

/**
 * sync_file_merge() - merge two sync_files
 * @name:	name of new fence
 * @a:		sync_file a
 * @b:		sync_file b
 *
 * Creates a new sync_file which contains copies of all the fences in both
 * @a and @b.  @a and @b remain valid, independent sync_file. Returns the
 * new merged sync_file or NULL in case of error.
 */
static struct sync_file *sync_file_merge(const char *name, struct sync_file *a,
					 struct sync_file *b)
{
	struct sync_file *sync_file;
	struct dma_fence **fences = NULL, **nfences, **a_fences, **b_fences;
	int i = 0, i_a, i_b, num_fences, a_num_fences, b_num_fences;

	sync_file = sync_file_alloc();
	if (!sync_file)
		return NULL;

	a_fences = get_fences(a, &a_num_fences);
	b_fences = get_fences(b, &b_num_fences);
	if (a_num_fences > INT_MAX - b_num_fences)
		goto err;

	num_fences = a_num_fences + b_num_fences;

	fences = kcalloc(num_fences, sizeof(*fences), GFP_KERNEL);
	if (!fences)
		goto err;

	/*
	 * Assume sync_file a and b are both ordered and have no
	 * duplicates with the same context.
	 *
	 * If a sync_file can only be created with sync_file_merge
	 * and sync_file_create, this is a reasonable assumption.
	 */
	for (i_a = i_b = 0; i_a < a_num_fences && i_b < b_num_fences; ) {
		struct dma_fence *pt_a = a_fences[i_a];
		struct dma_fence *pt_b = b_fences[i_b];

		if (pt_a->context < pt_b->context) {
			add_fence(fences, &i, pt_a);

			i_a++;
		} else if (pt_a->context > pt_b->context) {
			add_fence(fences, &i, pt_b);

			i_b++;
		} else {
			if (__dma_fence_is_later(pt_a->seqno, pt_b->seqno,
						 pt_a->ops))
				add_fence(fences, &i, pt_a);
			else
				add_fence(fences, &i, pt_b);

			i_a++;
			i_b++;
		}
	}

	for (; i_a < a_num_fences; i_a++)
		add_fence(fences, &i, a_fences[i_a]);

	for (; i_b < b_num_fences; i_b++)
		add_fence(fences, &i, b_fences[i_b]);

	if (i == 0)
		fences[i++] = dma_fence_get(a_fences[0]);

	if (num_fences > i) {
		nfences = krealloc(fences, i * sizeof(*fences),
				  GFP_KERNEL);
		if (!nfences)
			goto err;

		fences = nfences;
	}

	if (sync_file_set_fence(sync_file, fences, i) < 0)
		goto err;

	strlcpy(sync_file->user_name, name, sizeof(sync_file->user_name));
	return sync_file;

err:
	while (i)
		dma_fence_put(fences[--i]);
	kfree(fences);
	fput(sync_file->file);
	return NULL;

}

static int sync_file_release(struct inode *inode, struct file *file)
{
	struct sync_file *sync_file = file->private_data;

	if (test_bit(POLL_ENABLED, &sync_file->flags))
		dma_fence_remove_callback(sync_file->fence, &sync_file->cb);
	dma_fence_put(sync_file->fence);
	kfree(sync_file);

	return 0;
}

static __poll_t sync_file_poll(struct file *file, poll_table *wait)
{
	struct sync_file *sync_file = file->private_data;

	poll_wait(file, &sync_file->wq, wait);

	if (list_empty(&sync_file->cb.node) &&
	    !test_and_set_bit(POLL_ENABLED, &sync_file->flags)) {
		if (dma_fence_add_callback(sync_file->fence, &sync_file->cb,
					   fence_check_cb_func) < 0)
			wake_up_all(&sync_file->wq);
	}

	return dma_fence_is_signaled(sync_file->fence) ? EPOLLIN : 0;
}

/*
 * Android's pre-destaging sync ABI used ioctl opcodes 0, 1 and 2.
 * NVIDIA R32 userspace still issues these commands.  Upstream intentionally
 * burned those opcode numbers when sync_file was destaged, so accepting the
 * old layouts here is unambiguous and does not alter the modern ABI.
 */
struct sync_merge_data_legacy {
	__s32 fd2;
	char name[32];
	__s32 fence;
};

struct sync_pt_info_legacy {
	__u32 len;
	char obj_name[32];
	char driver_name[32];
	__s32 status;
	__u64 timestamp_ns;
	__u8 driver_data[0];
};

struct sync_fence_info_data_legacy {
	__u32 len;
	char name[32];
	__s32 status;
	__u8 pt_info[0];
};

/* NVIDIA R32 nvhost sync points expose this in sync_pt_info.driver_data. */
struct nvhost_sync_fence_info_legacy {
	__u32 id;
	__u32 thresh;
};

#define SYNC_IOC_WAIT_LEGACY \
	_IOW(SYNC_IOC_MAGIC, 0, __s32)
#define SYNC_IOC_MERGE_LEGACY \
	_IOWR(SYNC_IOC_MAGIC, 1, struct sync_merge_data_legacy)
#define SYNC_IOC_FENCE_INFO_LEGACY \
	_IOWR(SYNC_IOC_MAGIC, 2, struct sync_fence_info_data_legacy)

static long sync_file_ioctl_wait_legacy(struct sync_file *sync_file,
					unsigned long arg)
{
	__s32 timeout_ms;
	long timeout, ret;
	int status;

	if (copy_from_user(&timeout_ms, (void __user *)arg, sizeof(timeout_ms)))
		return -EFAULT;

	timeout = timeout_ms < 0 ? MAX_SCHEDULE_TIMEOUT :
		msecs_to_jiffies(timeout_ms);
	ret = dma_fence_wait_timeout(sync_file->fence, true, timeout);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIME;

	status = dma_fence_get_status(sync_file->fence);
	return status < 0 ? status : 0;
}

static long sync_file_ioctl_merge_legacy(struct sync_file *sync_file,
					 unsigned long arg)
{
	struct sync_merge_data_legacy data;
	struct sync_file *fence2, *fence3;
	int fd, err;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
		err = -EFAULT;
		goto err_put_fd;
	}

	fence2 = sync_file_fdget(data.fd2);
	if (!fence2) {
		err = -ENOENT;
		goto err_put_fd;
	}

	data.name[sizeof(data.name) - 1] = '\0';
	fence3 = sync_file_merge(data.name, sync_file, fence2);
	if (!fence3) {
		err = -ENOMEM;
		goto err_put_fence2;
	}

	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		err = -EFAULT;
		goto err_put_fence3;
	}

	fd_install(fd, fence3->file);
	fput(fence2->file);
	return 0;

err_put_fence3:
	fput(fence3->file);
err_put_fence2:
	fput(fence2->file);
err_put_fd:
	put_unused_fd(fd);
	return err;
}

static long sync_file_ioctl_merge(struct sync_file *sync_file,
				  unsigned long arg)
{
	int fd = get_unused_fd_flags(O_CLOEXEC);
	int err;
	struct sync_file *fence2, *fence3;
	struct sync_merge_data data;

	if (fd < 0)
		return fd;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
		err = -EFAULT;
		goto err_put_fd;
	}

	if (data.flags || data.pad) {
		err = -EINVAL;
		goto err_put_fd;
	}

	fence2 = sync_file_fdget(data.fd2);
	if (!fence2) {
		err = -ENOENT;
		goto err_put_fd;
	}

	data.name[sizeof(data.name) - 1] = '\0';
	fence3 = sync_file_merge(data.name, sync_file, fence2);
	if (!fence3) {
		err = -ENOMEM;
		goto err_put_fence2;
	}

	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		err = -EFAULT;
		goto err_put_fence3;
	}

	fd_install(fd, fence3->file);
	fput(fence2->file);
	return 0;

err_put_fence3:
	fput(fence3->file);

err_put_fence2:
	fput(fence2->file);

err_put_fd:
	put_unused_fd(fd);
	return err;
}

static int sync_fill_fence_info(struct dma_fence *fence,
				 struct sync_fence_info *info)
{
	strlcpy(info->obj_name, fence->ops->get_timeline_name(fence),
		sizeof(info->obj_name));
	strlcpy(info->driver_name, fence->ops->get_driver_name(fence),
		sizeof(info->driver_name));

	info->status = dma_fence_get_status(fence);
	while (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags) &&
	       !test_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags))
		cpu_relax();
	info->timestamp_ns =
		test_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags) ?
		ktime_to_ns(fence->timestamp) :
		ktime_set(0, 0);

	return info->status;
}

static long sync_file_ioctl_fence_info_legacy(struct sync_file *sync_file,
					      unsigned long arg)
{
	struct sync_fence_info_data_legacy *data;
	struct dma_fence **fences;
	__u32 size, len;
	int num_fences, i, ret = 0;

	if (copy_from_user(&size, (void __user *)arg, sizeof(size)))
		return -EFAULT;

	if (size < sizeof(*data))
		return -EINVAL;
	if (size > 4096)
		size = 4096;

	data = kzalloc(size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	sync_file_get_name(sync_file, data->name, sizeof(data->name));
	data->status = dma_fence_get_status(sync_file->fence);
	len = sizeof(*data);

	fences = get_fences(sync_file, &num_fences);
	for (i = 0; i < num_fences; i++) {
		struct sync_pt_info_legacy *info;
		struct dma_fence *fence = fences[i];
		const char *driver_name = fence->ops->get_driver_name(fence);
		const char *timeline_name = fence->ops->get_timeline_name(fence);
		struct nvhost_sync_fence_info_legacy nvhost_info;
		bool nvhost_fence = false;
		u32 info_len = sizeof(*info);

		/*
		 * R32's nvhost Android-sync implementation appended
		 * { syncpt_id, threshold } after struct sync_pt_info.
		 * The dma-fence based replacement retains both values: the
		 * timeline is named "sp<ID>" and seqno is the threshold.
		 */
		if (!strcmp(driver_name, "nvhost") &&
		    sscanf(timeline_name, "sp%u", &nvhost_info.id) == 1) {
			nvhost_info.thresh = (__u32)fence->seqno;
			info_len += sizeof(nvhost_info);
			nvhost_fence = true;
		}

		if (size - len < info_len) {
			ret = -ENOMEM;
			goto out;
		}

		info = (void *)data + len;
		info->len = info_len;
		strlcpy(info->obj_name, timeline_name, sizeof(info->obj_name));
		strlcpy(info->driver_name,
			nvhost_fence ? "nvhost_sync" : driver_name,
			sizeof(info->driver_name));
		info->status = dma_fence_get_status(fence);

		while (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags) &&
		       !test_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags))
			cpu_relax();
		info->timestamp_ns =
			test_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags) ?
			ktime_to_ns(fence->timestamp) : 0;

		if (info_len > sizeof(*info))
			memcpy(info->driver_data, &nvhost_info, sizeof(nvhost_info));

		len += info->len;
	}

	data->len = len;
	if (copy_to_user((void __user *)arg, data, len))
		ret = -EFAULT;

out:
	kfree(data);
	return ret;
}

static long sync_file_ioctl_fence_info(struct sync_file *sync_file,
				       unsigned long arg)
{
	struct sync_file_info info;
	struct sync_fence_info *fence_info = NULL;
	struct dma_fence **fences;
	__u32 size;
	int num_fences, ret, i;

	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;

	if (info.flags || info.pad)
		return -EINVAL;

	fences = get_fences(sync_file, &num_fences);

	/*
	 * Passing num_fences = 0 means that userspace doesn't want to
	 * retrieve any sync_fence_info. If num_fences = 0 we skip filling
	 * sync_fence_info and return the actual number of fences on
	 * info->num_fences.
	 */
	if (!info.num_fences) {
		info.status = dma_fence_get_status(sync_file->fence);
		goto no_fences;
	} else {
		info.status = 1;
	}

	if (info.num_fences < num_fences)
		return -EINVAL;

	size = num_fences * sizeof(*fence_info);
	fence_info = kzalloc(size, GFP_KERNEL);
	if (!fence_info)
		return -ENOMEM;

	for (i = 0; i < num_fences; i++) {
		int status = sync_fill_fence_info(fences[i], &fence_info[i]);
		info.status = info.status <= 0 ? info.status : status;
	}

	if (copy_to_user(u64_to_user_ptr(info.sync_fence_info), fence_info,
			 size)) {
		ret = -EFAULT;
		goto out;
	}

no_fences:
	sync_file_get_name(sync_file, info.name, sizeof(info.name));
	info.num_fences = num_fences;

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		ret = -EFAULT;
	else
		ret = 0;

out:
	kfree(fence_info);

	return ret;
}

static long sync_file_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct sync_file *sync_file = file->private_data;

	switch (cmd) {
	case SYNC_IOC_WAIT_LEGACY:
		return sync_file_ioctl_wait_legacy(sync_file, arg);

	case SYNC_IOC_MERGE_LEGACY:
		return sync_file_ioctl_merge_legacy(sync_file, arg);

	case SYNC_IOC_FENCE_INFO_LEGACY:
		return sync_file_ioctl_fence_info_legacy(sync_file, arg);

	case SYNC_IOC_MERGE:
		return sync_file_ioctl_merge(sync_file, arg);

	case SYNC_IOC_FILE_INFO:
		return sync_file_ioctl_fence_info(sync_file, arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations sync_file_fops = {
	.release = sync_file_release,
	.poll = sync_file_poll,
	.unlocked_ioctl = sync_file_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};
