/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AutoAOTCodegen_h
#define jit_AutoAOTCodegen_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Attributes.h"

#  include "jit/JitRuntime.h"
#  include "jit/MacroAssembler.h"
#  include "vm/JSContext.h"
#  include "vm/Runtime.h"

namespace js::jit {

// [SMDOC] AutoAOTCodegen
//
// This scope enables AOT capture for a macro assembler. It installs the runtime
// indirection table when created and restores the previous state when
// destroyed. While active, runtime pointers are emitted through the indirection
// table. The scope cannot be nested and must be created before code emission
// begins.
class MOZ_STACK_CLASS AutoAOTCodegen {
 public:
  AutoAOTCodegen(MacroAssembler& masm, JSContext* cx) : masm_(masm) {
    masm_.setAOTTable(&cx->runtime()->jitRuntime()->aotIndirectionTable());
  }

  ~AutoAOTCodegen() { masm_.setAOTTable(nullptr); }

  AutoAOTCodegen(const AutoAOTCodegen&) = delete;
  AutoAOTCodegen& operator=(const AutoAOTCodegen&) = delete;

 private:
  MacroAssembler& masm_;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AutoAOTCodegen_h
