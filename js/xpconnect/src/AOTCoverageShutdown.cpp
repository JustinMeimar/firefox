/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AOTCoverageShutdown.h"

#ifdef ENABLE_JS_AOT

#  include <cstdlib>
#  include <cstring>

#  include "js/AOTCoverage.h"
#  include "mozilla/Services.h"
#  include "nsIObserverService.h"

namespace mozilla {

NS_IMPL_ISUPPORTS(AOTCoverageShutdown, nsIObserver)

// Parent processes fire "xpcom-shutdown"; content processes fire
// "content-child-shutdown" from ContentChild::ShutdownInternal.
// Register for both so every process type is covered on the
// first-fired path.
static const char* const kShutdownTopics[] = {
    "xpcom-shutdown",
    "content-child-shutdown",
};

/* static */
void AOTCoverageShutdown::Register() {
  // Gate on the env var directly: coverage does not arm until the first AOT
  // install, which is strictly later than this.
  const char* path = getenv("JS_AOT_COVERAGE_OUT");
  if (!path || !*path) return;

  RefPtr<AOTCoverageShutdown> observer = new AOTCoverageShutdown();
  if (nsCOMPtr<nsIObserverService> obs =
          mozilla::services::GetObserverService()) {
    for (const char* topic : kShutdownTopics) {
      obs->AddObserver(observer, topic, false);
    }
  }
}

NS_IMETHODIMP
AOTCoverageShutdown::Observe(nsISupports* aSubject, const char* aTopic,
                             const char16_t* aData) {
  bool matched = false;
  for (const char* topic : kShutdownTopics) {
    if (strcmp(aTopic, topic) == 0) {
      matched = true;
      break;
    }
  }
  if (!matched) return NS_OK;

  JS::FlushAOTCoverage();

  if (nsCOMPtr<nsIObserverService> obs =
          mozilla::services::GetObserverService()) {
    for (const char* topic : kShutdownTopics) {
      obs->RemoveObserver(this, topic);
    }
  }
  return NS_OK;
}

}  // namespace mozilla

#endif  // ENABLE_JS_AOT
