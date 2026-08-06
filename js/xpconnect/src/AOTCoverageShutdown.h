/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef xpc_AOTCoverageShutdown_h
#define xpc_AOTCoverageShutdown_h

#ifdef ENABLE_JS_AOT

#  include "nsIObserver.h"

namespace mozilla {

// Flushes the AOT coverage dump on XPCOM shutdown. Content processes can be
// torn down without ever destroying their runtime, so relying on the engine
// side alone loses those dumps entirely.
class AOTCoverageShutdown final : public nsIObserver {
  ~AOTCoverageShutdown() = default;

 public:
  AOTCoverageShutdown() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  static void Register();
};

}  // namespace mozilla

#endif  // ENABLE_JS_AOT

#endif  // xpc_AOTCoverageShutdown_h
