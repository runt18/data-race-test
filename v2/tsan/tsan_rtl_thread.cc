//===-- tsan_rtl_thread.cc --------------------------------------*- C++ -*-===//
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

#include "tsan_rtl.h"
#include "tsan_mman.h"
#include "tsan_placement_new.h"
#include "tsan_platform.h"
#include "tsan_report.h"
#include "tsan_sync.h"

namespace __tsan {

const int kThreadQuarantineSize = 100;

void ThreadFinalize(ThreadState *thr) {
  CHECK_GT(thr->in_rtl, 0);
  if (!flags()->report_thread_leaks)
    return;
  Context *ctx = CTX();
  Lock l(&ctx->thread_mtx);
  for (int i = 0; i < kMaxTid; i++) {
    ThreadContext *tctx = ctx->threads[i];
    if (tctx == 0)
      continue;
    if (tctx->detached)
      continue;
    if (tctx->status != ThreadStatusCreated
        && tctx->status != ThreadStatusRunning
        && tctx->status != ThreadStatusFinished)
      continue;
/*
    ScopedReport rep(ReportTypeThreadLeak);
    rep.AddThread(tctx->tid);
    PrintReport(&rep);
*/

    Lock l(&ctx->report_mtx);
    ReportDesc &rep = *GetGlobalReport();
    internal_memset(&rep, 0, sizeof(rep));
    RegionAlloc alloc(rep.alloc, sizeof(rep.alloc));
    rep.typ = ReportTypeThreadLeak;
    rep.nthread = 1;
    rep.thread = alloc.Alloc<ReportThread>(1);
    rep.thread->id = tctx->tid;
    rep.thread->running = (tctx->status != ThreadStatusFinished);
    rep.thread->stack = SymbolizeStack(&alloc, tctx->creation_stack);
    PrintReport(&rep);
    ctx->nreported++;
  }
}

static void ThreadDead(ThreadState *thr, ThreadContext *tctx) {
  CHECK_GT(thr->in_rtl, 0);
  CHECK(tctx->status == ThreadStatusRunning
      || tctx->status == ThreadStatusFinished);
  DPrintf("#%d: ThreadDead uid=%lu\n", thr->tid, tctx->user_id);
  tctx->status = ThreadStatusDead;
  tctx->user_id = 0;
  tctx->sync.Free(&thr->clockslab);

  // Put to dead list.
  tctx->dead_next = 0;
  if (CTX()->dead_list_size == 0)
    CTX()->dead_list_head = tctx;
  else
    CTX()->dead_list_tail->dead_next = tctx;
  CTX()->dead_list_tail = tctx;
  CTX()->dead_list_size++;
}

int ThreadCreate(ThreadState *thr, uptr pc, uptr uid, bool detached) {
  CHECK_GT(thr->in_rtl, 0);
  Context *ctx = CTX();
  Lock l(&ctx->thread_mtx);
  int tid = -1;
  ThreadContext *tctx = 0;
  if (ctx->dead_list_size > kThreadQuarantineSize
      || ctx->thread_seq >= kMaxTid) {
    if (ctx->dead_list_size == 0) {
      Printf("ThreadSanitizer: %d thread limit exceeded. Dying.\n", kMaxTid);
      Die();
    }
    tctx = ctx->dead_list_head;
    ctx->dead_list_head = tctx->dead_next;
    ctx->dead_list_size--;
    if (ctx->dead_list_size == 0) {
      CHECK_EQ(tctx->dead_next, 0);
      ctx->dead_list_head = 0;
    }
    CHECK_EQ(tctx->status, ThreadStatusDead);
    tctx->status = ThreadStatusInvalid;
    tctx->reuse_count++;
    tid = tctx->tid;
    // The point to reclain dead_info.
    // delete tctx->dead_info;
  } else {
    tid = ctx->thread_seq++;
    tctx = new(virtual_alloc(sizeof(ThreadContext))) ThreadContext(tid);
    ctx->threads[tid] = tctx;
  }
  CHECK_NE(tctx, 0);
  CHECK_GE(tid, 0);
  CHECK_LT(tid, kMaxTid);
  DPrintf("#%d: ThreadCreate tid=%d uid=%lu\n", thr->tid, tid, uid);
  CHECK_EQ(tctx->status, ThreadStatusInvalid);
  tctx->status = ThreadStatusCreated;
  tctx->thr = 0;
  tctx->user_id = uid;
  tctx->unique_id = ctx->unique_thread_seq++;
  tctx->detached = detached;
  if (tid) {
    thr->fast_state.IncrementEpoch();
    // Can't increment epoch w/o writing to the trace as well.
    TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeMop, 0);
    thr->clock.set(thr->tid, thr->fast_state.epoch());
    thr->fast_synch_epoch = thr->fast_state.epoch();
    thr->clock.release(&tctx->sync, &thr->clockslab);

    tctx->creation_stack.ObtainCurrent(thr, pc);
  }
  return tid;
}

void ThreadStart(ThreadState *thr, int tid) {
  CHECK_GT(thr->in_rtl, 0);
  uptr stk_addr = 0;
  uptr stk_size = 0;
  uptr tls_addr = 0;
  uptr tls_size = 0;
  GetThreadStackAndTls(&stk_addr, &stk_size, &tls_addr, &tls_size);

  MemoryResetRange(thr, /*pc=*/ 1, stk_addr, stk_size);

  // Check that the thr object is in tls;
  const uptr thr_beg = (uptr)thr;
  const uptr thr_end = (uptr)thr + sizeof(*thr);
  CHECK_GE(thr_beg, tls_addr);
  CHECK_LE(thr_beg, tls_addr + tls_size);
  CHECK_GE(thr_end, tls_addr);
  CHECK_LE(thr_end, tls_addr + tls_size);
  // Since the thr object is huge, skip it.
  MemoryResetRange(thr, /*pc=*/ 2, tls_addr, thr_beg - tls_addr);
  MemoryResetRange(thr, /*pc=*/ 2, thr_end, tls_addr + tls_size - thr_end);

  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = CTX()->threads[tid];
  CHECK_NE(tctx, 0);
  CHECK_EQ(tctx->status, ThreadStatusCreated);
  tctx->status = ThreadStatusRunning;
  tctx->epoch0 = tctx->epoch1 + 1;
  tctx->epoch1 = (u64)-1;
  new(thr) ThreadState(CTX(), tid, tctx->epoch0, stk_addr, stk_size,
                       tls_addr, tls_size);
  tctx->thr = thr;
  thr->fast_synch_epoch = tctx->epoch0;
  thr->clock.set(tid, tctx->epoch0);
  thr->clock.acquire(&tctx->sync);
  DPrintf("#%d: ThreadStart epoch=%llu stk_addr=%lx stk_size=%lx "
      "tls_addr=%lx tls_size=%lx\n",
      tid, tctx->epoch0, stk_addr, stk_size, tls_addr, tls_size);
}

void ThreadFinish(ThreadState *thr) {
  CHECK_GT(thr->in_rtl, 0);
  // FIXME: Treat it as write.
  if (thr->stk_addr && thr->stk_size)
    MemoryResetRange(thr, /*pc=*/ 3, thr->stk_addr, thr->stk_size);
  if (thr->tls_addr && thr->tls_size)
    MemoryResetRange(thr, /*pc=*/ 4, thr->tls_addr, thr->tls_size);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = CTX()->threads[thr->tid];
  CHECK_NE(tctx, 0);
  CHECK_EQ(tctx->status, ThreadStatusRunning);
  if (tctx->detached) {
    ThreadDead(thr, tctx);
  } else {
    thr->fast_state.IncrementEpoch();
    // Can't increment epoch w/o writing to the trace as well.
    TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeMop, 0);
    thr->clock.set(thr->tid, thr->fast_state.epoch());
    thr->fast_synch_epoch = thr->fast_state.epoch();
    thr->clock.release(&tctx->sync, &thr->clockslab);
    tctx->status = ThreadStatusFinished;
  }

  // Save from info about the thread.
  // If dead_info will become dynamically allocated again,
  // it is the point to allocate it.
  // tctx->dead_info = new ThreadDeadInfo;
  internal_memcpy(&tctx->dead_info.trace.events[0],
      &thr->trace.events[0], sizeof(thr->trace.events));
  for (int i = 0; i < kTraceParts; i++)
    tctx->dead_info.trace.headers[i].stack0.CopyFrom(thr,
        thr->trace.headers[i].stack0);
  tctx->epoch1 = thr->clock.get(tctx->tid);

  if (kCollectStats) {
    for (int i = 0; i < StatCnt; i++)
      CTX()->stat[i] += thr->stat[i];
  }

  thr->~ThreadState();
  tctx->thr = 0;
}

void ThreadJoin(ThreadState *thr, uptr pc, uptr uid) {
  CHECK_GT(thr->in_rtl, 0);
  DPrintf("#%d: ThreadJoin uid=%lu\n", thr->tid, uid);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = 0;
  int tid = 0;
  for (; tid < kMaxTid; tid++) {
    if (CTX()->threads[tid] != 0
        && CTX()->threads[tid]->user_id == uid
        && CTX()->threads[tid]->status != ThreadStatusInvalid) {
      tctx = CTX()->threads[tid];
      break;
    }
  }
  if (tctx == 0 || tctx->status == ThreadStatusInvalid) {
    Printf("ThreadSanitizer: join of non-existent thread\n");
    return;
  }
  CHECK_EQ(tctx->detached, false);
  CHECK_EQ(tctx->status, ThreadStatusFinished);
  thr->clock.acquire(&tctx->sync);
  ThreadDead(thr, tctx);
}

void ThreadDetach(ThreadState *thr, uptr pc, uptr uid) {
  CHECK_GT(thr->in_rtl, 0);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = 0;
  for (int tid = 0; tid < kMaxTid; tid++) {
    if (CTX()->threads[tid] != 0
        && CTX()->threads[tid]->user_id == uid
        && CTX()->threads[tid]->status != ThreadStatusInvalid) {
      tctx = CTX()->threads[tid];
      break;
    }
  }
  if (tctx == 0 || tctx->status == ThreadStatusInvalid) {
    Printf("ThreadSanitizer: detach of non-existent thread\n");
    return;
  }
  if (tctx->status == ThreadStatusFinished) {
    ThreadDead(thr, tctx);
  } else {
    tctx->detached = true;
  }
}

void MemoryAccessRange(ThreadState *thr, uptr pc, uptr addr,
                       uptr size, bool is_write) {
  if (size == 0)
    return;

  u64 *shadow_mem = (u64*)MemToShadow(addr);
  DPrintf2("#%d: MemoryAccessRange: @%p %p size=%d is_write=%d\n",
      thr->tid, (void*)pc, (void*)addr,
      (int)size, is_write);

#if TSAN_DEBUG
  if (!IsAppMem(addr)) {
    Printf("Access to non app mem %lx\n", addr);
    DCHECK(IsAppMem(addr));
  }
  if (!IsAppMem(addr + size - 1)) {
    Printf("Access to non app mem %lx\n", addr + size - 1);
    DCHECK(IsAppMem(addr + size - 1));
  }
  if (!IsShadowMem((uptr)shadow_mem)) {
    Printf("Bad shadow addr %p (%lx)\n", shadow_mem, addr);
    DCHECK(IsShadowMem((uptr)shadow_mem));
  }
  if (!IsShadowMem((uptr)(shadow_mem + size * kShadowCnt / 8 - 1))) {
    Printf("Bad shadow addr %p (%lx)\n",
        shadow_mem + size * kShadowCnt / 8 - 1, addr + size - 1);
    DCHECK(IsShadowMem((uptr)(shadow_mem + size * kShadowCnt / 8 - 1)));
  }
#endif

  StatInc(thr, StatMopRange);

  FastState fast_state = thr->fast_state;
  if (fast_state.GetIgnoreBit())
    return;

  fast_state.IncrementEpoch();
  thr->fast_state = fast_state;
  TraceAddEvent(thr, fast_state.epoch(), EventTypeMop, pc);

  bool unaligned = (addr % kShadowCell) != 0;

  // Handle unaligned beginning, if any.
  for (; addr % kShadowCell && size; addr++, size--) {
    int const kAccessSizeLog = 0;
    Shadow cur(fast_state);
    cur.SetWrite(is_write);
    cur.SetAddr0AndSizeLog(addr & (kShadowCell - 1), kAccessSizeLog);
    MemoryAccessImpl(thr, addr, kAccessSizeLog, is_write, fast_state,
        shadow_mem, cur);
  }
  if (unaligned)
    shadow_mem += kShadowCnt;
  // Handle middle part, if any.
  for (; size >= kShadowCell; addr += kShadowCell, size -= kShadowCell) {
    int const kAccessSizeLog = 3;
    Shadow cur(fast_state);
    cur.SetWrite(is_write);
    cur.SetAddr0AndSizeLog(0, kAccessSizeLog);
    MemoryAccessImpl(thr, addr, kAccessSizeLog, is_write, fast_state,
        shadow_mem, cur);
    shadow_mem += kShadowCnt;
  }
  // Handle ending, if any.
  for (; size; addr++, size--) {
    int const kAccessSizeLog = 0;
    Shadow cur(fast_state);
    cur.SetWrite(is_write);
    cur.SetAddr0AndSizeLog(addr & (kShadowCell - 1), kAccessSizeLog);
    MemoryAccessImpl(thr, addr, kAccessSizeLog, is_write, fast_state,
        shadow_mem, cur);
  }
}

void MemoryRead1Byte(ThreadState *thr, uptr pc, uptr addr) {
  MemoryAccess(thr, pc, addr, 0, 0);
}

void MemoryWrite1Byte(ThreadState *thr, uptr pc, uptr addr) {
  MemoryAccess(thr, pc, addr, 0, 1);
}

void MemoryRead8Byte(ThreadState *thr, uptr pc, uptr addr) {
  MemoryAccess(thr, pc, addr, 3, 0);
}

void MemoryWrite8Byte(ThreadState *thr, uptr pc, uptr addr) {
  MemoryAccess(thr, pc, addr, 3, 1);
}
}  // namespace __tsan
