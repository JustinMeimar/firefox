/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AutoAOTCodegen_h
#define jit_AutoAOTCodegen_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Attributes.h"

#  include "jit/AOT.h"
#  include "jit/JitRuntime.h"
#  include "jit/MacroAssembler.h"
#  include "vm/JSContext.h"
#  include "vm/Runtime.h"

namespace js::jit {

// [SMDOC] AutoAOTCodegen
//
// RAII scope that switches a MacroAssembler into AOT capture mode. On ctor
// it installs a scope-owned AOTContext onto the masm; on dtor it uninstalls.
// While the scope is alive, masm.isAOT() is true and ImmPtr overrides route
// runtime pointers through the AOT indirection table.
//
//   StackMacroAssembler masm(cx, temp);
//   AutoAOTCodegen aot(masm, cx);
//   // ... codegen ...
//
// Nested scopes are disallowed. The scope must be installed before any code
// is emitted (asserted).
class MOZ_STACK_CLASS AutoAOTCodegen {
 public:
  AutoAOTCodegen(MacroAssembler& masm, JSContext* cx)
      : masm_(masm),
        ctx_(&cx->runtime()->jitRuntime()->aotIndirectionTable()) {
    masm_.setAOTContext(&ctx_);
  }

  ~AutoAOTCodegen() { masm_.setAOTContext(nullptr); }

  AutoAOTCodegen(const AutoAOTCodegen&) = delete;
  AutoAOTCodegen& operator=(const AutoAOTCodegen&) = delete;

 private:
  MacroAssembler& masm_;
  AOTContext ctx_;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AutoAOTCodegen_h
