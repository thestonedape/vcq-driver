// SPDX-License-Identifier: GPL-2.0
/*
 * vcq: a virtual command-queue device driver.
 *
 * The device models how GPU and accelerator drivers accept work: user space
 * submits jobs through an ioctl, an engine executes them asynchronously, and
 * completion is signalled through per-context fences that can be waited on or
 * polled. A single ordered workqueue plays the engine and runs jobs from all
 * files in FIFO order. The device-wide queue is bounded by queue_depth;
 * submitters block, or get -EAGAIN with O_NONBLOCK, while it is full.
 *
 * Each open file is a context with its own fence timeline. Its jobs complete
 * in fence order because the context mutex serialises submission, so a job's
 * position in the engine FIFO matches its fence.
 *
 * Locking:
 *   vcq_dev.lock  (spinlock)  pending list and the queued slot count
 *   vcq_ctx.lock  (mutex)     a context's job table, done list and fences
 * ctx->lock may be held while taking vcq_dev.lock, never the reverse.
 *
 * Lifetime: the open file holds one reference on its context and every queued
 * job holds another, so a file can be closed with jobs in flight. The context
 * and its unretrieved jobs are freed when the last reference is dropped.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "vcq.h"

/* Fences index an xarray, whose index type is unsigned long. */
#if BITS_PER_LONG != 64
#error "vcq requires a 64-bit kernel"
#endif

#define VCQ_MAX_JOBS_PER_FILE	256
#define VCQ_MAX_QUEUE_DEPTH	4096
#define VCQ_READ_BATCH		64

static unsigned int queue_depth = 64;
module_param(queue_depth, uint, 0444);
MODULE_PARM_DESC(queue_depth, "Device-wide limit on waiting and running jobs (1-4096)");

static unsigned int engine_delay_us;
module_param(engine_delay_us, uint, 0644);
MODULE_PARM_DESC(engine_delay_us, "Simulated execution time per job in microseconds");

struct vcq_ctx {
	struct kref ref;		/* the open file plus one per queued job */
	struct mutex lock;		/* protects every field below */
	struct xarray jobs;		/* fence -> job until VCQ_IOC_WAIT retrieves it */
	struct list_head done;		/* completed jobs not yet reported by read() */
	unsigned int ndone;		/* entries on @done */
	unsigned int njobs;		/* entries in @jobs */
	u64 last_fence;			/* last fence handed out */
	u64 done_fence;			/* last fence completed */
	wait_queue_head_t wq;		/* woken when one of this context's jobs completes */
};

struct vcq_job {
	struct list_head queue_node;	/* on vcq_dev.pending until the engine takes it */
	struct list_head done_node;	/* on ctx->done until reported by read() */
	struct vcq_ctx *ctx;
	u64 fence;
	u32 op;
	u32 len;
	u64 arg;
	u64 result;
	u8 *buf;			/* input copied in at submit, output produced in place */
};

static struct {
	spinlock_t lock;		/* protects @pending and @queued */
	struct list_head pending;	/* jobs waiting for the engine, FIFO */
	unsigned int queued;		/* reserved slots: waiting or running jobs */
	wait_queue_head_t space_wq;	/* woken when a slot frees */
	struct workqueue_struct *engine;
	struct work_struct work;
} vcq_dev;

static bool vcq_op_has_input(u32 op)
{
	return op == VCQ_OP_XOR || op == VCQ_OP_CRC32;
}

static bool vcq_op_has_output(u32 op)
{
	return op == VCQ_OP_FILL || op == VCQ_OP_XOR;
}

static void vcq_job_free(struct vcq_job *job)
{
	kvfree(job->buf);
	kfree(job);
}

static void vcq_ctx_free(struct kref *ref)
{
	struct vcq_ctx *ctx = container_of(ref, struct vcq_ctx, ref);
	struct vcq_job *job;
	unsigned long fence;

	xa_for_each(&ctx->jobs, fence, job)
		vcq_job_free(job);
	xa_destroy(&ctx->jobs);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
}

static void vcq_release_slot(void)
{
	spin_lock(&vcq_dev.lock);
	WRITE_ONCE(vcq_dev.queued, vcq_dev.queued - 1);
	spin_unlock(&vcq_dev.lock);
	wake_up_interruptible(&vcq_dev.space_wq);
}

/* The simulated hardware. Runs in the engine workqueue. */
static void vcq_execute(struct vcq_job *job)
{
	unsigned int delay = READ_ONCE(engine_delay_us);
	u32 i;

	if (delay)
		fsleep(delay);

	switch (job->op) {
	case VCQ_OP_FILL:
		memset(job->buf, (u8)job->arg, job->len);
		job->result = job->len;
		break;
	case VCQ_OP_XOR:
		for (i = 0; i < job->len; i++)
			job->buf[i] ^= (u8)job->arg;
		job->result = job->len;
		break;
	case VCQ_OP_CRC32:
		job->result = crc32_le(~0U, job->buf, job->len) ^ ~0U;
		break;
	default:
		job->result = 0;
	}
}

static void vcq_complete(struct vcq_job *job)
{
	struct vcq_ctx *ctx = job->ctx;

	mutex_lock(&ctx->lock);
	list_add_tail(&job->done_node, &ctx->done);
	WRITE_ONCE(ctx->ndone, ctx->ndone + 1);
	WRITE_ONCE(ctx->done_fence, job->fence);
	mutex_unlock(&ctx->lock);
	/* A waiter may now retrieve and free @job. The job's reference keeps @ctx. */
	wake_up_interruptible_all(&ctx->wq);
	vcq_release_slot();
	kref_put(&ctx->ref, vcq_ctx_free);
}

static void vcq_engine_work(struct work_struct *work)
{
	struct vcq_job *job;

	for (;;) {
		spin_lock(&vcq_dev.lock);
		job = list_first_entry_or_null(&vcq_dev.pending, struct vcq_job, queue_node);
		if (job)
			list_del(&job->queue_node);
		spin_unlock(&vcq_dev.lock);
		if (!job)
			return;

		vcq_execute(job);
		vcq_complete(job);
		cond_resched();
	}
}

static int vcq_reserve_slot(bool nonblock)
{
	for (;;) {
		spin_lock(&vcq_dev.lock);
		if (vcq_dev.queued < queue_depth) {
			WRITE_ONCE(vcq_dev.queued, vcq_dev.queued + 1);
			spin_unlock(&vcq_dev.lock);
			return 0;
		}
		spin_unlock(&vcq_dev.lock);

		if (nonblock)
			return -EAGAIN;
		if (wait_event_interruptible(vcq_dev.space_wq,
					     READ_ONCE(vcq_dev.queued) < queue_depth))
			return -ERESTARTSYS;
	}
}

static int vcq_validate(const struct vcq_submit *s)
{
	switch (s->op) {
	case VCQ_OP_NOP:
		return s->len ? -EINVAL : 0;
	case VCQ_OP_FILL:
	case VCQ_OP_XOR:
	case VCQ_OP_CRC32:
		if (!s->len)
			return -EINVAL;
		if (s->len > VCQ_MAX_PAYLOAD)
			return -EMSGSIZE;
		return 0;
	default:
		return -EINVAL;
	}
}

static long vcq_submit(struct file *file, struct vcq_submit __user *uarg)
{
	struct vcq_ctx *ctx = file->private_data;
	struct vcq_submit s;
	struct vcq_job *job;
	u64 fence;
	int ret;

	if (copy_from_user(&s, uarg, sizeof(s)))
		return -EFAULT;
	ret = vcq_validate(&s);
	if (ret)
		return ret;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	INIT_LIST_HEAD(&job->done_node);
	job->op = s.op;
	job->len = s.len;
	job->arg = s.arg;
	if (s.len) {
		job->buf = kvmalloc(s.len, GFP_KERNEL);
		if (!job->buf) {
			ret = -ENOMEM;
			goto err_free;
		}
		if (vcq_op_has_input(s.op) &&
		    copy_from_user(job->buf, u64_to_user_ptr(s.src), s.len)) {
			ret = -EFAULT;
			goto err_free;
		}
	}

	/* May sleep until the engine frees a slot, so take it before ctx->lock. */
	ret = vcq_reserve_slot(file->f_flags & O_NONBLOCK);
	if (ret)
		goto err_free;

	mutex_lock(&ctx->lock);
	if (ctx->njobs >= VCQ_MAX_JOBS_PER_FILE) {
		ret = -EBUSY;
		goto err_unlock;
	}
	fence = ctx->last_fence + 1;
	ret = xa_insert(&ctx->jobs, fence, job, GFP_KERNEL);
	if (ret)
		goto err_unlock;
	ctx->last_fence = fence;
	ctx->njobs++;
	job->fence = fence;
	job->ctx = ctx;
	kref_get(&ctx->ref);

	spin_lock(&vcq_dev.lock);
	list_add_tail(&job->queue_node, &vcq_dev.pending);
	spin_unlock(&vcq_dev.lock);
	mutex_unlock(&ctx->lock);

	queue_work(vcq_dev.engine, &vcq_dev.work);
	return put_user(fence, &uarg->fence);

err_unlock:
	mutex_unlock(&ctx->lock);
	vcq_release_slot();
err_free:
	vcq_job_free(job);
	return ret;
}

static bool vcq_fence_done(struct vcq_ctx *ctx, u64 fence)
{
	return READ_ONCE(ctx->done_fence) >= fence;
}

static long vcq_wait(struct file *file, struct vcq_wait __user *uarg)
{
	struct vcq_ctx *ctx = file->private_data;
	struct vcq_job *job;
	struct vcq_wait w;
	long timeout;
	int ret = 0;

	if (copy_from_user(&w, uarg, sizeof(w)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	if (!w.fence || w.fence > ctx->last_fence)
		ret = -EINVAL;
	else if (!xa_load(&ctx->jobs, w.fence))
		ret = -ENOENT;
	mutex_unlock(&ctx->lock);
	if (ret)
		return ret;

	if (!vcq_fence_done(ctx, w.fence)) {
		if (!w.timeout_ms)
			return -ETIMEDOUT;
		timeout = w.timeout_ms == VCQ_WAIT_INFINITE ?
			  MAX_SCHEDULE_TIMEOUT : msecs_to_jiffies(w.timeout_ms);
		timeout = wait_event_interruptible_timeout(ctx->wq,
							   vcq_fence_done(ctx, w.fence),
							   timeout);
		if (timeout < 0)
			return timeout;
		if (!timeout)
			return -ETIMEDOUT;
	}

	mutex_lock(&ctx->lock);
	job = xa_load(&ctx->jobs, w.fence);
	if (!job) {
		ret = -ENOENT;	/* another thread retrieved it meanwhile */
		goto out_unlock;
	}
	/* Copy out before releasing the job, so a fault or short buffer can retry. */
	if (vcq_op_has_output(job->op)) {
		if (w.dst_len < job->len) {
			ret = -ENOSPC;
			goto out_unlock;
		}
		if (copy_to_user(u64_to_user_ptr(w.dst), job->buf, job->len)) {
			ret = -EFAULT;
			goto out_unlock;
		}
	}
	if (put_user(job->result, &uarg->result)) {
		ret = -EFAULT;
		goto out_unlock;
	}
	xa_erase(&ctx->jobs, w.fence);
	ctx->njobs--;
	if (!list_empty(&job->done_node)) {
		list_del_init(&job->done_node);
		WRITE_ONCE(ctx->ndone, ctx->ndone - 1);
	}
	mutex_unlock(&ctx->lock);
	vcq_job_free(job);
	return 0;

out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long vcq_stats(struct file *file, struct vcq_stats __user *uarg)
{
	struct vcq_ctx *ctx = file->private_data;
	struct vcq_stats st = {};

	mutex_lock(&ctx->lock);
	st.submitted = ctx->last_fence;
	st.completed = ctx->done_fence;
	st.jobs = ctx->njobs;
	mutex_unlock(&ctx->lock);
	st.device_queued = READ_ONCE(vcq_dev.queued);
	st.queue_depth = queue_depth;

	return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
}

static long vcq_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case VCQ_IOC_SUBMIT:
		return vcq_submit(file, uarg);
	case VCQ_IOC_WAIT:
		return vcq_wait(file, uarg);
	case VCQ_IOC_STATS:
		return vcq_stats(file, uarg);
	default:
		return -ENOTTY;
	}
}

static bool vcq_has_done(struct vcq_ctx *ctx)
{
	return READ_ONCE(ctx->ndone) > 0;
}

/* Returns fences completed since the last read() as an array of u64. */
static ssize_t vcq_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct vcq_ctx *ctx = file->private_data;
	size_t max = min_t(size_t, count / sizeof(u64), VCQ_READ_BATCH);
	u64 fences[VCQ_READ_BATCH];
	struct vcq_job *job, *tmp;
	size_t n = 0;
	int ret;

	if (!max)
		return -EINVAL;

	for (;;) {
		mutex_lock(&ctx->lock);
		list_for_each_entry_safe(job, tmp, &ctx->done, done_node) {
			if (n == max)
				break;
			fences[n++] = job->fence;
			list_del_init(&job->done_node);
		}
		WRITE_ONCE(ctx->ndone, ctx->ndone - n);
		mutex_unlock(&ctx->lock);
		if (n)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(ctx->wq, vcq_has_done(ctx));
		if (ret)
			return ret;
	}

	if (copy_to_user(buf, fences, n * sizeof(u64)))
		return -EFAULT;
	return n * sizeof(u64);
}

static __poll_t vcq_poll(struct file *file, poll_table *wait)
{
	struct vcq_ctx *ctx = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctx->wq, wait);
	poll_wait(file, &vcq_dev.space_wq, wait);
	if (vcq_has_done(ctx))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(vcq_dev.queued) < queue_depth)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static int vcq_open(struct inode *inode, struct file *file)
{
	struct vcq_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	kref_init(&ctx->ref);
	mutex_init(&ctx->lock);
	xa_init(&ctx->jobs);
	INIT_LIST_HEAD(&ctx->done);
	init_waitqueue_head(&ctx->wq);
	file->private_data = ctx;
	return stream_open(inode, file);
}

static int vcq_release(struct inode *inode, struct file *file)
{
	struct vcq_ctx *ctx = file->private_data;

	kref_put(&ctx->ref, vcq_ctx_free);
	return 0;
}

static const struct file_operations vcq_fops = {
	.owner		= THIS_MODULE,
	.open		= vcq_open,
	.release	= vcq_release,
	.read		= vcq_read,
	.poll		= vcq_poll,
	.unlocked_ioctl	= vcq_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice vcq_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "vcq",
	.fops	= &vcq_fops,
};

static int __init vcq_init(void)
{
	int ret;

	if (!queue_depth || queue_depth > VCQ_MAX_QUEUE_DEPTH) {
		pr_err("queue_depth must be 1-%u\n", VCQ_MAX_QUEUE_DEPTH);
		return -EINVAL;
	}

	spin_lock_init(&vcq_dev.lock);
	INIT_LIST_HEAD(&vcq_dev.pending);
	init_waitqueue_head(&vcq_dev.space_wq);
	INIT_WORK(&vcq_dev.work, vcq_engine_work);
	vcq_dev.engine = alloc_ordered_workqueue("vcq_engine", 0);
	if (!vcq_dev.engine)
		return -ENOMEM;

	ret = misc_register(&vcq_misc);
	if (ret) {
		destroy_workqueue(vcq_dev.engine);
		return ret;
	}
	pr_info("/dev/vcq ready, queue_depth=%u\n", queue_depth);
	return 0;
}

static void __exit vcq_exit(void)
{
	misc_deregister(&vcq_misc);
	/*
	 * Open files pin the module, so none remain. Draining the engine finishes
	 * jobs left by closed files, which frees their contexts.
	 */
	destroy_workqueue(vcq_dev.engine);
}

module_init(vcq_init);
module_exit(vcq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nishant Kumar Jha");
MODULE_DESCRIPTION("Virtual command-queue device with fences, poll and backpressure");
