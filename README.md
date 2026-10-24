# vcq: a virtual command-queue Linux driver

`vcq` is a Linux kernel module that exposes `/dev/vcq`, a simulated
accelerator with an asynchronous command queue. It follows the pattern GPU and
accelerator drivers use to accept work: user space submits jobs with an ioctl,
an engine executes them in the background, and completion is signalled through
per-context fences that a program can wait on, read or poll.

The repository contains the driver (C), a C++17 user-space library, a test
suite with multithreaded stress tests, a small benchmark, and a harness that
boots a stock Ubuntu kernel under QEMU so the module can be tested from any
host with Docker.

```
 user space                                 kernel
 ----------                                 ------
 vcq::Device ──ioctl(SUBMIT)──▶ copy input ─▶ reserve slot ─▶ ctx job table (fence N)
                                                     │                 │
                                                     ▼                 │
                                       device FIFO (≤ queue_depth)     │
                                                     │                 │
                                     ordered workqueue = engine        │
                                       execute ─▶ mark fence N done ───┘
                                                     │
             ioctl(WAIT) ◀── wake waiters ◀──────────┤
             read()/poll() ◀── done list ◀───────────┘
```

## Interface

| Call | What it does |
|---|---|
| `VCQ_IOC_SUBMIT` | Copies the job's input into the kernel, queues it and returns its fence. Blocks while the device queue is full, or fails with `EAGAIN` on an `O_NONBLOCK` file. |
| `VCQ_IOC_WAIT` | Waits for a fence (no wait, a timeout in ms, or forever), copies the output to user space and releases the job. |
| `VCQ_IOC_STATS` | Returns the file's submitted/completed fences and job count, and the device queue occupancy. |
| `read()` | Returns completed fences not yet reported, as `__u64` values. |
| `poll()` | `EPOLLIN` when completed fences are waiting to be read, `EPOLLOUT` when the device queue has room. |

Operations: `NOP`, `FILL` (output only), `XOR` (input to output) and `CRC32`
(input to result), with payloads up to 256 KiB. See
[`include/uapi/vcq.h`](include/uapi/vcq.h).

## Design

**Fence timelines.** Every open file is a context with its own timeline:
fences 1, 2, 3, ... in submission order. The context mutex serialises
submission, so a job's position in the device FIFO matches its fence and jobs
complete in fence order. Waiting for fence N therefore also guarantees that
every earlier fence on that file has completed, which is the property GPU
fences provide.

**Engine.** One ordered workqueue executes jobs from all files in FIFO order.
`engine_delay_us` (writable in `/sys/module/vcq/parameters/`) simulates
hardware execution time, which the tests use to hold the queue full.

**Backpressure.** `queue_depth` (default 64) bounds the jobs waiting or
running across the device. A slot is reserved before the job is queued and
released on completion. Each file may also hold at most 256 unretrieved jobs,
which caps the kernel memory one file can pin.

**Locking.** A spinlock protects the device FIFO and slot count; a mutex per
context protects its job table (an xarray keyed by fence), done list and
fence counters. The context mutex may be held while taking the device
spinlock, never the reverse. Wait conditions read single fields with
`READ_ONCE` so they never take a sleeping lock.

**Lifetime.** The open file holds one reference on its context and every
queued job holds another (`kref`). A file can be closed with jobs in flight:
they finish, and the last reference frees the context and any unretrieved
jobs. Module unload drains the engine with `destroy_workqueue`.

**Error handling.** Invalid operations and sizes are rejected before any
allocation, bad user pointers return `EFAULT`, and `VCQ_IOC_WAIT` copies
output before releasing the job, so a short buffer (`ENOSPC`) or a fault can
be retried.

## Build and test

On Linux, with headers for the running kernel installed:

```sh
make                # vcq.ko, build/vcq_test, build/vcq_bench
sudo make check     # insmod, run the tests, rmmod
```

On Windows or macOS, or to avoid loading the module on your own machine:

```sh
make qemu-test      # builds in Docker, boots Ubuntu 24.04's kernel in QEMU
```

The QEMU harness builds the module against the image's kernel, boots it with
a busybox initramfs, runs the tests and benchmark as root, unloads the module
and fails if the kernel log contains `BUG`, `WARNING`, `Oops` or hung-task
reports. CI runs the same harness, the tests on the GitHub runner's own
kernel, and `checkpatch.pl --strict`.

The 17 tests cover each operation against a user-space reference, fence
ordering and independent timelines, every error path, timeouts, `poll` and
`read`, blocking and non-blocking backpressure, the per-file limit, closing a
file with jobs in flight, eight threads with their own files keeping 16 jobs
in flight each, and four threads sharing one file.

## Limitations

- Jobs use kernel bounce buffers. A real device would DMA directly from pinned
  user pages (`pin_user_pages`) or an `mmap`ed buffer.
- Completion comes from a workqueue rather than an interrupt handler, and there
  is a single engine.
- `/dev/vcq` is root-only (the misc-device default) and the driver needs a
  64-bit kernel, because fences index an xarray.

## License

GPL-2.0. The user-space header carries the Linux syscall note.
