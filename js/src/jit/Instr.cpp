/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Instr.h"

#include "mozilla/HashFunctions.h"
#include "mozilla/HashTable.h"
#include "mozilla/TimeStamp.h"

#ifndef JS_STANDALONE
#  include "mozilla/ProcessType.h"
#endif

#ifdef XP_LINUX
#  include <sys/syscall.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include "jit/ExecutableAllocator.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "vm/MutexIDs.h"

using namespace js;
using namespace js::jit;
using mozilla::TimeStamp;

namespace {

// -------------------------- enum -> name --------------------------

const char* NameOf(InstrMode m) {
  switch (m) {
    case InstrMode::Structural:
      return "structural";
    case InstrMode::Demand:
      return "demand";
  }
  return "?";
}
const char* NameOf(SourceClass c) {
  switch (c) {
    case SourceClass::SelfHosted:
      return "self-hosted";
    case SourceClass::Chrome:
      return "chrome";
    case SourceClass::Guest:
      return "guest";
  }
  return "?";
}
const char* NameOf(IcEngine e) {
  switch (e) {
    case IcEngine::Baseline:
      return "baseline";
    case IcEngine::Ion:
      return "ion";
  }
  return "?";
}
const char* NameOf(IcDetachReason r) {
  switch (r) {
    case IcDetachReason::Transition:
      return "transition";
    case IcDetachReason::Fold:
      return "fold";
    case IcDetachReason::Overflow:
      return "overflow";
    case IcDetachReason::TrialInline:
      return "trial-inline";
    case IcDetachReason::IonTransition:
      return "ion-transition";
    case IcDetachReason::GcPurge:
      return "gc-purge";
    case IcDetachReason::WeakSweep:
      return "weak-sweep";
    case IcDetachReason::ScriptDestroy:
      return "script-destroy";
    case IcDetachReason::RuntimeShutdown:
      return "runtime-shutdown";
    case IcDetachReason::Clone:
      return "clone";
    case IcDetachReason::WarpAbort:
      return "warp-abort";
  }
  return "?";
}
const char* NameOf(JitCodeOwner o) {
  switch (o) {
    case JitCodeOwner::BaselineScript:
      return "baseline-script";
    case JitCodeOwner::BaselineIC:
      return "baseline-ic";
    case JitCodeOwner::SharedIC:
      return "shared-ic";
    case JitCodeOwner::Trampoline:
      return "trampoline";
    case JitCodeOwner::Ion:
      return "ion";
    case JitCodeOwner::Regexp:
      return "regexp";
    case JitCodeOwner::Wasm:
      return "wasm";
    case JitCodeOwner::Other:
      return "other";
  }
  return "?";
}
const char* NameOf(ExecPoolKind k) {
  switch (k) {
    case ExecPoolKind::Baseline:
      return "baseline";
    case ExecPoolKind::Ion:
      return "ion";
    case ExecPoolKind::Other:
      return "other";
    case ExecPoolKind::Regexp:
      return "regexp";
    case ExecPoolKind::Wasm:
      return "wasm";
  }
  return "?";
}

const char* GetProcTag() {
#ifndef JS_STANDALONE
  switch (mozilla::GetGeckoProcessType()) {
    case GeckoProcessType_Default:
      return "parent";
    case GeckoProcessType_Content:
      return "content";
    case GeckoProcessType_IPDLUnitTest:
      return "ipdlunittest";
    case GeckoProcessType_GMPlugin:
      return "gmplugin";
    case GeckoProcessType_GPU:
      return "gpu";
    case GeckoProcessType_VR:
      return "vr";
    case GeckoProcessType_RDD:
      return "rdd";
    case GeckoProcessType_Socket:
      return "socket";
    case GeckoProcessType_ForkServer:
      return "forkserver";
    case GeckoProcessType_Utility:
      return "utility";
    default:
      return "unknown";
  }
#else
  return "jsshell";
#endif
}

uint32_t CurrentTid() {
#if defined(XP_LINUX) && defined(SYS_gettid)
  return uint32_t(syscall(SYS_gettid));
#else
  return uint32_t(getpid());
#endif
}

// ---------------------------- JsonlSink ----------------------------
//
// Owns the per-process FILE*, a small write buffer, and the sink
// mutex. Callers do not touch either. `EmitLine` acquires the mutex,
// hands the caller a JsonlBuilder, then unlocks and writes.
//
// Each line is one JSON object followed by '\n'. Buffering is
// line-granular: the whole line is written with a single fwrite under
// the lock. `setbuf(NULL)` gives us unbuffered mode; combined with a
// per-line fwrite we get atomic durability with negligible cost.

class JsonlBuilder {
  static constexpr size_t kInlineCap = 4096;
  char inlineBuf_[kInlineCap];
  char* buf_ = inlineBuf_;
  size_t cap_ = kInlineCap;
  size_t len_ = 0;
  bool ownsBuf_ = false;
  int depth_ = 0;
  bool needComma_[16] = {false};

  bool Grow(size_t want) {
    size_t need = len_ + want + 1;
    if (need <= cap_) return true;
    size_t newCap = cap_ * 2;
    while (newCap < need) newCap *= 2;
    char* nb = static_cast<char*>(malloc(newCap));
    if (!nb) return false;
    memcpy(nb, buf_, len_);
    if (ownsBuf_) free(buf_);
    buf_ = nb;
    cap_ = newCap;
    ownsBuf_ = true;
    return true;
  }

  void Raw(const char* s) { Raw(s, strlen(s)); }
  void Raw(const char* s, size_t n) {
    if (!Grow(n)) return;
    memcpy(buf_ + len_, s, n);
    len_ += n;
  }
  void RawCh(char c) {
    if (!Grow(1)) return;
    buf_[len_++] = c;
  }

  void EscString(const char* s) {
    RawCh('"');
    for (const char* p = s; *p; ++p) {
      unsigned char c = static_cast<unsigned char>(*p);
      switch (c) {
        case '"':
          Raw("\\\"", 2);
          break;
        case '\\':
          Raw("\\\\", 2);
          break;
        case '\b':
          Raw("\\b", 2);
          break;
        case '\f':
          Raw("\\f", 2);
          break;
        case '\n':
          Raw("\\n", 2);
          break;
        case '\r':
          Raw("\\r", 2);
          break;
        case '\t':
          Raw("\\t", 2);
          break;
        default:
          if (c < 0x20) {
            char tmp[8];
            int n = snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            Raw(tmp, size_t(n));
          } else {
            RawCh(char(c));
          }
      }
    }
    RawCh('"');
  }

  void CommaIfNeeded() {
    if (needComma_[depth_]) {
      RawCh(',');
    }
    needComma_[depth_] = true;
  }

 public:
  JsonlBuilder() = default;
  ~JsonlBuilder() {
    if (ownsBuf_) free(buf_);
  }

  JsonlBuilder(const JsonlBuilder&) = delete;
  JsonlBuilder& operator=(const JsonlBuilder&) = delete;

  void BeginObject() {
    CommaIfNeeded();
    RawCh('{');
    ++depth_;
    needComma_[depth_] = false;
  }
  void EndObject() {
    RawCh('}');
    --depth_;
  }

  void BeginArray(const char* key) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    RawCh('[');
    ++depth_;
    needComma_[depth_] = false;
  }
  void EndArray() {
    RawCh(']');
    --depth_;
  }

  // Element in an array (no key).
  void BeginObjectElement() {
    CommaIfNeeded();
    RawCh('{');
    ++depth_;
    needComma_[depth_] = false;
  }
  void EndObjectElement() {
    RawCh('}');
    --depth_;
  }

  void Str(const char* key, const char* value) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    EscString(value);
  }
  void Sha(const char* key, const Sha1Digest& d) {
    Str(key, ToHex(d).c_str());
  }
  void U32(const char* key, uint32_t v) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%u", v);
    Raw(tmp, size_t(n));
  }
  void I64(const char* key, int64_t v) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    Raw(tmp, size_t(n));
  }
  void U64(const char* key, uint64_t v) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    Raw(tmp, size_t(n));
  }
  void Bool(const char* key, bool b) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    Raw(b ? "true" : "false");
  }
  void Null(const char* key) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    Raw("null");
  }
  void F64(const char* key, double v) {
    CommaIfNeeded();
    EscString(key);
    RawCh(':');
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%.6f", v);
    Raw(tmp, size_t(n));
  }

  mozilla::Span<const char> Span() const {
    return mozilla::Span<const char>(buf_, len_);
  }
};

class JsonlSink {
  FILE* out_ = nullptr;
  js::Mutex lock_ MOZ_UNANNOTATED;
  std::atomic<uint64_t> seq_{0};
  TimeStamp epoch_;
  uint32_t pid_ = 0;
  const char* procTag_ = "unknown";

 public:
  JsonlSink() : lock_(js::mutexid::JSInstrumentation) {}

  bool Open(const char* dir, const char* proc) {
    char path[2048];
    int n = snprintf(path, sizeof(path), "%s/%s.%d.jsonl", dir, proc,
                     int(getpid()));
    if (n <= 0 || size_t(n) >= sizeof(path)) return false;
    FILE* f = fopen(path, "w");
    if (!f) return false;
    setbuf(f, nullptr);
    out_ = f;
    epoch_ = TimeStamp::Now();
    pid_ = uint32_t(getpid());
    procTag_ = proc;
    return true;
  }

  void Close() {
    js::LockGuard<js::Mutex> g(lock_);
    if (out_) {
      fclose(out_);
      out_ = nullptr;
    }
  }

  bool IsOpen() const { return out_ != nullptr; }

  double ElapsedUs() const {
    return (TimeStamp::Now() - epoch_).ToMicroseconds();
  }

  uint32_t Pid() const { return pid_; }
  const char* Proc() const { return procTag_; }

  // Write one JSON object line. `body(b)` fills the object; the
  // common header keys (v, kind, seq, ts_us, pid, proc, tid, rt) are
  // written first by the sink.
  template <typename Body>
  void EmitLine(const char* kind, uint32_t rt, Body&& body) {
    if (!out_) return;
    JsonlBuilder b;
    b.BeginObject();
    b.U32("v", 1);
    b.Str("kind", kind);
    b.U64("seq", seq_.fetch_add(1, std::memory_order_relaxed));
    b.F64("ts_us", ElapsedUs());
    b.U32("pid", pid_);
    b.Str("proc", procTag_);
    b.U32("tid", CurrentTid());
    b.U32("rt", rt);
    body(b);
    b.EndObject();
    auto span = b.Span();
    {
      js::LockGuard<js::Mutex> g(lock_);
      if (out_) {
        fwrite(span.data(), 1, span.size(), out_);
        fputc('\n', out_);
      }
    }
  }
};

// --------------------------- Registry ----------------------------
//
// Assigns process-monotonic local ids for pools, jitcode, scripts,
// sites, runtimes, and unique IC bodies. Also memoizes SHA-1
// identities that are expensive to recompute.

class InstrRegistry {
  struct PtrHasher {
    using Lookup = const void*;
    static mozilla::HashNumber hash(Lookup p) {
      return mozilla::HashGeneric(uintptr_t(p));
    }
    static bool match(const void* a, Lookup b) { return a == b; }
  };
  struct SiteKey {
    JSScript* script;
    uint32_t bcOffset;
  };
  struct SiteHasher {
    using Lookup = SiteKey;
    static mozilla::HashNumber hash(const SiteKey& k) {
      return mozilla::HashGeneric(uintptr_t(k.script), k.bcOffset);
    }
    static bool match(const SiteKey& a, const SiteKey& b) {
      return a.script == b.script && a.bcOffset == b.bcOffset;
    }
  };
  struct DigestHasher {
    using Lookup = Sha1Digest;
    static mozilla::HashNumber hash(const Sha1Digest& d) { return Prefix32(d); }
    static bool match(const Sha1Digest& a, const Sha1Digest& b) {
      return a == b;
    }
  };

  struct PoolRecord {
    uint32_t id;
    ExecPoolKind kind;
    size_t mmapBytes;
  };
  struct CodeRecord {
    uint32_t id;
    JitCodeOwner owner;
    uint64_t bytes;
  };

  static constexpr size_t kNumOwners = 8;  // JitCodeOwner enumerators

  js::Mutex lock_ MOZ_UNANNOTATED;
  std::atomic<uint32_t> nextRt_{1};
  std::atomic<uint32_t> nextPool_{1};
  std::atomic<uint32_t> nextCode_{1};
  std::atomic<uint32_t> nextScript_{1};
  std::atomic<uint32_t> nextSite_{1};
  std::atomic<uint32_t> nextIcBody_{1};

  std::atomic<uint64_t> liveCountByOwner_[kNumOwners]{};
  std::atomic<uint64_t> liveBytesByOwner_[kNumOwners]{};
  std::atomic<uint64_t> livePoolCount_{0};
  std::atomic<uint64_t> liveMmapBytes_{0};
  std::atomic<uint64_t> liveIcBodyCount_{0};
  std::atomic<uint64_t> liveIcBodyBytes_{0};

  mozilla::HashMap<const void*, uint32_t, PtrHasher, js::SystemAllocPolicy>
      runtimes_;
  mozilla::HashMap<const ExecutablePool*, PoolRecord, PtrHasher,
                   js::SystemAllocPolicy>
      pools_;
  mozilla::HashMap<const JitCode*, CodeRecord, PtrHasher, js::SystemAllocPolicy>
      codes_;
  mozilla::HashMap<const void*, uint32_t, PtrHasher, js::SystemAllocPolicy>
      scripts_;
  mozilla::HashMap<SiteKey, uint32_t, SiteHasher, js::SystemAllocPolicy> sites_;
  mozilla::HashMap<Sha1Digest, uint32_t, DigestHasher, js::SystemAllocPolicy>
      icBodies_;

 public:
  InstrRegistry() : lock_(js::mutexid::JSInstrumentation) {}

  uint32_t RuntimeId(JSRuntime* rt) {
    if (!rt) return 0;
    js::LockGuard<js::Mutex> g(lock_);
    auto p = runtimes_.lookupForAdd(rt);
    if (p) return p->value();
    uint32_t id = nextRt_.fetch_add(1, std::memory_order_relaxed);
    (void)runtimes_.add(p, rt, id);
    return id;
  }

  uint32_t RegisterPool(const ExecutablePool* pool, ExecPoolKind kind,
                        size_t mmapBytes, bool* isNew) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = pools_.lookupForAdd(pool);
    if (p) {
      *isNew = false;
      return p->value().id;
    }
    uint32_t id = nextPool_.fetch_add(1, std::memory_order_relaxed);
    PoolRecord rec{id, kind, mmapBytes};
    (void)pools_.add(p, pool, rec);
    *isNew = true;
    livePoolCount_.fetch_add(1, std::memory_order_relaxed);
    liveMmapBytes_.fetch_add(mmapBytes, std::memory_order_relaxed);
    return id;
  }
  uint32_t PoolId(const ExecutablePool* pool) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = pools_.lookup(pool);
    return p ? p->value().id : 0;
  }
  // Returns the removed record so LogPoolUnmap can decrement live
  // counters. If the pool was never registered, returns {0, Other, 0}.
  PoolRecord ForgetPool(const ExecutablePool* pool) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = pools_.lookup(pool);
    if (!p) return {0, ExecPoolKind::Other, 0};
    PoolRecord rec = p->value();
    pools_.remove(p);
    livePoolCount_.fetch_sub(1, std::memory_order_relaxed);
    liveMmapBytes_.fetch_sub(rec.mmapBytes, std::memory_order_relaxed);
    return rec;
  }

  uint32_t RegisterCode(const JitCode* code, JitCodeOwner owner, uint64_t bytes,
                        bool* isNew) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = codes_.lookupForAdd(code);
    if (p) {
      *isNew = false;
      return p->value().id;
    }
    uint32_t id = nextCode_.fetch_add(1, std::memory_order_relaxed);
    CodeRecord rec{id, owner, bytes};
    (void)codes_.add(p, code, rec);
    *isNew = true;
    liveCountByOwner_[size_t(owner)].fetch_add(1, std::memory_order_relaxed);
    liveBytesByOwner_[size_t(owner)].fetch_add(bytes,
                                               std::memory_order_relaxed);
    return id;
  }
  uint32_t CodeId(const JitCode* code) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = codes_.lookup(code);
    return p ? p->value().id : 0;
  }
  CodeRecord ForgetCode(const JitCode* code) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = codes_.lookup(code);
    if (!p) return {0, JitCodeOwner::Other, 0};
    CodeRecord rec = p->value();
    codes_.remove(p);
    liveCountByOwner_[size_t(rec.owner)].fetch_sub(1,
                                                   std::memory_order_relaxed);
    liveBytesByOwner_[size_t(rec.owner)].fetch_sub(rec.bytes,
                                                   std::memory_order_relaxed);
    return rec;
  }

  // Snapshot iteration. Holds the registry lock while `cb` is invoked
  // for each live pool. Callback must not re-enter the registry.
  template <typename Cb>
  void ForEachLivePool(Cb cb) {
    js::LockGuard<js::Mutex> g(lock_);
    for (auto r = pools_.iter(); !r.done(); r.next()) {
      const ExecutablePool* pool = r.get().key();
      const PoolRecord& rec = r.get().value();
      cb(pool, rec.id, rec.kind, rec.mmapBytes);
    }
  }

  void ReadLiveCounters(uint64_t (&count)[kNumOwners],
                        uint64_t (&bytes)[kNumOwners],
                        uint64_t& poolCount, uint64_t& mmapBytes,
                        uint64_t& icBodyCount, uint64_t& icBodyBytes) {
    for (size_t i = 0; i < kNumOwners; ++i) {
      count[i] = liveCountByOwner_[i].load(std::memory_order_relaxed);
      bytes[i] = liveBytesByOwner_[i].load(std::memory_order_relaxed);
    }
    poolCount = livePoolCount_.load(std::memory_order_relaxed);
    mmapBytes = liveMmapBytes_.load(std::memory_order_relaxed);
    icBodyCount = liveIcBodyCount_.load(std::memory_order_relaxed);
    icBodyBytes = liveIcBodyBytes_.load(std::memory_order_relaxed);
  }

  void BumpIcBody(uint32_t bodyBytes) {
    liveIcBodyCount_.fetch_add(1, std::memory_order_relaxed);
    liveIcBodyBytes_.fetch_add(bodyBytes, std::memory_order_relaxed);
  }

  uint32_t ScriptId(const JSScript* script, bool* isNew = nullptr) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = scripts_.lookupForAdd(script);
    if (p) {
      if (isNew) *isNew = false;
      return p->value();
    }
    uint32_t id = nextScript_.fetch_add(1, std::memory_order_relaxed);
    (void)scripts_.add(p, script, id);
    if (isNew) *isNew = true;
    return id;
  }
  void ForgetScript(const JSScript* script) {
    js::LockGuard<js::Mutex> g(lock_);
    scripts_.remove(script);
  }

  uint32_t SiteId(JSScript* script, uint32_t bcOffset, bool* isNew = nullptr) {
    SiteKey key{script, bcOffset};
    js::LockGuard<js::Mutex> g(lock_);
    auto p = sites_.lookupForAdd(key);
    if (p) {
      if (isNew) *isNew = false;
      return p->value();
    }
    uint32_t id = nextSite_.fetch_add(1, std::memory_order_relaxed);
    (void)sites_.add(p, key, id);
    if (isNew) *isNew = true;
    return id;
  }

  // Returns local id of the interned body; sets `firstTime` iff this
  // was the first intern (in which case the caller must emit
  // `ic-body-emit`).
  uint32_t InternIcBody(const Sha1Digest& d, bool* firstTime) {
    js::LockGuard<js::Mutex> g(lock_);
    auto p = icBodies_.lookupForAdd(d);
    if (p) {
      *firstTime = false;
      return p->value();
    }
    uint32_t id = nextIcBody_.fetch_add(1, std::memory_order_relaxed);
    (void)icBodies_.add(p, d, id);
    *firstTime = true;
    return id;
  }
};

// -------------------------- singleton state --------------------------

struct InstrGlobal {
  std::atomic<uint32_t> channels{0};
  InstrMode mode = InstrMode::Structural;
  JsonlSink sink;
  InstrRegistry registry;
  char runId[256] = {0};
  bool initialized = false;
};

InstrGlobal* GlobalPtr() {
  static InstrGlobal g;
  return &g;
}

std::once_flag gInitOnce;

uint32_t ParseChannels(const char* env) {
  if (!env || !*env) return 0;
  if (strcmp(env, "1") == 0 || strcmp(env, "all") == 0) {
    return InstrCh_All;
  }
  uint32_t mask = 0;
  if (strstr(env, "lifecycle")) mask |= InstrCh_Lifecycle;
  if (strstr(env, "ic")) mask |= InstrCh_IC;
  if (strstr(env, "demand")) mask |= InstrCh_Demand;
  if (strstr(env, "coupling")) mask |= InstrCh_Coupling;
  if (strstr(env, "snapshot")) mask |= InstrCh_Snapshot;
  if (strstr(env, "timing")) mask |= InstrCh_Timing;
  if (strstr(env, "baseline")) mask |= InstrCh_Baseline;
  return mask;
}

}  // namespace

namespace js::jit {

const char* Name(InstrMode m) { return NameOf(m); }
const char* Name(SourceClass c) { return NameOf(c); }
const char* Name(IcEngine e) { return NameOf(e); }
const char* Name(IcDetachReason r) { return NameOf(r); }
const char* Name(JitCodeOwner o) { return NameOf(o); }
const char* Name(ExecPoolKind k) { return NameOf(k); }

void JSInstr::Init() {
  std::call_once(gInitOnce, [] {
    InstrGlobal* g = GlobalPtr();

    // Loud-fail path: if any JS_INSTR_* env var is set the operator
    // intends to record. Any early return silently disables logging
    // and produces zero-byte artefacts, which corrupts multi-hour
    // experiments. So when intent is detected, hard-fail instead.
    auto Intent = []() {
      return getenv("JS_INSTR") || getenv("JS_INSTR_DIR") ||
             getenv("JS_INSTR_RUN_ID") || getenv("JS_INSTR_MODE");
    };
    auto Die = [&](const char* why) {
      if (Intent()) {
        fprintf(stderr, "JS_INSTR misconfigured: %s\n", why);
        _exit(1);
      }
    };

    const char* env = getenv("JS_INSTR");
    uint32_t channels = ParseChannels(env);
    if (!channels) {
      Die("JS_INSTR env var missing or empty (want '1', 'all', or "
          "comma-separated channels: lifecycle,ic,demand,coupling,snapshot,"
          "timing,baseline)");
      return;
    }

    const char* dir = getenv("JS_INSTR_DIR");
    const char* runId = getenv("JS_INSTR_RUN_ID");
    const char* mode = getenv("JS_INSTR_MODE");
    if (!dir || !runId) {
      Die("JS_INSTR_DIR and JS_INSTR_RUN_ID are both required");
      return;
    }

    const char* proc = GetProcTag();
    if (!g->sink.Open(dir, proc)) {
      Die("JsonlSink::Open failed (check JS_INSTR_DIR exists and is writable)");
      return;
    }

    g->mode = InstrMode::Structural;
    if (mode && strcmp(mode, "demand") == 0) g->mode = InstrMode::Demand;
    strncpy(g->runId, runId, sizeof(g->runId) - 1);
    g->channels.store(channels, std::memory_order_release);
    g->initialized = true;

    if (g->mode == InstrMode::Demand) {
      JitOptions.instrDemandMode = true;
    }

    atexit(&JSInstr::ProcessShutdown);

    // Wall-clock epoch in microseconds since the Unix epoch, captured
    // at log-open time. Every subsequent event line's `ts_us` is an
    // offset from this moment. The harness computes absolute wall
    // time for any event as `wall_us_epoch + ts_us`, which is how it
    // aligns per-process JSONL files without needing coordinated
    // triggers.
    uint64_t wallUsEpoch = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    g->sink.EmitLine("run-header", 0, [g, channels, wallUsEpoch](JsonlBuilder& b) {
      b.Str("run_id", g->runId);
      b.Str("mode", NameOf(g->mode));
      b.U32("channels", channels);
      b.U64("wall_us_epoch", wallUsEpoch);
    });
  });

  InstrGlobal* g = GlobalPtr();
  if (!g->initialized) return;
}

bool JSInstr::Enabled(uint32_t channel) {
  return (GlobalPtr()->channels.load(std::memory_order_relaxed) & channel) != 0;
}

uint32_t JSInstr::RuntimeLocalId(JSRuntime* rt) {
  return GlobalPtr()->registry.RuntimeId(rt);
}

uint32_t JSInstr::ScriptLocalId(JSScript* script) {
  bool _isNew = false;
  return GlobalPtr()->registry.ScriptId(script, &_isNew);
}

uint32_t JSInstr::SiteLocalId(JSScript* script, uint32_t bcOffset) {
  return GlobalPtr()->registry.SiteId(script, bcOffset);
}

void JSInstr::RuntimeShutdown(JSRuntime* rt) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  uint32_t rid = RuntimeLocalId(rt);
  GlobalPtr()->sink.EmitLine("runtime-shutdown", rid, [](JsonlBuilder&) {});
}

void JSInstr::ProcessShutdown() {
  InstrGlobal* g = GlobalPtr();
  if (g->initialized) {
    g->sink.EmitLine("process-shutdown", 0, [](JsonlBuilder&) {});
    g->sink.Close();
  }
}

// ---------------------------- lifecycle ----------------------------

void JSInstr::LogPoolCreate(ExecutablePool* pool, ExecPoolKind kind,
                            size_t mmapBytes) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  auto* g = GlobalPtr();
  bool isNew = false;
  uint32_t poolId = g->registry.RegisterPool(pool, kind, mmapBytes, &isNew);
  if (!isNew) return;
  g->sink.EmitLine("pool-create", 0, [&](JsonlBuilder& b) {
    b.U32("pool_id", poolId);
    b.Str("pool_kind", NameOf(kind));
    b.U64("mmap_bytes", uint64_t(mmapBytes));
  });
}

void JSInstr::LogPoolUnmap(ExecutablePool* pool) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  auto* g = GlobalPtr();
  auto rec = g->registry.ForgetPool(pool);
  g->sink.EmitLine("pool-unmap", 0, [&](JsonlBuilder& b) {
    b.U32("pool_id", rec.id);
  });
}

void JSInstr::LogJitCodeCreate(JitCode* code, JitCodeOwner owner) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  auto* g = GlobalPtr();
  bool isNew = false;
  uint64_t bytes = uint64_t(code->instructionsSize());
  uint32_t codeId = g->registry.RegisterCode(code, owner, bytes, &isNew);
  if (!isNew) return;
  ExecutablePool* pool = code->pool();
  uint32_t poolId = pool ? g->registry.PoolId(pool) : 0;
  g->sink.EmitLine("jitcode-create", 0, [&](JsonlBuilder& b) {
    b.U32("code_local_id", codeId);
    b.U32("pool_id", poolId);
    b.U64("bytes", bytes);
    b.Str("owner", NameOf(owner));
  });
}

void JSInstr::LogJitCodeFinalize(JitCode* code) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  auto* g = GlobalPtr();
  auto rec = g->registry.ForgetCode(code);
  g->sink.EmitLine("jitcode-finalize", 0, [&](JsonlBuilder& b) {
    b.U32("code_local_id", rec.id);
  });
}

static SourceClass ClassifyScript(JSScript* script) {
  if (script->selfHosted()) return SourceClass::SelfHosted;
  // Chrome vs. guest is determined by the script's realm's principals.
  // Deferring an accurate classification here would drag in
  // JSPrincipals plumbing; for now, non-self-hosted defaults to guest
  // and the harness re-classifies via source_id at analysis time.
  return SourceClass::Guest;
}

static Sha1Digest ComputeSourceId(JSScript* script) {
  Sha1Mixer m;
  const char* fn = script->filename();
  if (fn) m.Append(fn, strlen(fn));
  uint32_t line = script->lineno();
  uint32_t col = script->column().oneOriginValue();
  m.AppendPod(line);
  m.AppendPod(col);
  return m.Finish();
}

void JSInstr::LogScriptCreate(JSScript* script) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  auto* g = GlobalPtr();
  bool isNew = false;
  uint32_t sid = g->registry.ScriptId(script, &isNew);
  if (!isNew) return;
  Sha1Digest srcId = ComputeSourceId(script);
  g->sink.EmitLine("script-create", 0, [&](JsonlBuilder& b) {
    b.U32("script_local_id", sid);
    b.Sha("source_id", srcId);
    b.Str("source_class", NameOf(ClassifyScript(script)));
    b.U32("line", script->lineno());
    b.U32("col", script->column().oneOriginValue());
  });
}

void JSInstr::LogScriptDestroy(JSScript* script) {
  if (!Enabled(InstrCh_Lifecycle)) return;
  auto* g = GlobalPtr();
  uint32_t sid = g->registry.ScriptId(script);
  g->sink.EmitLine("script-destroy", 0, [&](JsonlBuilder& b) {
    b.U32("script_local_id", sid);
  });
  g->registry.ForgetScript(script);
}

// ----------------------------- baseline -----------------------------

void JSInstr::LogBaselineCompile(JSRuntime* rt, JSScript* script,
                                 const Sha1Digest& semanticId,
                                 const Sha1Digest& codeId, uint32_t methodBytes,
                                 uint32_t metadataBytes,
                                 uint32_t numIcEntries) {
  if (!Enabled(InstrCh_Baseline)) return;
  auto* g = GlobalPtr();
  bool _newScript = false;
  uint32_t sid = g->registry.ScriptId(script, &_newScript);
  uint32_t rid = RuntimeLocalId(rt);
  g->sink.EmitLine("baseline-compile", rid, [&](JsonlBuilder& b) {
    b.U32("script_local_id", sid);
    b.Sha("semantic_id", semanticId);
    b.Sha("code_id", codeId);
    b.U32("method_bytes", methodBytes);
    b.U32("metadata_bytes", metadataBytes);
    b.U32("num_ic_entries", numIcEntries);
  });
}

void JSInstr::LogBaselineRetire(JSScript* script) {
  if (!Enabled(InstrCh_Baseline)) return;
  auto* g = GlobalPtr();
  uint32_t sid = g->registry.ScriptId(script);
  g->sink.EmitLine("baseline-retire", 0, [&](JsonlBuilder& b) {
    b.U32("script_local_id", sid);
  });
}

// -------------------------------- IC --------------------------------

void JSInstr::LogIcBodyEmit(const Sha1Digest& icBodyId, const char* cacheKind,
                            uint32_t bodyBytes, uint32_t stubDataBytes,
                            mozilla::Span<const CouplingRecord> coupling) {
  if (!Enabled(InstrCh_IC)) return;
  auto* g = GlobalPtr();
  bool first = false;
  uint32_t bodyLocalId = g->registry.InternIcBody(icBodyId, &first);
  if (!first) return;
  g->registry.BumpIcBody(bodyBytes);
  const bool includeCoupling = Enabled(InstrCh_Coupling);
  g->sink.EmitLine("ic-body-emit", 0, [&](JsonlBuilder& b) {
    b.U32("ic_body_local_id", bodyLocalId);
    b.Sha("ic_body_id", icBodyId);
    b.Str("cache_kind", cacheKind ? cacheKind : "?");
    b.U32("body_bytes", bodyBytes);
    b.U32("stub_data_bytes", stubDataBytes);
    if (includeCoupling) {
      b.BeginArray("coupling");
      for (const CouplingRecord& r : coupling) {
        b.BeginObjectElement();
        b.Str("operand_kind", r.operandKind ? r.operandKind : "");
        b.U32("patch_offset", r.patchOffset);
        b.Str("target_kind", r.targetKind ? r.targetKind : "");
        b.Str("reloc_kind", r.relocKind ? r.relocKind : "");
        b.Str("eligibility", r.eligibility ? r.eligibility : "");
        b.EndObjectElement();
      }
      b.EndArray();
    }
  });
}

void JSInstr::LogIcInstanceAttach(JSScript* outerScript, uint32_t bcOffset,
                                  const Sha1Digest& icBodyId,
                                  IcEngine engine) {
  if (!Enabled(InstrCh_IC)) return;
  auto* g = GlobalPtr();
  uint32_t siteId = g->registry.SiteId(outerScript, bcOffset);
  uint32_t scriptId = g->registry.ScriptId(outerScript);
  SourceClass sc = ClassifyScript(outerScript);
  g->sink.EmitLine("ic-instance-attach", 0, [&](JsonlBuilder& b) {
    b.U32("site_local_id", siteId);
    b.U32("script_local_id", scriptId);
    b.Sha("ic_body_id", icBodyId);
    b.Str("engine", NameOf(engine));
    b.Str("source_class", NameOf(sc));
  });
}

void JSInstr::LogIcInstanceDetach(JSScript* outerScript, uint32_t bcOffset,
                                  const Sha1Digest& icBodyId,
                                  IcDetachReason reason, uint32_t enteredCount,
                                  bool isFallback,
                                  uint32_t chainLengthBefore) {
  if (!Enabled(InstrCh_IC)) return;
  auto* g = GlobalPtr();
  uint32_t siteId = g->registry.SiteId(outerScript, bcOffset);
  uint32_t scriptId = g->registry.ScriptId(outerScript);
  g->sink.EmitLine("ic-instance-detach", 0, [&](JsonlBuilder& b) {
    b.U32("site_local_id", siteId);
    b.U32("script_local_id", scriptId);
    b.Sha("ic_body_id", icBodyId);
    b.Str("reason", NameOf(reason));
    b.U32("entered_count", enteredCount);
    b.Bool("is_fallback", isFallback);
    b.U32("chain_length_before", chainLengthBefore);
  });
}

// ----------------------------- snapshot -----------------------------

void JSInstr::LogSnapshotMarker(const char* marker) {
  if (!Enabled(InstrCh_Snapshot)) return;
  auto* g = GlobalPtr();
  g->sink.EmitLine("snapshot-marker", 0,
                   [&](JsonlBuilder& b) { b.Str("marker", marker); });
}

void JSInstr::LogSnapshotFootprint(uint32_t poolId, const char* poolKind,
                                   size_t mmapBytes, size_t usedBytes,
                                   size_t unusedBytes) {
  if (!Enabled(InstrCh_Snapshot)) return;
  auto* g = GlobalPtr();
  g->sink.EmitLine("snapshot-footprint", 0, [&](JsonlBuilder& b) {
    b.U32("pool_id", poolId);
    b.Str("pool_kind", poolKind ? poolKind : "");
    b.U64("mmap_bytes", uint64_t(mmapBytes));
    b.U64("used_bytes", uint64_t(usedBytes));
    b.U64("unused_bytes", uint64_t(unusedBytes));
  });
}

void JSInstr::GetLiveCounters(LiveCounters* out) {
  auto* g = GlobalPtr();
  uint64_t counts[8];
  uint64_t bytes[8];
  uint64_t poolCount, mmapBytes, icBodyCount, icBodyBytes;
  g->registry.ReadLiveCounters(counts, bytes, poolCount, mmapBytes, icBodyCount,
                               icBodyBytes);
  for (size_t i = 0; i < 8; ++i) {
    out->perOwner[i] = {JitCodeOwner(i), counts[i], bytes[i]};
  }
  out->livePoolCount = poolCount;
  out->liveMmapBytes = mmapBytes;
  out->liveIcBodyCount = icBodyCount;
  out->liveIcBodyBytes = icBodyBytes;
}

void JSInstr::LogSnapshotLive(const LiveCounters& c) {
  if (!Enabled(InstrCh_Snapshot)) return;
  auto* g = GlobalPtr();
  g->sink.EmitLine("snapshot-live", 0, [&](JsonlBuilder& b) {
    b.U64("live_pool_count", c.livePoolCount);
    b.U64("live_mmap_bytes", c.liveMmapBytes);
    // Interned bodies live for the whole process, not the whole
    // program run. Field name reflects "distinct ever seen", not
    // "currently attached to any IC".
    b.U64("distinct_ic_body_count", c.liveIcBodyCount);
    b.U64("distinct_ic_body_bytes", c.liveIcBodyBytes);
    b.BeginArray("by_owner");
    for (size_t i = 0; i < 8; ++i) {
      if (c.perOwner[i].count == 0 && c.perOwner[i].codeBytes == 0) continue;
      b.BeginObjectElement();
      b.Str("owner", NameOf(c.perOwner[i].owner));
      b.U64("count", c.perOwner[i].count);
      b.U64("code_bytes", c.perOwner[i].codeBytes);
      b.EndObjectElement();
    }
    b.EndArray();
  });
}

void JSInstr::LogSnapshotSmapsRow(const SmapsRow& r) {
  if (!Enabled(InstrCh_Snapshot)) return;
  auto* g = GlobalPtr();
  g->sink.EmitLine("snapshot-smaps", 0, [&](JsonlBuilder& b) {
    b.U64("start", r.startAddr);
    b.U64("end", r.endAddr);
    b.U64("size_kb", r.sizeKb);
    b.U64("rss_kb", r.rssKb);
    b.U64("pss_kb", r.pssKb);
    b.U64("shared_clean_kb", r.sharedCleanKb);
    b.U64("shared_dirty_kb", r.sharedDirtyKb);
    b.U64("private_clean_kb", r.privateCleanKb);
    b.U64("private_dirty_kb", r.privateDirtyKb);
    b.U64("referenced_kb", r.referencedKb);
    b.U64("anonymous_kb", r.anonymousKb);
    b.Str("perms", r.perms ? r.perms : "");
    b.Str("path", r.path ? r.path : "");
  });
}

void JSInstr::ForEachLivePool(void* userdata, PoolCallback cb) {
  auto* g = GlobalPtr();
  g->registry.ForEachLivePool(
      [&](const ExecutablePool* pool, uint32_t id, ExecPoolKind kind,
          size_t mmapBytes) {
        PoolInfo info{id, NameOf(kind), pool->base(), mmapBytes,
                      pool->usedCodeBytes()};
        cb(userdata, info);
      });
}

void JSInstr::LogEntriesFlush(const char* reason,
                              mozilla::Span<const EntriesFlushRow> scripts,
                              mozilla::Span<const IcEntryRow> icEntries) {
  if (!Enabled(InstrCh_Demand)) return;
  auto* g = GlobalPtr();
  g->sink.EmitLine("entries-flush", 0, [&](JsonlBuilder& b) {
    b.Str("reason", reason ? reason : "");
    b.U32("script_count", uint32_t(scripts.size()));
    b.BeginArray("scripts");
    for (const auto& r : scripts) {
      b.BeginObjectElement();
      b.U32("script_local_id", r.scriptLocalId);
      b.U64("entered_count", r.enteredCount);
      if (r.icEntryCount) {
        b.BeginArray("ic_entries");
        const size_t end = size_t(r.icEntryStart) + r.icEntryCount;
        for (size_t i = r.icEntryStart; i < end && i < icEntries.size(); ++i) {
          const IcEntryRow& e = icEntries[i];
          b.BeginObjectElement();
          b.U32("site_local_id", e.siteLocalId);
          b.Sha("ic_body_id", e.icBodyId);
          b.U64("entered_count", e.enteredCount);
          b.Bool("is_fallback", e.isFallback);
          b.EndObjectElement();
        }
        b.EndArray();
      }
      b.EndObjectElement();
    }
    b.EndArray();
  });
}

void JSInstr::LogEntriesOverflow(uint32_t scriptLocalId) {
  if (!Enabled(InstrCh_Demand)) return;
  auto* g = GlobalPtr();
  g->sink.EmitLine("entries-overflow", 0, [&](JsonlBuilder& b) {
    b.U32("script_local_id", scriptLocalId);
  });
}

}  // namespace js::jit
