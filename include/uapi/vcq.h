/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * User-space interface of the vcq virtual command-queue device (/dev/vcq).
 *
 * Every open file is an independent timeline. VCQ_IOC_SUBMIT queues a job and
 * returns its fence (1, 2, 3, ... per file). Jobs on one file complete in fence
 * order, so once fence N has completed every earlier fence has too.
 * VCQ_IOC_WAIT blocks until a fence completes, then copies the job's output
 * back and releases it. read() returns the fences that completed since the
 * last read() as an array of __u64, and poll() reports EPOLLIN when there are
 * any, or EPOLLOUT when the device queue has room for another job.
 */
#ifndef _UAPI_VCQ_H
#define _UAPI_VCQ_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VCQ_MAX_PAYLOAD		(256U * 1024)	/* bytes per job */
#define VCQ_WAIT_INFINITE	0xffffffffU

enum vcq_op {
	VCQ_OP_NOP   = 1,	/* no payload, result 0 */
	VCQ_OP_FILL  = 2,	/* output: len bytes of (arg & 0xff) */
	VCQ_OP_XOR   = 3,	/* output: input bytes XOR (arg & 0xff) */
	VCQ_OP_CRC32 = 4,	/* result: CRC-32 (IEEE 802.3) of the input */
};

struct vcq_submit {
	__u32 op;		/* enum vcq_op */
	__u32 len;		/* payload bytes, 0 for VCQ_OP_NOP */
	__u64 src;		/* user pointer to input (XOR, CRC32) */
	__u64 arg;		/* operation argument */
	__u64 fence;		/* out: fence of the queued job */
};

struct vcq_wait {
	__u64 fence;		/* job to wait for */
	__u64 dst;		/* user pointer for output (FILL, XOR) */
	__u32 dst_len;		/* bytes available at dst */
	__u32 timeout_ms;	/* 0: do not block, VCQ_WAIT_INFINITE: no limit */
	__u64 result;		/* out: job result */
};

struct vcq_stats {
	__u64 submitted;	/* last fence handed out on this file */
	__u64 completed;	/* last fence completed on this file */
	__u32 jobs;		/* jobs on this file not yet retrieved */
	__u32 device_queued;	/* jobs waiting or running, all files */
	__u32 queue_depth;	/* device-wide limit on device_queued */
	__u32 pad;
};

#define VCQ_IOC_MAGIC	'V'
#define VCQ_IOC_SUBMIT	_IOWR(VCQ_IOC_MAGIC, 1, struct vcq_submit)
#define VCQ_IOC_WAIT	_IOWR(VCQ_IOC_MAGIC, 2, struct vcq_wait)
#define VCQ_IOC_STATS	_IOR(VCQ_IOC_MAGIC, 3, struct vcq_stats)

#endif /* _UAPI_VCQ_H */
