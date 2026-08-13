/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTTiming.h"
#  include "js/AOTTiming.h"

#  include "mozilla/JSONWriter.h"
#  include "mozilla/Span.h"
#  include "mozilla/UniquePtr.h"

#  include <algorithm>
#  include <array>
#  include <atomic>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <mutex>
#  include <string>
#  include <unistd.h>

namespace js::jit {

namespace {

struct PhaseCounts {
  uint64_t calls = 0;
  uint64_t totalNs = 0;
};

struct State {
  std::mutex mutex;
  std::array<PhaseCounts, size_t(AOTTimingPhase::Limit)> phases;
  std::array<uint64_t, size_t(AOTTimingCounter::Limit)> counters = {};
};

std::atomic<int8_t> gEnabled{-1};
State gState;

constexpr const char* PhaseName(AOTTimingPhase phase) {
  switch (phase) {
    case AOTTimingPhase::ImageCompatibility:
      return "image_compatibility";
    case AOTTimingPhase::InterpreterAttach:
      return "interpreter_attach";
    case AOTTimingPhase::RITInitialization:
      return "rit_initialization";
    case AOTTimingPhase::ICCorpusAttach:
      return "ic_corpus_attach";
    case AOTTimingPhase::BaselineFunctionLookup:
      return "baseline_function_lookup";
    case AOTTimingPhase::BaselineFunctionReconstruct:
      return "baseline_function_reconstruct";
    case AOTTimingPhase::ICImageLookup:
      return "ic_image_lookup";
    case AOTTimingPhase::ICPrivateAttach:
      return "ic_private_attach";
    case AOTTimingPhase::RuntimeBaselineCompile:
      return "runtime_baseline_compile";
    case AOTTimingPhase::RuntimeICCompile:
      return "runtime_ic_compile";
    case AOTTimingPhase::Limit:
      break;
  }
  MOZ_CRASH("invalid AOT timing phase");
}

constexpr const char* CounterName(AOTTimingCounter counter) {
  switch (counter) {
    case AOTTimingCounter::InterpreterCodeBytes:
      return "interpreter_code_bytes";
    case AOTTimingCounter::InterpreterMetadataBytes:
      return "interpreter_metadata_bytes";
    case AOTTimingCounter::InterpreterWrappers:
      return "interpreter_wrappers";
    case AOTTimingCounter::ICCorpusAttempted:
      return "ic_corpus_attempted";
    case AOTTimingCounter::ICCorpusLoaded:
      return "ic_corpus_loaded";
    case AOTTimingCounter::ICCorpusCodeBytes:
      return "ic_corpus_code_bytes";
    case AOTTimingCounter::ICCorpusMetadataBytes:
      return "ic_corpus_metadata_bytes";
    case AOTTimingCounter::ICCorpusWrappers:
      return "ic_corpus_wrappers";
    case AOTTimingCounter::BaselineLookupHits:
      return "baseline_lookup_hits";
    case AOTTimingCounter::BaselineLookupMisses:
      return "baseline_lookup_misses";
    case AOTTimingCounter::BaselineCodeBytes:
      return "baseline_code_bytes";
    case AOTTimingCounter::BaselineMetadataBytes:
      return "baseline_metadata_bytes";
    case AOTTimingCounter::BaselineWrappers:
      return "baseline_wrappers";
    case AOTTimingCounter::ICImageLookupHits:
      return "ic_image_lookup_hits";
    case AOTTimingCounter::ICImageLookupMisses:
      return "ic_image_lookup_misses";
    case AOTTimingCounter::ICPrivateStubs:
      return "ic_private_stubs";
    case AOTTimingCounter::ICPrivateStubBytes:
      return "ic_private_stub_bytes";
    case AOTTimingCounter::Limit:
      break;
  }
  MOZ_CRASH("invalid AOT timing counter");
}

class StringWriteFunc final : public mozilla::JSONWriteFunc {
 public:
  explicit StringWriteFunc(std::string& output) : output_(output) {}

  void Write(const mozilla::Span<const char>& text) override {
    output_.append(text.Elements(), text.Length());
  }

 private:
  std::string& output_;
};

}  // namespace

bool AOTTiming::IsEnabled() {
  int8_t enabled = gEnabled.load(std::memory_order_acquire);
  if (enabled >= 0) {
    return enabled != 0;
  }

  const char* value = getenv("JS_AOT_TIMING");
  int8_t resolved = value && *value && strcmp(value, "0") != 0;
  int8_t expected = -1;
  if (!gEnabled.compare_exchange_strong(expected, resolved,
                                        std::memory_order_release,
                                        std::memory_order_acquire)) {
    resolved = expected;
  }
  return resolved != 0;
}

void AOTTiming::Record(AOTTimingPhase phase, mozilla::TimeDuration duration) {
  if (!IsEnabled()) {
    return;
  }
  uint64_t nanoseconds = uint64_t(duration.ToMicroseconds() * 1000.0);
  std::lock_guard<std::mutex> lock(gState.mutex);
  PhaseCounts& counts = gState.phases[size_t(phase)];
  counts.calls++;
  counts.totalNs += nanoseconds;
}

void AOTTiming::AddCounter(AOTTimingCounter counter, uint64_t value) {
  if (!IsEnabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(gState.mutex);
  gState.counters[size_t(counter)] += value;
}

void AOTTiming::FlushAndWrite(const char* processType) {
  if (!IsEnabled()) {
    return;
  }

  std::lock_guard<std::mutex> lock(gState.mutex);
  std::string output;
  {
    mozilla::JSONWriter writer(mozilla::MakeUnique<StringWriteFunc>(output));
    writer.Start();
    writer.IntProperty("pid", int64_t(getpid()));
    writer.StringProperty("process_type", mozilla::MakeStringSpan(processType));
    writer.StartObjectProperty("phases");
    for (size_t i = 0; i < size_t(AOTTimingPhase::Limit); i++) {
      AOTTimingPhase phase = AOTTimingPhase(i);
      const PhaseCounts& counts = gState.phases[i];
      writer.StartObjectProperty(mozilla::MakeStringSpan(PhaseName(phase)));
      writer.IntProperty("calls", int64_t(counts.calls));
      writer.IntProperty("total_ns", int64_t(counts.totalNs));
      writer.EndObject();
    }
    writer.EndObject();
    writer.StartObjectProperty("counters");
    for (size_t i = 0; i < size_t(AOTTimingCounter::Limit); i++) {
      writer.IntProperty(
          mozilla::MakeStringSpan(CounterName(AOTTimingCounter(i))),
          int64_t(gState.counters[i]));
    }
    writer.EndObject();
    writer.End();
  }
  output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
  output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
  fprintf(stderr, "AOT_TIMING %s\n", output.c_str());
  fflush(stderr);
}

AutoAOTTimer::AutoAOTTimer(AOTTimingPhase phase, bool condition)
    : phase_(phase), enabled_(condition && AOTTiming::IsEnabled()) {
  if (enabled_) {
    start_ = mozilla::TimeStamp::Now();
  }
}

AutoAOTTimer::~AutoAOTTimer() { Stop(); }

void AutoAOTTimer::Stop() {
  if (enabled_) {
    AOTTiming::Record(phase_, mozilla::TimeStamp::Now() - start_);
    enabled_ = false;
  }
}

}  // namespace js::jit

JS_PUBLIC_API void JS::FlushAOTTiming(const char* processType) {
  js::jit::AOTTiming::FlushAndWrite(processType);
}

#endif  // ENABLE_JS_AOT
