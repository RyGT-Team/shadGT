// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/frame_queue_trace.h"
#include <atomic>
#include <cassert>
#include <thread>
using namespace Common::FrameQueueTrace;
int main() {
  Row row{};
  row.ticket = {3, 1, 1, 5, 10, 20};
  row.flip_begin = 30;
  row.present_begin = 40;
  row.present_end = 50;
  assert(CompleteRow(row, 3));
  assert(!CompleteRow(row, 0));
  assert(!CompleteRow(row, 2));
  assert(!CompleteRow(row, 5));
  row.ticket.prepare_begin = 0;
  assert(!CompleteRow(row, 3));
  row.ticket.prepare_begin = 5;
  row.ticket.submit = 6;
  assert(!CompleteRow(row, 3));
  row.ticket.submit = 1;
  row.ticket.dequeue = 0;
  assert(!CompleteRow(row, 3));
  row.ticket.dequeue = 20;
  row.present_end = 39;
  assert(!CompleteRow(row, 3));
  row.present_end = 50;
  Ring<2> ring;
  assert(ring.Push(row));
  row.ticket.id = 2;
  assert(ring.Push(row));
  row.ticket.id = 3;
  assert(!ring.Push(row));
  Row out;
  assert(ring.Pop(out) && out.ticket.id == 1);
  assert(ring.Pop(out) && out.ticket.id == 2);
  assert(!ring.Pop(out));
  // A stalled consumer leaves the bounded queue full; producers drop
  // immediately.
  Ring<4> blocked;
  for (int i = 0; i < 4; ++i)
    assert(blocked.Push(row));
  std::atomic<int> failures{};
  std::thread one([&] {
    for (int i = 0; i < 10000; ++i)
      if (!blocked.Push(row))
        ++failures;
  });
  std::thread two([&] {
    for (int i = 0; i < 10000; ++i)
      if (!blocked.Push(row))
        ++failures;
  });
  one.join();
  two.join();
  assert(failures == 20000);
}
