//===-- tsan_rtl.cc ---------------------------------------------*- C++ -*-===//
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
// Main file (entry points) for the TSan run-time.
//===----------------------------------------------------------------------===//

#include "tsan_platform.h"
#include "tsan_rtl.h"
#include "tsan_interface.h"
#include "tsan_atomic.h"
#include "tsan_placement_new.h"
#include "tsan_suppressions.h"
#include "tsan_symbolize.h"
#include "tsan_sync.h"
#include "tsan_report.h"

namespace __tsan {

__thread char cur_thread_placeholder[sizeof(ThreadState)] ALIGN(64);
static char ctx_placeholder[sizeof(Context)] ALIGN(64);
static ReportDesc g_report;

// Freed memory.
// As if 8-byte write by thread 0xff..f at epoch 0xff..f, races with everything.
const u64 kShadowFreed = 0xfffffffffffffff8ull;

// Shadow:
//   tid             : kTidBits
//   epoch           : kClkBits
//   is_write        : 1
//   size_log        : 2
//   addr0           : 3
class Shadow: public FastState {
 public:
  explicit Shadow(u64 x) : FastState(x) { }

  explicit Shadow(const FastState &s) : FastState(s.x_) { }

  template<unsigned kAccessSizeLog>
  void SetAddr0AndSizeLog(u64 addr0) {
    DCHECK((x_ & 31) == 0);  // NOLINT
    DCHECK(addr0 <= 7);  // NOLINT
    DCHECK(kAccessSizeLog <= 3);  // NOLINT
    x_ |= (kAccessSizeLog << 3) | addr0;
    DCHECK(kAccessSizeLog == size_log());  // NOLINT
    DCHECK(addr0 == this->addr0());  // NOLINT
  }

  template<unsigned kAccessIsWrite>
  void SetWrite() {
    DCHECK((x_ & 32) == 0);  // NOLINT
    if (kAccessIsWrite)
      x_ |= 32;
    DCHECK(kAccessIsWrite == is_write());
  }

  u64 addr0() const { return x_ & 7; }
  u64 size() const { return 1ull << size_log(); }
  bool is_write() const { return x_ & 32; }
  bool IsZero() const { return x_ == 0; }
  u64 raw() const { return x_; }

  static inline bool TidsAreEqual(Shadow s1, Shadow s2) {
    u64 shifted_xor = (s1.x_ ^ s2.x_) >> (64 - kTidBits);
    DCHECK((shifted_xor == 0) == (s1.tid() == s2.tid()));  // NOLINT
    return shifted_xor == 0;
  }
  static inline bool Addr0AndSizeAreEqual(Shadow s1, Shadow s2) {
    u64 masked_xor = (s1.x_ ^ s2.x_) & 31;
    return masked_xor == 0;
  }

  template<unsigned kS2AccessSize>
  static inline bool TwoRangesIntersect(Shadow s1, Shadow s2) {
    u64 diff = s1.addr0() - s2.addr0();
    if ((s64)diff < 0) {  // s1.addr0 < s2.addr0  // NOLINT
      // if (s1.addr0() + size1) > s2.addr0()) return true;
      if (s1.size() > -diff)  return true;
    } else {
      // if (s2.addr0() + kS2AccessSize > s1.addr0()) return true;
      if (kS2AccessSize > diff) return true;
    }
    return false;
  }

  // The idea behind the offset is as follows.
  // Consider that we have 8 bool's contained within a single 8-byte block
  // (mapped to a single shadow "cell"). Now consider that we write to the bools
  // from a single thread (which we consider the common case).
  // W/o offsetting each access will have to scan 4 shadow values at average
  // to find the corresponding shadow value for the bool.
  // With offsetting we start scanning shadow with the offset so that
  // each access hits necessary shadow straight off (at least in an expected
  // optimistic case).
  // This logic works seamlessly for any layout of user data. For example,
  // if user data is {int, short, char, char}, then accesses to the int are
  // offsetted to 0, short - 4, 1st char - 6, 2nd char - 7. Hopefully, accesses
  // from a single thread won't need to scan all 8 shadow values.
  template<unsigned kAccessSize>
  unsigned ComputeSearchOffset() {
    return x_ & 7;
  }

 private:
  u64 size_log() const { return (x_ >> 3) & 3; }
};

static Context *ctx;
Context *CTX() { return ctx; }

void CheckFailed(const char *file, int line, const char *cond) {
  Report("FATAL: ThreadSanitizer CHECK failed: %s:%d \"%s\"\n",
         file, line, cond);
  Die();
}

Context::Context()
  : initialized()
  , clockslab(SyncClock::kChunkSize)
  , syncslab(sizeof(SyncVar))
  , report_mtx(StatMtxReport)
  , nreported()
  , thread_mtx(StatMtxThreads) {
}

ThreadState::ThreadState(Context *ctx, int tid, u64 epoch,
                         uptr stk_addr, uptr stk_size,
                         uptr tls_addr, uptr tls_size)
  : fast_state(tid, epoch)
  // Do not touch these, rely on zero initialization,
  // they may be accessed before the ctor.
  // , fast_ignore_reads()
  // , fast_ignore_writes()
  , clockslab(&ctx->clockslab)
  , syncslab(&ctx->syncslab)
  , tid(tid)
  , in_rtl()
  , func_call_count()
  , stk_addr(stk_addr)
  , stk_size(stk_size)
  , tls_addr(tls_addr)
  , tls_size(tls_size) {
}

ThreadContext::ThreadContext(int tid)
  : tid(tid)
  , thr()
  , status(ThreadStatusInvalid)
  , uid()
  , detached()
  , reuse_count()
  , epoch0()
  , dead_next() {
}

void Initialize(ThreadState *thr) {
  // Thread safe because done before all threads exist.
  static bool is_initialized = false;
  if (is_initialized)
    return;
  is_initialized = true;
  ScopedInRtl in_rtl;
  InitializeInterceptors();
  InitializePlatform();
  InitializeDynamicAnnotations();
  ctx = new(ctx_placeholder) Context;
  InitializeShadowMemory();
  ctx->dead_list_size = 0;
  ctx->dead_list_head = 0;
  ctx->dead_list_tail = 0;
  InitializeSuppressions();

  // Initialize thread 0.
  ctx->thread_seq = 0;
  int tid = ThreadCreate(thr, 0, 0, true);
  CHECK_EQ(tid, 0);
  ThreadStart(thr, tid);
  thr->in_rtl++;  // ThreadStart() resets it to zero.
  ctx->initialized = true;
}

int Finalize(ThreadState *thr) {
  // FIXME: Output leaked (non-joined, non-detached) threads.

  if (ctx->nreported)
    Printf("ThreadSanitizer summary: reported %d warnings\n", ctx->nreported);

  if (kCollectStats) {
    for (int i = 0; i < StatCnt; i++)
      ctx->stat[i] += thr->stat[i];
    PrintStats(ctx->stat);
  }

  return ctx->nreported ? 66 : 0;
}

static void TraceSwitch(ThreadState *thr) {
  Lock l(&thr->trace.mtx);
  unsigned trace = (thr->fast_state.epoch() / kTracePartSize) % kTraceParts;
  thr->trace.headers[trace].epoch0 = thr->fast_state.epoch();
}

extern "C" void __tsan_trace_switch() {
  TraceSwitch(cur_thread());
}

static int RestoreStack(int tid, const u64 epoch, uptr *stack, int n) {
  Lock l0(&ctx->thread_mtx);
  ThreadContext *tctx = ctx->threads[tid];
  if (tctx == 0)
    return 0;
  Trace* trace = 0;
  if (tctx->status == ThreadStatusRunning) {
    CHECK(tctx->thr);
    trace = &tctx->thr->trace;
  } else if (tctx->status == ThreadStatusFinished
      || tctx->status == ThreadStatusDead) {
    trace = &tctx->dead_info.trace;
  } else {
    return 0;
  }
  Lock l(&trace->mtx);
  const int partidx = (epoch / (kTraceSize / kTraceParts)) % kTraceParts;
  TraceHeader* hdr = &trace->headers[partidx];
  if (epoch < hdr->epoch0)
    return 0;
  u64 pos = 0;
  const u64 eend = epoch % kTraceSize;
  const u64 ebegin = eend / kTracePartSize * kTracePartSize;
  DPrintf("#%d: RestoreStack epoch=%llu ebegin=%llu eend=%llu partidx=%d\n",
      tid, epoch, ebegin, eend, partidx);
  for (u64 i = ebegin; i <= eend; i++) {
    Event ev = trace->events[i];
    EventType typ = (EventType)(ev >> 61);
    uptr pc = (uptr)(ev & 0xffffffffffffull);
    DPrintf2("  %04llu typ=%d pc=%p\n", i, typ, pc);
    if (typ == EventTypeMop) {
      stack[pos] = pc;
    } else if (typ == EventTypeFuncEnter) {
      // We obtain the return address, that is, address of the next instruction,
      // so offset it by 1 byte.
      stack[pos++] = pc - 1;
    } else if (typ == EventTypeFuncExit) {
      if (pos > 0)
        pos--;
    }
  }
  if (pos == 0 && stack[0] == 0)
    return 0;
  pos++;
  for (u64 i = 0; i < pos / 2; i++) {
    uptr pc = stack[i];
    stack[i] = stack[pos - i - 1];
    stack[pos - i - 1] = pc;
  }
  return pos;
}

static void NOINLINE ReportRace(ThreadState *thr) {
  const int kStackMax = 64;

  ScopedInRtl in_rtl;
  uptr addr = thr->racy_addr & ~7;
  {
    uptr a0 = addr + Shadow(thr->racy_state[0]).addr0();
    uptr a1 = addr + Shadow(thr->racy_state[1]).addr0();
    uptr e0 = a0 + Shadow(thr->racy_state[0]).size();
    uptr e1 = a1 + Shadow(thr->racy_state[1]).size();
    uptr minaddr = min(a0, a1);
    uptr maxaddr = max(e0, e1);
    if (IsExpectReport(minaddr, maxaddr - minaddr))
      return;
  }

  Lock l(&ctx->report_mtx);
  RegionAlloc alloc(g_report.alloc, sizeof(g_report.alloc));
  ReportDesc &rep = g_report;
  rep.typ = ReportTypeRace;
  rep.nmop = 2;
  if (thr->racy_state[1] == kShadowFreed)
    rep.nmop = 1;
  rep.mop = alloc.Alloc<ReportMop>(rep.nmop);
  for (int i = 0; i < rep.nmop; i++) {
    ReportMop *mop = &rep.mop[i];
    Shadow s(thr->racy_state[i]);
    mop->tid = s.tid();
    mop->addr = addr + s.addr0();
    mop->size = s.size();
    mop->write = s.is_write();
    mop->nmutex = 0;
    mop->stack.cnt = 0;
    uptr stack[kStackMax] = {};
    int stackcnt = RestoreStack(s.tid(), s.epoch(), stack, kStackMax);
    // Ensure that we have at least something for the current thread.
    CHECK(i != 0 || stackcnt != 0);
    if (stackcnt != 0) {
      mop->stack.entry = alloc.Alloc<ReportStackEntry>(kStackMax);
      for (int si = 0; si < stackcnt; si++) {
        Symbol symb[kStackMax];
        int framecnt = SymbolizeCode(&alloc, stack[si], symb, kStackMax);
        if (framecnt) {
          for (int fi = 0; fi < framecnt && mop->stack.cnt < kStackMax; fi++) {
            ReportStackEntry *ent = &mop->stack.entry[mop->stack.cnt++];
            ent->pc = stack[si];
            ent->func = symb[fi].name;
            ent->file = symb[fi].file;
            ent->line = symb[fi].line;
          }
        } else if (mop->stack.cnt < kStackMax) {
          ReportStackEntry *ent = &mop->stack.entry[mop->stack.cnt++];
          ent->pc = stack[si];
          ent->func = 0;
          ent->file = 0;
          ent->line = 0;
        }
      }
      // Strip frame above 'main'
      if (mop->stack.cnt >= 2 && internal_strcmp(
          mop->stack.entry[mop->stack.cnt - 2].func, "main") == 0) {
        mop->stack.cnt--;
      // Strip our internal thread start routine.
      } else if (mop->stack.cnt >= 2 && internal_strcmp(
          mop->stack.entry[mop->stack.cnt - 1].func,
          "__tsan_thread_start_func") == 0) {
        mop->stack.cnt--;
      } else {
        // Ensure that we recovered stack completely. Trimmed stack
        // can actually happen if we do not instrument some code,
        // so it's only a DCHECK. However we must try hard to not miss it
        // due to our fault.
        Printf("Top stack frame (main or __tsan::ThreadStartFunc) missed %d\n",
            i);
      }
    }
  }
  rep.loc = 0;
  rep.nthread = 0;
  rep.nmutex = 0;
  bool suppressed = IsSuppressed(ReportTypeRace, &rep.mop[0].stack);
  suppressed = OnReport(&rep, suppressed);
  if (suppressed)
    return;
  PrintReport(&rep);
  ctx->nreported++;
}

extern "C" void __tsan_report_race() {
  ReportRace(cur_thread());
}

ALWAYS_INLINE
static Shadow LoadShadow(u64 *p) {
  u64 raw = atomic_load((atomic_uint64_t*)p, memory_order_relaxed);
  return Shadow(raw);
}

ALWAYS_INLINE
static void StoreShadow(u64 *sp, u64 s) {
  atomic_store((atomic_uint64_t*)sp, s, memory_order_relaxed);
}

ALWAYS_INLINE
static void StoreIfNotYetStored(u64 *sp, u64 *s) {
  StoreShadow(sp, *s);
  *s = 0;
}

static inline void HandleRace(ThreadState *thr, uptr addr,
                              Shadow cur, Shadow old) {
  thr->racy_state[0] = cur.raw();
  thr->racy_state[1] = old.raw();
  thr->racy_addr     = addr;
  HACKY_CALL(__tsan_report_race);
}

template<int kAccessSizeLog, int kAccessIsWrite>
ALWAYS_INLINE
static bool MemoryAccess1(ThreadState *thr, uptr addr,
                          Shadow cur, u64 *shadow_mem, unsigned i,
                          u64 &store_state) {
  StatInc(thr, StatShadowProcessed);
  const unsigned kAccessSize = 1 << kAccessSizeLog;
  unsigned off = cur.ComputeSearchOffset<kAccessSize>();
  u64 *sp = &shadow_mem[(i + off) % kShadowCnt];
  Shadow old = LoadShadow(sp);
  if (old.IsZero()) {
    StatInc(thr, StatShadowZero);
    if (store_state)
      StoreIfNotYetStored(sp, &store_state);
    return false;
  }
  // is the memory access equal to the previous?
  if (Shadow::Addr0AndSizeAreEqual(cur, old)) {
    StatInc(thr, StatShadowSameSize);
    // same thread?
    if (Shadow::TidsAreEqual(old, cur)) {
      StatInc(thr, StatShadowSameThread);
      if (old.epoch() >= thr->fast_synch_epoch) {
        if (old.is_write() || !kAccessIsWrite) {
          // found a slot that holds effectively the same info
          // (that is, same tid, same sync epoch and same size)
          StatInc(thr, StatMopSame);
          return true;
        } else {
          StoreIfNotYetStored(sp, &store_state);
          return false;
        }
      } else {
        if (!old.is_write() || kAccessIsWrite) {
          StoreIfNotYetStored(sp, &store_state);
          return false;
        } else {
          return false;
        }
      }
    } else {
      StatInc(thr, StatShadowAnotherThread);
      // happens before?
      if (thr->clock.get(old.tid()) >= old.epoch()) {
        StoreIfNotYetStored(sp, &store_state);
        return false;
      } else if (!old.is_write() && !kAccessIsWrite) {
        return false;
      } else {
        HandleRace(thr, addr, cur, old);
        return true;
      }
    }
  // Do the memory access intersect?
  } else if (Shadow::TwoRangesIntersect<kAccessSize>(old, cur)) {
    StatInc(thr, StatShadowIntersect);
    if (Shadow::TidsAreEqual(old, cur)) {
      StatInc(thr, StatShadowSameThread);
      return false;
    }
    StatInc(thr, StatShadowAnotherThread);
    // happens before?
    if (thr->clock.get(old.tid()) >= old.epoch()) {
      return false;
    } else if (!old.is_write() && !kAccessIsWrite) {
      return false;
    } else {
      HandleRace(thr, addr, cur, old);
      return true;
    }
  // The accesses do not intersect.
  } else {
    StatInc(thr, StatShadowNotIntersect);
  }
  return false;
}

template<u64 kAccessSizeLog, bool kAccessIsWrite>
ALWAYS_INLINE
void MemoryAccess(ThreadState *thr, uptr pc, uptr addr) {
  CHECK_LE(kAccessIsWrite, 1);
  if ((kAccessIsWrite && thr->fast_ignore_writes)
      || (!kAccessIsWrite && thr->fast_ignore_reads))
    return;
  u64 *shadow_mem = (u64*)MemToShadow(addr);
  DPrintf2("#%d: tsan::OnMemoryAccess: @%p %p size=%d"
      " is_write=%d shadow_mem=%p {%p, %p, %p, %p}\n",
      (int)thr->fast_state.tid(), (void*)pc, (void*)addr,
      (int)(1 << kAccessSizeLog), kAccessIsWrite, shadow_mem,
      shadow_mem[0], shadow_mem[1], shadow_mem[2], shadow_mem[3]);
  DCHECK(IsAppMem(addr));
  DCHECK(IsShadowMem((uptr)shadow_mem));

  StatInc(thr, StatMop);
  StatInc(thr, kAccessIsWrite ? StatMopWrite : StatMopRead);
  StatInc(thr, (StatType)(StatMop1 + kAccessSizeLog));

  thr->fast_state.IncrementEpoch();
  Shadow cur(thr->fast_state);
  cur.SetAddr0AndSizeLog<kAccessSizeLog>(addr & 7);
  cur.SetWrite<kAccessIsWrite>();
  // This potentially can live in an MMX/SSE scratch register.
  // The required intrinsics are:
  // __m128i _mm_move_epi64(__m128i*);
  // _mm_storel_epi64(u64*, __m128i);
  u64 store_state = cur.raw();

  // We must not store to the trace if we do not store to the shadow.
  // That is, this call must be moved somewhere below.
  TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeMop, pc);

  // scan all the shadow values and dispatch to 4 categories:
  // same, replace, candidate and race (see comments below).
  // we consider only 3 cases regarding access sizes:
  // equal, intersect and not intersect. initially I considered
  // larger and smaller as well, it allowed to replace some
  // 'candidates' with 'same' or 'replace', but I think
  // it's just not worth it (performance- and complexity-wise).

#define MEM_ACCESS_ITER(i) \
    if (MemoryAccess1<kAccessSizeLog, kAccessIsWrite>( \
        thr, addr, cur, shadow_mem, i, store_state)) \
      return;
  if (kShadowCnt == 1) {
    MEM_ACCESS_ITER(0);
  } else if (kShadowCnt == 2) {
    MEM_ACCESS_ITER(0);
    MEM_ACCESS_ITER(1);
  } else if (kShadowCnt == 4) {
    MEM_ACCESS_ITER(0);
    MEM_ACCESS_ITER(1);
    MEM_ACCESS_ITER(2);
    MEM_ACCESS_ITER(3);
  } else if (kShadowCnt == 8) {
    MEM_ACCESS_ITER(0);
    MEM_ACCESS_ITER(1);
    MEM_ACCESS_ITER(2);
    MEM_ACCESS_ITER(3);
    MEM_ACCESS_ITER(4);
    MEM_ACCESS_ITER(5);
    MEM_ACCESS_ITER(6);
    MEM_ACCESS_ITER(7);
  } else {
    CHECK(false);
  }
#undef MEM_ACCESS_ITER

  // we did not find any races and had already stored
  // the current access info, so we are done
  if (LIKELY(store_state == 0))
    return;
  // choose a random candidate slot and replace it
  unsigned i = cur.epoch() % kShadowCnt;
  StoreShadow(shadow_mem+i, store_state);
  StatInc(thr, StatShadowReplace);
}

template<int kAccessIsWrite>
static void MemoryAccessRange(ThreadState *thr, uptr pc, uptr addr,
                              uptr size) {
  // Handle unaligned beginning, if any.
  for (; addr % 8 && size; addr++, size--) {
    MemoryAccess<0, kAccessIsWrite>(thr, pc, addr);
  }
  // Handle middle part, if any.
  for (; size >= 8; addr += 8, size -= 8) {
    StatInc(thr, StatMopRange);
    MemoryAccess<3, kAccessIsWrite>(thr, pc, addr);
  }
  // Handle ending, if any.
  for (; size; addr++, size--) {
    MemoryAccess<0, kAccessIsWrite>(thr, pc, addr);
  }
}

void MemoryAccessRange(ThreadState *thr, uptr pc, uptr addr,
                       uptr size, bool is_write) {
  if (is_write)
    MemoryAccessRange<1>(thr, pc, addr, size);
  else
    MemoryAccessRange<0>(thr, pc, addr, size);
}

void MemoryRead1Byte(ThreadState *thr, uptr pc, uptr addr) {
  MemoryAccess<0, 0>(thr, pc, addr);
}

void MemoryWrite1Byte(ThreadState *thr, uptr pc, uptr addr) {
  MemoryAccess<0, 1>(thr, pc, addr);
}

static void MemoryRangeSet(ThreadState *thr, uptr pc, uptr addr, uptr size,
                           u64 val) {
  CHECK_EQ(addr % 8, 0);
  (void)thr;
  (void)pc;
  // Some programs mmap like hundreds of GBs but actually used a small part.
  // So, it's better to report a false positive on the memory
  // then to hang here senselessly.
  const uptr kMaxResetSize = 1024*1024*1024;
  if (size > kMaxResetSize)
    size = kMaxResetSize;
  size = (size + 7) & ~7;
  u64 *p = (u64*)MemToShadow(addr);
  // FIXME: may overwrite a part outside the region
  for (uptr i = 0; i < size; i++)
    p[i] = val;
}

void MemoryResetRange(ThreadState *thr, uptr pc, uptr addr, uptr size) {
  MemoryRangeSet(thr, pc, addr, size, 0);
}

void MemoryRangeFreed(ThreadState *thr, uptr pc, uptr addr, uptr size) {
  MemoryAccessRange(thr, pc, addr, size, true);
  MemoryRangeSet(thr, pc, addr, size, kShadowFreed);
}

void FuncEntry(ThreadState *thr, uptr pc) {
  DCHECK_EQ(thr->in_rtl, 0);
  StatInc(thr, StatFuncEnter);
  DPrintf2("#%d: tsan::FuncEntry %p\n", (int)thr->fast_state.tid(), (void*)pc);
  thr->fast_state.IncrementEpoch();
  TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeFuncEnter, pc);

#if 1
  // While we are testing on single-threaded benchmarks,
  // emulate some synchronization activity.
  // FIXME: remove me later.
  if (((++thr->func_call_count) % 1000) == 0) {
    thr->clock.set(thr->fast_state.tid(), thr->fast_state.epoch());
    thr->fast_synch_epoch = thr->fast_state.epoch();
  }
#endif
}

void FuncExit(ThreadState *thr) {
  DCHECK_EQ(thr->in_rtl, 0);
  StatInc(thr, StatFuncExit);
  DPrintf2("#%d: tsan::FuncExit\n", (int)thr->fast_state.tid());
  thr->fast_state.IncrementEpoch();
  TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeFuncExit, 0);
}

void IgnoreCtl(ThreadState *thr, bool write, bool begin) {
  DPrintf("#%d: IgnoreCtl(%d, %d)\n", thr->tid, write, begin);
  int* p = write ? &thr->fast_ignore_writes : &thr->fast_ignore_reads;
  *p += begin ? 1 : -1;
  CHECK_GE(*p, 0);
}

}  // namespace __tsan

// Must be included in this file to make sure everything is inlined.
#include "tsan_interface_inl.h"