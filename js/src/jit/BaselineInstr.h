/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineInstr_h
#define jit_BaselineInstr_h

#include <cstdint>

#include "vm/JSScript.h"

namespace js::jit {

// Cheap probe hash on the JSScript's shared bytecode data. Suitable as
// a first-level fingerprint but not as a canonical identity, since two
// scripts with identical bytecode may still differ in flags or shape.
uint32_t ComputeBaselineProbeHash(JSScript* script);

// Emit a `baseline-compile` line to the JSInstr_Baseline channel when
// enabled. Fires per successful baseline compile so
// `JS_INSTR=baseline` yields per-workload frequency logs suitable for
// the phase-2 preliminary evaluations. Cheap no-op when the channel is
// off.
void EmitBaselineCompileEvent(JSContext* cx, JSScript* script);

}  // namespace js::jit

#endif  // jit_BaselineInstr_h
