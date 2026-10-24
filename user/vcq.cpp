// SPDX-License-Identifier: GPL-2.0
#include "vcq.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace vcq {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

uint64_t to_user_ptr(const void* p) { return reinterpret_cast<uintptr_t>(p); }

}  // namespace

Device::Device(const std::string& path, bool nonblock) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (nonblock ? O_NONBLOCK : 0));
  if (fd_ < 0) throw_errno(path.c_str());
}

Device::~Device() {
  if (fd_ >= 0) ::close(fd_);
}

Device::Device(Device&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Device& Device::operator=(Device&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

int Device::submit_raw(vcq_submit& s) {
  return ::ioctl(fd_, VCQ_IOC_SUBMIT, &s) == 0 ? 0 : -errno;
}

int Device::wait_raw(vcq_wait& w) {
  return ::ioctl(fd_, VCQ_IOC_WAIT, &w) == 0 ? 0 : -errno;
}

uint64_t Device::submit(uint32_t op, const void* src, uint32_t len, uint64_t arg) {
  vcq_submit s{};
  s.op = op;
  s.len = len;
  s.src = to_user_ptr(src);
  s.arg = arg;
  if (::ioctl(fd_, VCQ_IOC_SUBMIT, &s) != 0) throw_errno("VCQ_IOC_SUBMIT");
  return s.fence;
}

uint64_t Device::nop() { return submit(VCQ_OP_NOP, nullptr, 0, 0); }

uint64_t Device::fill(uint32_t len, uint8_t value) {
  return submit(VCQ_OP_FILL, nullptr, len, value);
}

uint64_t Device::xor_bytes(const void* in, uint32_t len, uint8_t key) {
  return submit(VCQ_OP_XOR, in, len, key);
}

uint64_t Device::crc32(const void* in, uint32_t len) {
  return submit(VCQ_OP_CRC32, in, len, 0);
}

std::optional<uint64_t> Device::wait(uint64_t fence, void* out, uint32_t out_len,
                                     uint32_t timeout_ms) {
  vcq_wait w{};
  w.fence = fence;
  w.dst = to_user_ptr(out);
  w.dst_len = out_len;
  w.timeout_ms = timeout_ms;
  if (::ioctl(fd_, VCQ_IOC_WAIT, &w) != 0) {
    if (errno == ETIMEDOUT) return std::nullopt;
    throw_errno("VCQ_IOC_WAIT");
  }
  return w.result;
}

std::vector<uint64_t> Device::completed(size_t max) {
  std::vector<uint64_t> fences(max);
  ssize_t n = ::read(fd_, fences.data(), fences.size() * sizeof(uint64_t));
  if (n < 0) {
    if (errno == EAGAIN) return {};
    throw_errno("read");
  }
  fences.resize(static_cast<size_t>(n) / sizeof(uint64_t));
  return fences;
}

vcq_stats Device::stats() {
  vcq_stats st{};
  if (::ioctl(fd_, VCQ_IOC_STATS, &st) != 0) throw_errno("VCQ_IOC_STATS");
  return st;
}

}  // namespace vcq
