/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AutoAOTCodegen_h
#define jit_AutoAOTCodegen_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Attributes.h"

#  include "jit/CompileWrappers.h"
#  include "jit/JitRuntime.h"
#  include "jit/MacroAssembler.h"
#  include "vm/JSContext.h"
#  include "vm/Runtime.h"

namespace js::jit {

// [SMDOC] AutoAOTCodegen
//
// RAII scope that switches a MacroAssembler into AOT capture mode. On
// construction it installs the JitRuntime's AOTIndirectionTable and the
// capture Zone on the masm; on destruction it uninstalls them. While alive,
// masm.isAOT() is true and pointer overrides emit relocatable operands. The
// capture Zone is only used to classify operands.
//
//   StackMacroAssembler masm(cx, temp);
//   AutoAOTCodegen aot(masm, cx);
//   // ... codegen ...
//
// Nesting is disallowed and the scope must be installed before any code
// is emitted; both are enforced by MacroAssembler::setAOTTable.
class MOZ_STACK_CLASS AutoAOTCodegen {
 public:
  AutoAOTCodegen(MacroAssembler& masm, JSContext* cx)
      : AutoAOTCodegen(masm, cx, cx->zone()) {}

  AutoAOTCodegen(MacroAssembler& masm, JSContext* cx, JS::Zone* zone)
      : masm_(masm) {
    MOZ_ASSERT(zone);
    masm_.setAOTTable(&cx->runtime()->jitRuntime()->aotIndirectionTable(),
                      CompileZone::get(zone));
  }

  ~AutoAOTCodegen() { masm_.setAOTTable(nullptr, nullptr); }

  AutoAOTCodegen(const AutoAOTCodegen&) = delete;
  AutoAOTCodegen& operator=(const AutoAOTCodegen&) = delete;

 private:
  MacroAssembler& masm_;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AutoAOTCodegen_h
