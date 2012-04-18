//===-- tsan_flags_test.cc --------------------------------------*- C++ -*-===//
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
#include "tsan_flags.h"
#include "tsan_rtl.h"
#include "gtest/gtest.h"

namespace __tsan {

TEST(Flags, Basic) {
  // At least should not crash.
  Flags f = {};
  InitializeFlags(&f, 0);
  InitializeFlags(&f, "");
}

TEST(Flags, ParseBool) {
  ScopedInRtl in_rtl;
  Flags f = {};

  f.enable_annotations = false;
  InitializeFlags(&f, "enable_annotations");
  EXPECT_EQ(f.enable_annotations, true);

  f.enable_annotations = false;
  InitializeFlags(&f, "--enable_annotations");
  EXPECT_EQ(f.enable_annotations, true);

  f.enable_annotations = false;
  InitializeFlags(&f, "--enable_annotations=1");
  EXPECT_EQ(f.enable_annotations, true);

  f.enable_annotations = true;
  InitializeFlags(&f, "asdas enable_annotations=0 asdasd");
  EXPECT_EQ(f.enable_annotations, false);

  f.enable_annotations = true;
  InitializeFlags(&f, "   --enable_annotations=0   ");
  EXPECT_EQ(f.enable_annotations, false);
}

TEST(Flags, ParseInt) {
  ScopedInRtl in_rtl;
  Flags f = {};

  f.exit_status = -11;
  InitializeFlags(&f, "exit_status");
  EXPECT_EQ(f.exit_status, 0);

  f.exit_status = -11;
  InitializeFlags(&f, "--exit_status=");
  EXPECT_EQ(f.exit_status, 0);

  f.exit_status = -11;
  InitializeFlags(&f, "--exit_status=42");
  EXPECT_EQ(f.exit_status, 42);

  f.exit_status = -11;
  InitializeFlags(&f, "--exit_status=-42");
  EXPECT_EQ(f.exit_status, -42);
}

TEST(Flags, ParseStr) {
  ScopedInRtl in_rtl;
  Flags f = {};

  InitializeFlags(&f, 0);
  EXPECT_EQ(0, strcmp(f.strip_path_prefix, ""));

  InitializeFlags(&f, "strip_path_prefix");
  EXPECT_EQ(0, strcmp(f.strip_path_prefix, ""));

  InitializeFlags(&f, "--strip_path_prefix=");
  EXPECT_EQ(0, strcmp(f.strip_path_prefix, ""));

  InitializeFlags(&f, "--strip_path_prefix=abc");
  EXPECT_EQ(0, strcmp(f.strip_path_prefix, "abc"));

  InitializeFlags(&f, "--strip_path_prefix='abc zxc'");
  EXPECT_EQ(0, strcmp(f.strip_path_prefix, "abc zxc"));

  InitializeFlags(&f, "--strip_path_prefix=\"abc zxc\"");
  EXPECT_EQ(0, strcmp(f.strip_path_prefix, "abc zxc"));
}

}  // namespace __tsan