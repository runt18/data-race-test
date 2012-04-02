//===-- tsan_mman.h ---------------------------------------------*- C++ -*-===//
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
#ifndef TSAN_MMAN_H
#define TSAN_MMAN_H

#include "tsan_defs.h"

namespace __tsan {

// Descriptor of user's memory block.
struct MBlock {
  uptr size;
};

// For user allocations.
void *user_alloc(ThreadState *thr, uptr pc, uptr sz);
void user_free(ThreadState *thr, uptr pc, void *p);  // Does not accept NULL.
void *user_realloc(ThreadState *thr, uptr pc, void *p, uptr sz);
// Given the pointer p into a valid allocated block,
// returns the descriptor of the block.
MBlock *user_mblock(ThreadState *thr, void *p);

// For internal data structures.
void *internal_alloc(ThreadState *thr, uptr sz);
void internal_free(ThreadState *thr, void *p);

}  // namespace __tsan

#endif  // TSAN_MMAN_H
