// SPDX-License-Identifier: GPL-2.0
// Functional, error-path and concurrency tests for /dev/vcq. Run as root with
// vcq.ko loaded; the tests change /sys/module/vcq/parameters/engine_delay_us.
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "vcq.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

struct Failure {};

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                       \
      throw Failure{};                                                           \
    }                                                                            \
  } while (0)

#define CHECK_ERR(expr, err)                                                       \
  do {                                                                             \
    int r_ = (expr);                                                               \
    if (r_ != -(err)) {                                                            \
      std::fprintf(stderr, "    %s:%d: %s = %d (%s), expected -%s\n", __FILE__,   \
                   __LINE__, #expr, r_, r_ ? std::strerror(-r_) : "success", #err); \
      throw Failure{};                                                             \
    }                                                                              \
  } while (0)

uint64_t ptr(const void* p) { return reinterpret_cast<uintptr_t>(p); }

void set_delay(unsigned us) {
  std::ofstream f("/sys/module/vcq/parameters/engine_delay_us");
  f << us;
  f.flush();
  if (!f) throw std::runtime_error("cannot write engine_delay_us (run as root)");
}

uint32_t crc32_ref(const uint8_t* p, size_t n) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = static_cast<uint8_t>(rng());
  return v;
}

// ---------------------------------------------------------------- operations

void test_nop_roundtrip() {
  vcq::Device d;
  uint64_t f = d.nop();
  CHECK(f == 1);
  auto r = d.wait(f);
  CHECK(r && *r == 0);
  auto st = d.stats();
  CHECK(st.submitted == 1 && st.completed == 1 && st.jobs == 0);
}

void test_fill() {
  vcq::Device d;
  std::vector<uint8_t> out(4096, 0);
  auto r = d.wait(d.fill(out.size(), 0xab), out.data(), out.size());
  CHECK(r && *r == out.size());
  CHECK(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0xab; }));
}

void test_xor() {
  vcq::Device d;
  auto in = random_bytes(10000, 1);
  std::vector<uint8_t> out(in.size());
  auto r = d.wait(d.xor_bytes(in.data(), in.size(), 0x5a), out.data(), out.size());
  CHECK(r && *r == in.size());
  for (size_t i = 0; i < in.size(); ++i) CHECK(out[i] == (in[i] ^ 0x5a));
}

void test_crc32() {
  vcq::Device d;
  const char* check = "123456789";
  CHECK(d.wait(d.crc32(check, 9)) == 0xcbf43926u);  // standard CRC-32 check value
  for (uint32_t len : {1u, 3u, 4096u, 65537u, VCQ_MAX_PAYLOAD}) {
    auto in = random_bytes(len, len);
    CHECK(d.wait(d.crc32(in.data(), len)) == crc32_ref(in.data(), len));
  }
}

// ------------------------------------------------------------------- fences

void test_fences_complete_in_order() {
  set_delay(500);
  vcq::Device d;
  for (uint64_t i = 1; i <= 50; ++i) CHECK(d.nop() == i);
  CHECK(d.wait(50).has_value());
  // Fence 50 completing implies 1-49 have too, so none of them block.
  for (uint64_t f = 1; f < 50; ++f) CHECK(d.wait(f, nullptr, 0, 0).has_value());
  CHECK(d.stats().completed == 50);
}

void test_files_are_independent_timelines() {
  vcq::Device a, b;
  CHECK(a.nop() == 1);
  CHECK(b.nop() == 1);
  CHECK(b.nop() == 2);
  CHECK(a.wait(1) && b.wait(2) && b.wait(1));
}

// -------------------------------------------------------------- error paths

void test_submit_validation() {
  vcq::Device d;
  vcq_submit s{};
  s.op = 99;
  CHECK_ERR(d.submit_raw(s), EINVAL);
  s = {};
  s.op = VCQ_OP_NOP;
  s.len = 8;
  CHECK_ERR(d.submit_raw(s), EINVAL);
  s = {};
  s.op = VCQ_OP_FILL;
  CHECK_ERR(d.submit_raw(s), EINVAL);
  s.len = VCQ_MAX_PAYLOAD + 1;
  CHECK_ERR(d.submit_raw(s), EMSGSIZE);
  s = {};
  s.op = VCQ_OP_XOR;
  s.len = 16;
  CHECK_ERR(d.submit_raw(s), EFAULT);  // NULL input
  s.op = VCQ_OP_CRC32;
  s.src = 1;
  CHECK_ERR(d.submit_raw(s), EFAULT);  // unmapped input
  CHECK(::ioctl(d.fd(), VCQ_IOC_SUBMIT, nullptr) == -1 && errno == EFAULT);
  CHECK(::ioctl(d.fd(), _IO(VCQ_IOC_MAGIC, 42)) == -1 && errno == ENOTTY);
  CHECK(d.stats().submitted == 0);  // rejected jobs use no fence
}

void test_wait_errors() {
  vcq::Device d;
  vcq_wait w{};
  CHECK_ERR(d.wait_raw(w), EINVAL);  // fence 0
  w.fence = 1;
  CHECK_ERR(d.wait_raw(w), EINVAL);  // never submitted

  std::vector<uint8_t> out(64);
  w = {};
  w.fence = d.fill(out.size(), 0x11);
  w.timeout_ms = VCQ_WAIT_INFINITE;
  w.dst = ptr(out.data());
  w.dst_len = 32;
  CHECK_ERR(d.wait_raw(w), ENOSPC);
  w.dst = 1;
  w.dst_len = out.size();
  CHECK_ERR(d.wait_raw(w), EFAULT);
  // A failed copy-out leaves the job in place to retry.
  w.dst = ptr(out.data());
  CHECK(d.wait_raw(w) == 0 && w.result == out.size() && out[63] == 0x11);
  CHECK_ERR(d.wait_raw(w), ENOENT);  // already retrieved
}

void test_wait_timeout() {
  set_delay(300000);
  vcq::Device d;
  uint64_t f = d.nop();
  CHECK(!d.wait(f, nullptr, 0, 0));  // timeout 0 only checks
  auto t0 = Clock::now();
  CHECK(!d.wait(f, nullptr, 0, 20));
  CHECK(Clock::now() - t0 >= milliseconds(10));
  CHECK(d.wait(f).has_value());  // no limit: returns once the engine finishes
}

// ------------------------------------------------------------ poll and read

void test_poll_and_read() {
  set_delay(2000);
  vcq::Device d("/dev/vcq", true);
  CHECK(d.completed().empty());  // nothing yet: read() gives EAGAIN
  for (int i = 0; i < 5; ++i) d.nop();

  std::vector<uint64_t> seen;
  while (seen.size() < 5) {
    pollfd p{d.fd(), POLLIN, 0};
    CHECK(::poll(&p, 1, 5000) == 1 && (p.revents & POLLIN));
    auto got = d.completed();
    seen.insert(seen.end(), got.begin(), got.end());
  }
  CHECK((seen == std::vector<uint64_t>{1, 2, 3, 4, 5}));
  for (uint64_t f : seen) CHECK(d.wait(f, nullptr, 0, 0).has_value());
  pollfd p{d.fd(), POLLIN, 0};
  CHECK(::poll(&p, 1, 0) == 0);
}

void test_wait_consumes_completion_event() {
  vcq::Device d("/dev/vcq", true);
  CHECK(d.wait(d.nop()).has_value());
  CHECK(d.completed().empty());  // retrieved jobs are not reported by read()
}

// ------------------------------------------------------------- backpressure

void test_nonblocking_backpressure() {
  set_delay(100000);  // 100 ms per job keeps the queue full while we submit
  vcq::Device d("/dev/vcq", true);
  const uint32_t depth = d.stats().queue_depth;
  std::vector<uint64_t> fences;
  for (;;) {
    vcq_submit s{};
    s.op = VCQ_OP_NOP;
    int r = d.submit_raw(s);
    if (r == -EAGAIN) break;
    CHECK(r == 0);
    fences.push_back(s.fence);
    CHECK(fences.size() <= depth + 4);
  }
  CHECK(fences.size() >= depth);
  CHECK(d.stats().device_queued == depth);
  pollfd p{d.fd(), POLLOUT, 0};
  CHECK(::poll(&p, 1, 0) == 0);  // full queue: not writable

  set_delay(0);
  for (uint64_t f : fences) CHECK(d.wait(f).has_value());
  CHECK(d.stats().device_queued == 0);
}

void test_blocking_submit_waits_for_space() {
  set_delay(1000);
  vcq::Device d;
  const uint32_t depth = d.stats().queue_depth;
  const uint32_t n = depth * 3;
  auto t0 = Clock::now();
  uint64_t last = 0;
  for (uint32_t i = 0; i < n; ++i) last = d.nop();
  // Each submit past queue_depth waited about 1 ms for the engine; allow half.
  CHECK(Clock::now() - t0 >= milliseconds((n - depth) / 2));
  CHECK(last == n && d.wait(last).has_value());
  for (uint64_t f = 1; f < n; ++f) CHECK(d.wait(f, nullptr, 0, 0).has_value());
}

void test_per_file_job_limit() {
  vcq::Device d;
  for (int i = 0; i < 256; ++i) d.nop();
  vcq_submit s{};
  s.op = VCQ_OP_NOP;
  CHECK_ERR(d.submit_raw(s), EBUSY);
  CHECK(d.wait(1).has_value());
  CHECK(d.submit_raw(s) == 0 && s.fence == 257);
}

// ----------------------------------------------------------------- lifetime

void test_close_with_jobs_in_flight() {
  set_delay(5000);
  {
    vcq::Device d;
    for (int i = 0; i < 20; ++i) d.fill(65536, i);
  }  // closed while its jobs are still queued
  vcq::Device probe;
  auto deadline = Clock::now() + std::chrono::seconds(10);
  while (probe.stats().device_queued != 0) {
    CHECK(Clock::now() < deadline);
    std::this_thread::sleep_for(milliseconds(10));
  }
  set_delay(0);
  CHECK(probe.wait(probe.nop()).has_value());
}

// -------------------------------------------------------------- concurrency

struct Pending {
  uint64_t fence;
  uint32_t op;
  uint32_t len;
  uint8_t key;
  std::vector<uint8_t> in;
};

bool verify(const Pending& p, std::optional<uint64_t> r, const std::vector<uint8_t>& out) {
  if (!r) return false;
  switch (p.op) {
    case VCQ_OP_NOP:
      return *r == 0;
    case VCQ_OP_FILL:
      return *r == p.len &&
             std::all_of(out.begin(), out.begin() + p.len, [&](uint8_t b) { return b == p.key; });
    case VCQ_OP_XOR:
      for (uint32_t i = 0; i < p.len; ++i)
        if (out[i] != (p.in[i] ^ p.key)) return false;
      return *r == p.len;
    default:
      return *r == crc32_ref(p.in.data(), p.len);
  }
}

// Each thread owns a file and keeps a window of 16 jobs in flight.
void test_threads_with_own_files() {
  constexpr int kThreads = 8, kJobs = 1000, kWindow = 16;
  std::atomic<int> verified{0}, errors{0};
  auto worker = [&](int id) {
    try {
      vcq::Device d;
      std::mt19937 rng(100 + id);
      std::deque<Pending> window;
      std::vector<uint8_t> out(16384);
      auto retire = [&] {
        Pending p = std::move(window.front());
        window.pop_front();
        auto r = d.wait(p.fence, out.data(), out.size());
        if (verify(p, r, out))
          ++verified;
        else
          ++errors;
      };
      for (int i = 0; i < kJobs; ++i) {
        if (window.size() == kWindow) retire();
        Pending p{};
        p.op = 1 + rng() % 4;
        p.len = p.op == VCQ_OP_NOP ? 0 : 1 + rng() % 16384;
        p.key = static_cast<uint8_t>(rng());
        if (p.op == VCQ_OP_XOR || p.op == VCQ_OP_CRC32) p.in = random_bytes(p.len, rng());
        switch (p.op) {
          case VCQ_OP_NOP: p.fence = d.nop(); break;
          case VCQ_OP_FILL: p.fence = d.fill(p.len, p.key); break;
          case VCQ_OP_XOR: p.fence = d.xor_bytes(p.in.data(), p.len, p.key); break;
          default: p.fence = d.crc32(p.in.data(), p.len);
        }
        window.push_back(std::move(p));
      }
      while (!window.empty()) retire();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "    thread %d: %s\n", id, e.what());
      ++errors;
    }
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker, i);
  for (auto& t : threads) t.join();
  CHECK(errors == 0);
  CHECK(verified == kThreads * kJobs);
}

// Several threads submit and wait on one shared file.
void test_threads_sharing_a_file() {
  constexpr int kThreads = 4, kJobs = 500;
  vcq::Device d;
  std::atomic<int> verified{0}, errors{0};
  auto worker = [&](int id) {
    try {
      std::mt19937 rng(200 + id);
      for (int i = 0; i < kJobs; ++i) {
        auto in = random_bytes(1 + rng() % 4096, rng());
        auto r = d.wait(d.crc32(in.data(), in.size()));
        if (r && *r == crc32_ref(in.data(), in.size()))
          ++verified;
        else
          ++errors;
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "    thread %d: %s\n", id, e.what());
      ++errors;
    }
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker, i);
  for (auto& t : threads) t.join();
  CHECK(errors == 0 && verified == kThreads * kJobs);
  auto st = d.stats();
  CHECK(st.submitted == kThreads * kJobs && st.completed == st.submitted && st.jobs == 0);
}

}  // namespace

int main() {
  struct {
    const char* name;
    void (*fn)();
  } const tests[] = {
      {"nop_roundtrip", test_nop_roundtrip},
      {"fill", test_fill},
      {"xor", test_xor},
      {"crc32", test_crc32},
      {"fences_complete_in_order", test_fences_complete_in_order},
      {"files_are_independent_timelines", test_files_are_independent_timelines},
      {"submit_validation", test_submit_validation},
      {"wait_errors", test_wait_errors},
      {"wait_timeout", test_wait_timeout},
      {"poll_and_read", test_poll_and_read},
      {"wait_consumes_completion_event", test_wait_consumes_completion_event},
      {"nonblocking_backpressure", test_nonblocking_backpressure},
      {"blocking_submit_waits_for_space", test_blocking_submit_waits_for_space},
      {"per_file_job_limit", test_per_file_job_limit},
      {"close_with_jobs_in_flight", test_close_with_jobs_in_flight},
      {"threads_with_own_files", test_threads_with_own_files},
      {"threads_sharing_a_file", test_threads_sharing_a_file},
  };

  int failed = 0;
  for (const auto& t : tests) {
    bool ok = true;
    try {
      set_delay(0);
      t.fn();
    } catch (const Failure&) {
      ok = false;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "    exception: %s\n", e.what());
      ok = false;
    }
    try {
      set_delay(0);
    } catch (const std::exception&) {
    }
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
    std::fflush(stdout);
    failed += !ok;
  }
  std::printf("%zu tests, %d failed\n", sizeof(tests) / sizeof(tests[0]), failed);
  return failed ? 1 : 0;
}
