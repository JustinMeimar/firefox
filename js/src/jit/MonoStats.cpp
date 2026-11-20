/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MonoStats.h"

#include "mozilla/Atomics.h"

#include <algorithm>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef XP_UNIX
#  include <fcntl.h>
#  include <limits.h>
#  include <pthread.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRReader.h"
#include "jit/JitScript.h"
#include "js/Vector.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSScript.h"

#include "jit/JitScript-inl.h"

using namespace js;
using namespace js::jit;

static const char* const MonoScriptReasonNames[] = {
    "mono",      "off",       "offthread", "debug",          "profiler",
    "failed",    "realm-ind", "generator", "async",          "not-mono"};

static const char* const MonoICDispositionNames[] = {
    "chain", "empty", "inline", "copy", "pushed", "operands", "nocopy"};

static const char* const MonoNoInlineNames[] = {"", "gc", "nocopy", "op",
                                                "failed"};

#ifdef XP_UNIX
static constexpr size_t MaxMonoCounters = size_t(1) << 24;
static pthread_mutex_t gMonoStatsLock = PTHREAD_MUTEX_INITIALIZER;
static pid_t gMonoStatsPid = 0;
static int gMonoEvents = -1;
static uint64_t* gMonoCounters = nullptr;
static size_t gNumMonoCounters = 0;
static pid_t gMonoCodeMapPid = 0;
static int gMonoCodeMap = -1;
#endif
static mozilla::Atomic<uint32_t> gMonoCompileId(0);

static int64_t MonoStatsNowNs() {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}

namespace {

class MonoEventBuffer {
  Vector<char, 1024, SystemAllocPolicy> chars_;
  bool ok_ = true;

 public:
  void printf(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3) {
    if (!ok_) {
      return;
    }
    va_list args;
    va_start(args, fmt);
    char small[256];
    int length = vsnprintf(small, sizeof(small), fmt, args);
    va_end(args);
    if (length < 0) {
      ok_ = false;
      return;
    }
    if (size_t(length) < sizeof(small)) {
      ok_ = chars_.append(small, length);
      return;
    }
    size_t start = chars_.length();
    if (!chars_.growBy(length + 1)) {
      ok_ = false;
      return;
    }
    va_start(args, fmt);
    vsnprintf(chars_.begin() + start, length + 1, fmt, args);
    va_end(args);
    chars_.shrinkBy(1);
  }

  void string(const char* s) {
    printf("\"");
    for (const char* c = s ? s : ""; *c && ok_; c++) {
      if (*c == '"' || *c == '\\') {
        ok_ = chars_.append('\\') && chars_.append(*c);
      } else {
        ok_ = chars_.append(static_cast<unsigned char>(*c) < 0x20 ? '?' : *c);
      }
    }
    printf("\"");
  }

  void hex(const uint8_t* bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    printf("\"");
    if (ok_ && !chars_.reserve(chars_.length() + 2 * length + 1)) {
      ok_ = false;
    }
    for (size_t i = 0; i < length && ok_; i++) {
      chars_.infallibleAppend(digits[bytes[i] >> 4]);
      chars_.infallibleAppend(digits[bytes[i] & 0xf]);
    }
    printf("\"");
  }

  void write(int fd) {
    if (!ok_ || !chars_.append('\n')) {
      return;
    }
#ifdef XP_UNIX
    const char* p = chars_.begin();
    size_t left = chars_.length();
    while (left) {
      ssize_t written = ::write(fd, p, left);
      if (written <= 0) {
        gMonoEvents = -1;
        return;
      }
      p += written;
      left -= written;
    }
#endif
  }
};

}  // namespace

static void WriteInitEventLocked();

#ifdef XP_UNIX
// Opens this process's stats files the first time it asks for them, also
// after a fork. Must be called with the lock held.
static void InitMonoStatsLocked(const char* dir) {
  pid_t pid = getpid();
  if (pid == gMonoStatsPid) {
    return;
  }
  gMonoStatsPid = pid;
  gMonoEvents = -1;
  gMonoCounters = nullptr;
  gNumMonoCounters = 0;

  char path[PATH_MAX];
  const char* counts = getenv("JS_MONO_COUNTS");
  if (counts && *counts && *counts != '0') {
    snprintf(path, sizeof(path), "%s/mono-%d.counters", dir, int(pid));
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      size_t size = MaxMonoCounters * sizeof(uint64_t);
      void* counters = MAP_FAILED;
      if (ftruncate(fd, off_t(size)) == 0) {
        counters =
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      }
      close(fd);
      if (counters != MAP_FAILED) {
        gMonoCounters = static_cast<uint64_t*>(counters);
      }
    }
  }

  snprintf(path, sizeof(path), "%s/mono-%d.events", dir, int(pid));
  gMonoEvents =
      open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (gMonoEvents >= 0) {
    WriteInitEventLocked();
  }
}
#endif

bool js::jit::MonoStatsEnabled() {
#ifdef XP_UNIX
  static const char* dir = getenv("JS_MONO_STATS");
  if (!dir || !*dir) {
    return false;
  }
  pthread_mutex_lock(&gMonoStatsLock);
  InitMonoStatsLocked(dir);
  bool enabled = gMonoEvents >= 0;
  pthread_mutex_unlock(&gMonoStatsLock);
  return enabled;
#else
  return false;
#endif
}

bool js::jit::MonoCountsEnabled() {
#ifdef XP_UNIX
  if (!MonoStatsEnabled()) {
    return false;
  }
  pthread_mutex_lock(&gMonoStatsLock);
  bool enabled = gMonoCounters;
  pthread_mutex_unlock(&gMonoStatsLock);
  return enabled;
#else
  return false;
#endif
}

uint64_t* js::jit::NewMonoCounters(size_t count, int64_t* index) {
  *index = -1;
#ifdef XP_UNIX
  if (!MonoCountsEnabled()) {
    return nullptr;
  }
  pthread_mutex_lock(&gMonoStatsLock);
  uint64_t* counters = nullptr;
  if (gMonoCounters && gNumMonoCounters + count <= MaxMonoCounters) {
    *index = int64_t(gNumMonoCounters);
    counters = &gMonoCounters[gNumMonoCounters];
    gNumMonoCounters += count;
  }
  pthread_mutex_unlock(&gMonoStatsLock);
  return counters;
#else
  return nullptr;
#endif
}

uint32_t js::jit::NextMonoCompileId() { return ++gMonoCompileId; }

static void WriteMonoEvent(MonoEventBuffer& buf) {
#ifdef XP_UNIX
  pthread_mutex_lock(&gMonoStatsLock);
  if (gMonoEvents >= 0) {
    buf.write(gMonoEvents);
  }
  pthread_mutex_unlock(&gMonoStatsLock);
#endif
}

static void WriteInitEventLocked() {
#ifdef XP_UNIX
  MonoEventBuffer buf;
  buf.printf("{\"ev\":\"init\",\"t\":%" PRId64 ",\"pid\":%d,\"counts\":%s",
             MonoStatsNowNs(), int(getpid()), gMonoCounters ? "true" : "false");
  buf.printf(",\"jsops\":[");
  for (size_t i = 0; i < size_t(JSOP_LIMIT); i++) {
    const JSCodeSpec& spec = CodeSpec(JSOp(i));
    buf.printf("%s[", i ? "," : "");
    buf.string(CodeName(JSOp(i)));
    buf.printf(",%d,%d,%d,%u,%d]", int(spec.length), int(spec.nuses),
               int(spec.ndefs), unsigned(spec.format),
               int(BytecodeOpHasIC(JSOp(i))));
  }
  buf.printf("],\"kinds\":[");
  static const size_t numKinds = 0
#define COUNT_KIND(kind) +1
      CACHE_IR_KINDS(COUNT_KIND)
#undef COUNT_KIND
      ;
  for (size_t i = 0; i < numKinds; i++) {
    buf.printf("%s", i ? "," : "");
    buf.string(CacheKindNames[i]);
  }
  buf.printf("],\"cacheops\":[");
  for (size_t i = 0; i < size_t(CacheOp::NumOpcodes); i++) {
    buf.printf("%s", i ? "," : "");
    buf.string(CacheIROpNames[i]);
  }
  buf.printf("]}");
  buf.write(gMonoEvents);
#endif
}

static void PrintMonoScript(MonoEventBuffer& buf, JSScript* script) {
  buf.printf("\"s\":\"%u:%u\"", script->scriptSource()->id(),
             script->sourceStart());
}

static void PrintMonoStubOps(MonoEventBuffer& buf, ICCacheIRStub* stub) {
  buf.printf("\"");
  CacheIRReader reader(stub->stubInfo());
  bool first = true;
  while (reader.more()) {
    CacheOp op = reader.readOp();
    buf.printf("%s%u", first ? "" : ".", unsigned(op));
    first = false;
    uint32_t argLength = CacheIROpInfos[size_t(op)].argLength;
    const uint8_t* args = reader.currentPosition();
    for (uint32_t i = 0; i < argLength; i++) {
      buf.printf("%s%02x", i ? "" : ":", args[i]);
    }
    reader.skip(argLength);
  }
  buf.printf("\"");
}

void js::jit::LogMonoCompile(JSScript* script, BaselineScript* baselineScript,
                             MonoScriptReason reason, int64_t countersIndex,
                             uint32_t exitsStart, uint32_t exitsEnd,
                             mozilla::Span<const MonoStatsIC> ics) {
  if (!MonoStatsEnabled()) {
    return;
  }
  JitCode* code = baselineScript->method();
  MonoEventBuffer buf;
  buf.printf("{\"ev\":\"compile\",\"t\":%" PRId64 ",\"id\":%u,", MonoStatsNowNs(),
             baselineScript->monoStatsId());
  PrintMonoScript(buf, script);
  buf.printf(",\"file\":");
  buf.string(script->filename());
  buf.printf(",\"line\":%u,\"col\":%u", script->lineno(),
             script->column().oneOriginValue());
  buf.printf(",\"why\":\"%s\",\"wu\":%u,\"code\":\"%p\",\"bytes\":%zu",
             MonoScriptReasonNames[size_t(reason)], script->getWarmUpCount(),
             code->raw(), code->instructionsSize());
  buf.printf(",\"exits\":[%u,%u],\"ctr\":%" PRId64, exitsStart, exitsEnd,
             countersIndex);
  buf.printf(",\"bc\":");
  buf.hex(script->code(), script->length());

  ICScript* icScript = script->jitScript()->icScript();
  buf.printf(",\"ics\":[");
  bool first = true;
  for (const MonoStatsIC& ic : ics) {
    ICFallbackStub* fallback = icScript->fallbackStub(ic.entryIndex);
    const ICState& state = fallback->state();
    ICStub* firstStub = icScript->icEntry(ic.entryIndex).firstStub();
    buf.printf("%s[%u,%u,%u,%u,%u,%u,%u,\"%s\",\"%s\",%d,", first ? "" : ",",
               ic.entryIndex, ic.pcOffset, ic.start, ic.end, ic.exitBytes,
               unsigned(state.numOptimizedStubs()), unsigned(state.mode()),
               MonoICDispositionNames[size_t(ic.disposition)],
               MonoNoInlineNames[size_t(ic.noInline)],
               ic.noInline == MonoNoInline::Op ? int(ic.noInlineOp) : -1);
    buf.printf("%d,", int(state.hasFailures()));
    if (firstStub->isFallback()) {
      buf.printf("\"\"]");
    } else {
      PrintMonoStubOps(buf, firstStub->toCacheIRStub());
      buf.printf("]");
    }
    first = false;
  }
  buf.printf("]}");
  WriteMonoEvent(buf);
}

void js::jit::LogMonoExit(JSScript* script, BaselineScript* baselineScript,
                          uint32_t pcOffset) {
  if (!MonoStatsEnabled()) {
    return;
  }
  MonoEventBuffer buf;
  buf.printf("{\"ev\":\"exit\",\"t\":%" PRId64 ",\"id\":%u,", MonoStatsNowNs(),
             baselineScript->monoStatsId());
  PrintMonoScript(buf, script);
  buf.printf(",\"pc\":%u,\"wu\":%u}", pcOffset, script->getWarmUpCount());
  WriteMonoEvent(buf);
}

void js::jit::LogMonoScriptEvent(const char* event, JSScript* script,
                                 BaselineScript* baselineScript) {
  if (!MonoStatsEnabled()) {
    return;
  }
  MonoEventBuffer buf;
  buf.printf("{\"ev\":\"%s\",\"t\":%" PRId64 ",\"id\":%u", event,
             MonoStatsNowNs(), baselineScript->monoStatsId());
  if (script) {
    buf.printf(",");
    PrintMonoScript(buf, script);
  }
  buf.printf("}");
  WriteMonoEvent(buf);
}

void js::jit::LogMonoDiscardBlocked(JSScript* script,
                                    BaselineScript* baselineScript,
                                    uint32_t blocked, uint32_t monoFrames,
                                    uint32_t jitFrames) {
  if (!MonoStatsEnabled()) {
    return;
  }
  MonoEventBuffer buf;
  buf.printf("{\"ev\":\"blocked\",\"t\":%" PRId64 ",\"id\":%u,",
             MonoStatsNowNs(), baselineScript->monoStatsId());
  PrintMonoScript(buf, script);
  buf.printf(",\"n\":%u,\"frames\":%u,\"depth\":%u}", blocked, monoFrames,
             jitFrames);
  WriteMonoEvent(buf);
}

void js::jit::LogMonoAttach(JSScript* script, ICScript* icScript,
                            ICFallbackStub* fallback, ICCacheIRStub* stub,
                            bool folded) {
  if (!MonoStatsEnabled()) {
    return;
  }
  const ICState& state = fallback->state();
  MonoEventBuffer buf;
  buf.printf("{\"ev\":\"attach\",\"t\":%" PRId64 ",", MonoStatsNowNs());
  PrintMonoScript(buf, script);
  buf.printf(",\"pc\":%u,\"inlined\":%d,\"folded\":%d,\"n\":%u,\"mode\":%u,"
             "\"kind\":%u,\"baseline\":%d,\"ops\":",
             fallback->pcOffset(), int(icScript->isInlined()), int(folded),
             unsigned(state.numOptimizedStubs()), unsigned(state.mode()),
             unsigned(stub->stubInfo()->kind()),
             int(script->hasBaselineScript()));
  PrintMonoStubOps(buf, stub);
  buf.printf("}");
  WriteMonoEvent(buf);
}

bool js::jit::MonoCodeMapEnabled() {
#ifdef XP_UNIX
  static const char* dir = getenv("JS_MONO_CODEMAP");
  if (!dir || !*dir) {
    return false;
  }
  pthread_mutex_lock(&gMonoStatsLock);
  pid_t pid = getpid();
  if (pid != gMonoCodeMapPid) {
    gMonoCodeMapPid = pid;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/codemap-%d.txt", dir, int(pid));
    gMonoCodeMap =
        open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  }
  bool enabled = gMonoCodeMap >= 0;
  pthread_mutex_unlock(&gMonoStatsLock);
  return enabled;
#else
  return false;
#endif
}

void js::jit::LogMonoCode(const void* start, size_t bytes, const char* kind,
                          const char* name) {
#ifdef XP_UNIX
  if (!MonoCodeMapEnabled()) {
    return;
  }
  char line[768];
  int length = snprintf(line, sizeof(line), "%" PRId64 " %p %zu %s %s\n",
                        MonoStatsNowNs(), start, bytes, kind, name);
  if (length <= 0) {
    return;
  }
  length = std::min(length, int(sizeof(line) - 1));
  line[length - 1] = '\n';
  pthread_mutex_lock(&gMonoStatsLock);
  if (gMonoCodeMap >= 0 && ::write(gMonoCodeMap, line, length) < 0) {
    gMonoCodeMap = -1;
  }
  pthread_mutex_unlock(&gMonoStatsLock);
#endif
}

void js::jit::LogMonoBaselineCode(JSScript* script,
                                  BaselineScript* baselineScript,
                                  uint32_t exitsStart, uint32_t exitsEnd,
                                  mozilla::Span<const MonoStatsIC> ics) {
  if (!MonoCodeMapEnabled()) {
    return;
  }
  JitCode* code = baselineScript->method();
  char name[600];
  snprintf(name, sizeof(name), "%u %u:%u %s:%u:%u",
           baselineScript->monoStatsId(), script->scriptSource()->id(),
           script->sourceStart(), script->filename() ? script->filename() : "",
           script->lineno(), script->column().oneOriginValue());
  LogMonoCode(code->raw(), code->instructionsSize(),
              baselineScript->isMono() ? "mono" : "baseline", name);
  for (const MonoStatsIC& ic : ics) {
    if (ic.end <= ic.start) {
      continue;
    }
    const char* kind;
    switch (ic.disposition) {
      case MonoICDisposition::Inline:
        kind = "ic-inline";
        break;
      case MonoICDisposition::Copy:
        kind = "ic-copy";
        break;
      default:
        kind = "ic-chain";
        break;
    }
    snprintf(name, sizeof(name), "%u %u", baselineScript->monoStatsId(),
             ic.pcOffset);
    LogMonoCode(code->raw() + ic.start, ic.end - ic.start, kind, name);
  }
  if (exitsEnd > exitsStart) {
    snprintf(name, sizeof(name), "%u", baselineScript->monoStatsId());
    LogMonoCode(code->raw() + exitsStart, exitsEnd - exitsStart, "mono-exits",
                name);
  }
}
