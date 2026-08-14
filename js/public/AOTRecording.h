/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_AOTRecording_h
#define js_AOTRecording_h

#ifdef ENABLE_JS_AOT

#  include "jstypes.h"

struct JSContext;

namespace JS {

// If JitOptions.aotRecordSelfHosted is set and an AOT recorder is armed on
// this runtime, sweep the entire self-host stencil and force baseline
// compilation of every self-hosted function so the recorder captures a full
// blfun set. No-op otherwise. Embedders call this once after the JS runtime
// is fully initialized; safe to call more than once (the recorder dedups via
// O_EXCL).
extern JS_PUBLIC_API bool MaybeRecordAOTSelfHostedBaselineCorpus(JSContext* cx);

}  // namespace JS

#endif  // ENABLE_JS_AOT

#endif  // js_AOTRecording_h
