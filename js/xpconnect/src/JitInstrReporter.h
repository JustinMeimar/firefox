/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef xpc_JitInstrReporter_h
#define xpc_JitInstrReporter_h

#include "nsIMemoryReporter.h"
#include "nsIObserver.h"
#include "mozilla/AlreadyAddRefed.h"

namespace mozilla {

// Registered once per process from XPCJSRuntime::Initialize. Its
// CollectReports drives Phase-3 instrumentation snapshots; its
// xpcom-shutdown observer emits the shutdown entries-flush that
// exposes live per-stub enteredCount() before teardown. See the
// SMDOC block in JitInstrReporter.cpp.
class JitInstrReporter final : public nsIMemoryReporter, public nsIObserver {
  ~JitInstrReporter() = default;

 public:
  JitInstrReporter() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIMEMORYREPORTER
  NS_DECL_NSIOBSERVER

  static already_AddRefed<JitInstrReporter> Create();
  static void Register();
};

}  // namespace mozilla

#endif  // xpc_JitInstrReporter_h
