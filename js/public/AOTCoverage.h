/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_AOTCoverage_h
#define js_AOTCoverage_h

#ifdef ENABLE_JS_AOT

#  include "jstypes.h"

namespace JS {

// Write this process's AOT corpus coverage dump, if coverage is armed. No
// runtime is needed and repeat calls are harmless: the dump is rewritten in
// full each time, so the embedder can flush from whichever teardown path it
// reaches first.
extern JS_PUBLIC_API void FlushAOTCoverage();

}  // namespace JS

#endif  // ENABLE_JS_AOT

#endif  // js_AOTCoverage_h
