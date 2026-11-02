// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/frame_queue_trace.h"
#include "common/thread.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
namespace Common {
void SetCurrentThreadName(const char *) {}
void SetCurrentThreadPriority(ThreadPriority) {}
} // namespace Common
using namespace Common::FrameQueueTrace;
static void Env(const char *key, const std::string &value) {
#ifdef _WIN32
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}
static std::string Read(const std::string &path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "disabled") {
    Env("RYGT_FRAME_QUEUE_TRACE", "0");
    Initialize();
    assert(!Start().generation && current == nullptr);
    return 0;
  }
  const std::string base = "frame-queue-runtime-test";
  std::filesystem::remove(base + ".csv");
  std::filesystem::remove(base + ".control");
  std::filesystem::remove(base + ".status");
  Env("RYGT_FRAME_QUEUE_TRACE", "1");
  Env("RYGT_FRAME_QUEUE_TRACE_OUTPUT", base + ".csv");
  Env("RYGT_FRAME_QUEUE_TRACE_CONTROL", base + ".control");
  Env("RYGT_FRAME_QUEUE_TRACE_STATUS", base + ".status");
  {
    std::ofstream f(base + ".control");
    f << 0;
  }
  Initialize();
  assert(!Start().generation);
  {
    std::ofstream f(base + ".control");
    f << 1;
  }
  bool on = false;
  for (int i = 0; i < 200 && !on; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    on = Read(base + ".status").find("active=1") != std::string::npos;
  }
  assert(on);
  Ticket t = Start();
  assert(t.generation);
  Prepare(t);
  Enqueue(t);
  Dequeue(t);
  Row r{};
  r.ticket = t;
  r.flip_begin = Now(t);
  r.present_begin = Now(t);
  r.present_end = Now(t);
  Complete(r);
  bool written = false;
  for (int i = 0; i < 200 && !written; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto data = Read(base + ".csv");
    written = data.find('\n') != std::string::npos &&
              data.find('\n', data.find('\n') + 1) != std::string::npos;
  }
  assert(written);
  {
    std::ofstream f(base + ".control");
    f << "invalid";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  assert(Valid(t));
  std::filesystem::remove(base + ".control");
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  assert(Valid(t));
  PauseSinkForTest(true);
  bool paused = false;
  for (int i = 0; i < 200 && !paused; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    paused = SinkIsPausedForTest();
  }
  assert(paused);
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 9000; ++i)
    Complete(r);
  assert(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
  PauseSinkForTest(false);
  bool dropped = false;
  for (int i = 0; i < 200 && !dropped; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto status = Read(base + ".status");
    auto at = status.find("dropped=");
    dropped = at != std::string::npos && status.substr(at, 10) != "dropped=0\n";
  }
  assert(dropped);
  Ticket old = Start();
  {
    std::ofstream f(base + ".control");
    f << 0;
  }
  bool off = false;
  for (int i = 0; i < 200 && !off; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    off = Read(base + ".status").find("active=0") != std::string::npos;
  }
  assert(off && !Valid(old) && !Start().generation);
}
