// SPDX-License-Identifier: GPL-2.0
// C++17 wrapper around /dev/vcq. One Device is one fence timeline. submit,
// wait, completed and stats may be called from several threads at once; the
// driver serialises them per file. Moving or destroying a Device is not
// thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vcq.h"

namespace vcq {

class Device {
 public:
  explicit Device(const std::string& path = "/dev/vcq", bool nonblock = false);
  ~Device();
  Device(Device&& other) noexcept;
  Device& operator=(Device&& other) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  int fd() const { return fd_; }

  // Each queues one job and returns its fence. Throws std::system_error.
  uint64_t nop();
  uint64_t fill(uint32_t len, uint8_t value);
  uint64_t xor_bytes(const void* in, uint32_t len, uint8_t key);
  uint64_t crc32(const void* in, uint32_t len);

  // Waits for `fence`, copies any output into `out` and releases the job.
  // Returns the job result, or std::nullopt if `timeout_ms` passed first.
  std::optional<uint64_t> wait(uint64_t fence, void* out = nullptr, uint32_t out_len = 0,
                               uint32_t timeout_ms = VCQ_WAIT_INFINITE);

  // Fences completed since the last call. Blocks until there is at least one
  // unless the device was opened nonblocking, in which case it may be empty.
  std::vector<uint64_t> completed(size_t max = 64);

  vcq_stats stats();

  // Raw ioctls returning 0 or -errno, for callers that test error paths.
  int submit_raw(vcq_submit& s);
  int wait_raw(vcq_wait& w);

 private:
  uint64_t submit(uint32_t op, const void* src, uint32_t len, uint64_t arg);
  int fd_ = -1;
};

}  // namespace vcq
