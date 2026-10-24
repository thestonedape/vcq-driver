// SPDX-License-Identifier: GPL-2.0
// Submit-to-retire latency for NOP jobs, and CRC32 throughput with a window
// of jobs in flight. Numbers describe the machine it runs on: under QEMU
// without KVM they measure the emulator, not the driver.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <vector>

#include "vcq.hpp"

using Clock = std::chrono::steady_clock;

int main() {
  vcq::Device d;

  constexpr int kIters = 2000;
  std::vector<double> us;
  us.reserve(kIters);
  for (int i = 0; i < kIters; ++i) {
    auto t0 = Clock::now();
    d.wait(d.nop());
    us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
  }
  std::sort(us.begin(), us.end());
  std::printf("nop round trip: p50 %.1f us, p99 %.1f us (%d jobs)\n", us[kIters / 2],
              us[kIters * 99 / 100], kIters);

  constexpr size_t kWindow = 32, kBytes = 16u << 20;
  for (uint32_t size : {4096u, 65536u, VCQ_MAX_PAYLOAD}) {
    std::vector<uint8_t> buf(size, 0x5a);
    const size_t jobs = kBytes / size;
    std::deque<uint64_t> window;
    auto t0 = Clock::now();
    for (size_t i = 0; i < jobs; ++i) {
      if (window.size() == kWindow) {
        d.wait(window.front());
        window.pop_front();
      }
      window.push_back(d.crc32(buf.data(), size));
    }
    for (uint64_t f : window) d.wait(f);
    double s = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("crc32 %6u B jobs: %8.1f MiB/s, %9.0f jobs/s\n", size, kBytes / s / (1 << 20),
                jobs / s);
  }
  return 0;
}
