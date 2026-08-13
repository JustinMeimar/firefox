/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_AOTTiming_h
#define js_AOTTiming_h

#ifdef ENABLE_JS_AOT

#  include "jstypes.h"

namespace JS {

extern JS_PUBLIC_API void FlushAOTTiming(const char* processType);

}  // namespace JS

#endif  // ENABLE_JS_AOT

#endif  // js_AOTTiming_h
