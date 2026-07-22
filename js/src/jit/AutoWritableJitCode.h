/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AutoWritableJitCode_h
#define jit_AutoWritableJitCode_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/TimeStamp.h"

#include <stddef.h>

#include "jit/ExecutableAllocator.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/ProcessExecutableMemory.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/Runtime.h"

#ifdef ENABLE_JS_AOT
#  include <stdint.h>
#  include <sys/mman.h>

#  include "gc/Memory.h"
#  include "jit/FlushICache.h"
#endif

namespace js::jit {

// This class ensures JIT code is executable on its destruction. Creators
// must call makeWritable(), and not attempt to write to the buffer if it fails.
//
// AutoWritableJitCodeFallible may only fail to make code writable; it cannot
// fail to make JIT code executable (because the creating code has no chance to
// recover from a failed destructor).
class MOZ_RAII AutoWritableJitCodeFallible {
  JSRuntime* rt_;
  void* addr_;
  size_t size_;
#ifdef ENABLE_JS_AOT
  bool isStatic_;
#endif
  AutoMarkJitCodeWritableForThread writableForThread_;

#ifdef ENABLE_JS_AOT
  // Static AOT code lives in the binary's r-x .text pages rather than
  // the JIT pool, so ExecutableAllocator::makeWritable would assert.
  // Toggle protection with mprotect over the page range covering
  // [addr, addr + size). This CoWs the page on first write; sharing
  // across processes is lost until the process exits.
  [[nodiscard]] static bool StaticMprotect(void* addr, size_t size,
                                           bool writable) {
    size_t page = gc::SystemPageSize();
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t aligned = start & ~(page - 1);
    size_t len = ((size + (start - aligned)) + page - 1) & ~(page - 1);
    int prot = writable ? (PROT_READ | PROT_WRITE) : (PROT_READ | PROT_EXEC);
    return mprotect(reinterpret_cast<void*>(aligned), len, prot) == 0;
  }
#endif

 public:
  explicit AutoWritableJitCodeFallible(JitCode* code)
      : rt_(code->runtimeFromMainThread()),
        addr_(code->allocatedMemory()),
        size_(code->allocatedSize()),
#ifdef ENABLE_JS_AOT
        isStatic_(code->isStaticCode()),
#endif
        writableForThread_() {
    rt_->toggleAutoWritableJitCodeActive(true);
  }

  [[nodiscard]] bool makeWritable() {
#ifdef ENABLE_JS_AOT
    if (isStatic_) {
      return StaticMprotect(addr_, size_, true);
    }
#endif
    return ExecutableAllocator::makeWritable(addr_, size_);
  }

  ~AutoWritableJitCodeFallible() {
    bool ok;
#ifdef ENABLE_JS_AOT
    if (isStatic_) {
      jit::FlushICache(addr_, size_);
      ok = StaticMprotect(addr_, size_, false);
    } else
#endif
    {
      ok = ExecutableAllocator::makeExecutableAndFlushICache(addr_, size_);
    }
    if (!ok) {
      MOZ_CRASH();
    }
    rt_->toggleAutoWritableJitCodeActive(false);
  }
};

// Infallible variant of AutoWritableJitCodeFallible, ensures writable during
// construction
class MOZ_RAII AutoWritableJitCode : private AutoWritableJitCodeFallible {
 public:
  explicit AutoWritableJitCode(JitCode* code)
      : AutoWritableJitCodeFallible(code) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!makeWritable()) {
      oomUnsafe.crash("Failed to mmap. Likely no mappings available.");
    }
  }
};

}  // namespace js::jit

#endif /* jit_AutoWritableJitCode_h */
