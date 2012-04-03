//===-- tsan_sync_test.cc ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//
#include "tsan_sync.h"
#include "tsan_slab.h"
#include "tsan_rtl.h"
#include "gtest/gtest.h"

#include <stdlib.h>
#include <stdint.h>
#include <map>

namespace __tsan {

TEST(Sync, Table) {
  const uintptr_t kIters = 512*1024;
  const uintptr_t kRange = 10000;

  ScopedInRtl in_rtl;
  ThreadState *thr = cur_thread();
  uptr pc = 0;

  SlabAlloc alloc(sizeof(SyncVar));
  SlabCache slab(&alloc);
  SyncTab tab;
  SyncVar *golden[kRange] = {};
  unsigned seed = 0;
  for (uintptr_t i = 0; i < kIters; i++) {
    uintptr_t addr = rand_r(&seed) % (kRange - 1) + 1;
    if (rand_r(&seed) % 2) {
      // Get or add.
      SyncVar *v = tab.GetAndLock(thr, pc, &slab, addr, true);
      CHECK(golden[addr] == 0 || golden[addr] == v);
      CHECK(v->addr == addr);
      golden[addr] = v;
      v->mtx.Unlock();
    } else {
      // Remove.
      SyncVar *v = tab.GetAndRemove(addr);
      CHECK(golden[addr] == v);
      if (v) {
        CHECK(v->addr == addr);
        golden[addr] = 0;
        v->creation_stack.Free(thr);
        v->~SyncVar();
        slab.Free(v);
      }
    }
  }
  for (uintptr_t addr = 0; addr < kRange; addr++) {
    if (golden[addr] == 0)
      continue;
    SyncVar *v = tab.GetAndRemove(addr);
    CHECK(v == golden[addr]);
    CHECK(v->addr == addr);
    v->creation_stack.Free(thr);
    v->~SyncVar();
    slab.Free(v);
  }
}

}  // namespace __tsan
