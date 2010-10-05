#!/bin/bash
#

PASS_SO="$SCRIPT_ROOT/../opt/ThreadSanitizer/ThreadSanitizer.so"
LLVM_GCC=llvm-gcc
LLVM_GPP=llvm-g++
LD=g++
OPT=opt
LLC=llc
TSAN_RTL="$SCRIPT_ROOT/../tsan_rtl/tsan_rtl32.a"
LINK_DBG="$SCRIPT_ROOT/link_debuginfo.sh"

