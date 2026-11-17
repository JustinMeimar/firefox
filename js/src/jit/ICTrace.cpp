/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ICTrace.h"

#include "mozilla/Sprintf.h"

#include <stdio.h>
#include <unistd.h>

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/MIR.h"
#include "js/HashTable.h"
#include "threading/Mutex.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSScript.h"
#include "vm/MutexIDs.h"

#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"

using namespace js;
using namespace js::jit;

// Little-endian records. Strings are a u32 length followed by bytes.
//
// Header: "ICTRACE1",
//   u16 count, {u8 length, u8 flags, str name} per JSOp, where flags are
//     1 jump, 2 jump target, 4 no fallthrough, 8 has IC,
//   u16 count, {u8 argLength, u8 transpile, str name} per CacheOp,
//   u16 count, {str name} per CacheKind.
// 'S' script: u64 scriptId, str filename, u32 line, u32 column,
//   u32 count, {u32 offset, u8 op, u16 uses, u16 defs} per op,
//   u32 length, bytecode.
// 'F' fallback: u64 frame, u64 icScript, u64 fallback, u64 scriptId,
//   u32 pcOffset.
// 'A' attach: u64 stub, u64 icScript, u64 fallback, u32 pcOffset, u8 kind,
//   u32 length, CacheIR code.
// 'C' clone: u64 newStub, u64 oldStub.
// 'E' stub entry: u64 stub, u64 frame.
// 'X' stub guard failure: u64 stub, u64 frame.
// 'B' baseline stub compiler emitted a CacheOp: u16 cacheOp, u8 makesGCCalls
//   (the op entered a stub frame to call into the VM or JIT code).
// 'T' Warp starts transpiling a CacheOp: u16 cacheOp.
// 'M' MIR node added while Warp transpiles a CacheOp: u16 cacheOp,
//   str opcode, u32 alias set (bit 31 store), u8 bits where
//   1 effectful, 2 possibly calls, 4 guard.
//
// Frames are identified by the baseline frame pointer.

namespace {

struct ICTraceState {
  FILE* file = nullptr;
  HashSet<uint64_t, DefaultHasher<uint64_t>, SystemAllocPolicy> seenScripts;
};

ICTraceState* gState = nullptr;
Mutex* gLock = nullptr;

void WriteBytes(const void* p, size_t n) { fwrite(p, 1, n, gState->file); }

template <typename T>
void Write(T v) {
  WriteBytes(&v, sizeof(T));
}

void WritePtr(const void* p) { Write(uint64_t(uintptr_t(p))); }

void WriteString(const char* s) {
  uint32_t n = s ? strlen(s) : 0;
  Write(n);
  WriteBytes(s, n);
}

void WriteHeader() {
  WriteBytes("ICTRACE1", 8);

  Write(uint16_t(JSOP_LIMIT));
  for (size_t i = 0; i < JSOP_LIMIT; i++) {
    JSOp op = JSOp(i);
    Write(CodeSpec(op).length);
    Write(uint8_t(
        (IsJumpOpcode(op) ? 1 : 0) | (BytecodeIsJumpTarget(op) ? 2 : 0) |
        (BytecodeFallsThrough(op) ? 0 : 4) | (BytecodeOpHasIC(op) ? 8 : 0)));
    WriteString(CodeName(op));
  }

  Write(uint16_t(CacheOp::NumOpcodes));
  for (size_t i = 0; i < size_t(CacheOp::NumOpcodes); i++) {
    Write(uint8_t(CacheIROpInfos[i].argLength));
    Write(uint8_t(CacheIROpInfos[i].transpile));
    WriteString(CacheIROpNames[i]);
  }

  size_t numKinds = 0;
#define COUNT_KIND(kind) numKinds++;
  CACHE_IR_KINDS(COUNT_KIND)
#undef COUNT_KIND
  Write(uint16_t(numKinds));
  for (size_t i = 0; i < numKinds; i++) {
    WriteString(CacheKindNames[i]);
  }
}

uint64_t ScriptId(JSScript* script) {
  return (uint64_t(script->scriptSource()->id()) << 32) | script->sourceStart();
}

void MaybeWriteScript(JSScript* script, uint64_t id) {
  auto p = gState->seenScripts.lookupForAdd(id);
  if (p) {
    return;
  }
  if (!gState->seenScripts.add(p, id)) {
    return;
  }

  Write('S');
  Write(id);
  WriteString(script->filename());
  Write(uint32_t(script->lineno()));
  Write(uint32_t(script->column().oneOriginValue()));

  uint32_t numOps = 0;
  for (BytecodeLocation loc : AllBytecodesIterable(script)) {
    (void)loc;
    numOps++;
  }
  Write(numOps);
  for (BytecodeLocation loc : AllBytecodesIterable(script)) {
    Write(uint32_t(loc.bytecodeToOffset(script)));
    Write(uint8_t(loc.getOp()));
    Write(uint16_t(loc.useCount()));
    Write(uint16_t(loc.defCount()));
  }

  Write(uint32_t(script->length()));
  WriteBytes(script->code(), script->length());
}

}  // namespace

bool jit::ICTraceOpen(const char* path) {
  MOZ_ASSERT(!gState);
  char expanded[4096];
  const char* pid = strstr(path, "%p");
  if (pid) {
    SprintfLiteral(expanded, "%.*s%u%s", int(pid - path), path,
                   unsigned(getpid()), pid + 2);
    path = expanded;
  }
  FILE* file = fopen(path, "wb");
  if (!file) {
    return false;
  }
  setvbuf(file, nullptr, _IOFBF, 1 << 20);
  gState = js_new<ICTraceState>();
  gLock = js_new<Mutex>(mutexid::ICTrace);
  if (!gState || !gLock) {
    fclose(file);
    return false;
  }
  gState->file = file;
  WriteHeader();
  return true;
}

void jit::ICTraceClose() {
  if (!gState) {
    return;
  }
  fclose(gState->file);
  js_delete(gState);
  js_delete(gLock);
  gState = nullptr;
  gLock = nullptr;
}

bool jit::ICTraceEnabled() { return !!gState; }

void jit::ICTraceStubEntry(ICCacheIRStub* stub, void* framePtr) {
  AutoUnsafeCallWithABI unsafe;
  LockGuard<Mutex> guard(*gLock);
  Write('E');
  WritePtr(stub);
  WritePtr(framePtr);
}

void jit::ICTraceStubFailure(ICCacheIRStub* stub, void* framePtr) {
  AutoUnsafeCallWithABI unsafe;
  LockGuard<Mutex> guard(*gLock);
  Write('X');
  WritePtr(stub);
  WritePtr(framePtr);
}

void jit::ICTraceFallback(BaselineFrame* frame, ICFallbackStub* stub) {
  if (!gState) {
    return;
  }
  JSScript* script = frame->script();
  uint64_t id = ScriptId(script);
  uint8_t* framePtr = reinterpret_cast<uint8_t*>(frame) + BaselineFrame::Size();

  LockGuard<Mutex> guard(*gLock);
  MaybeWriteScript(script, id);
  Write('F');
  WritePtr(framePtr);
  WritePtr(frame->icScript());
  WritePtr(stub);
  Write(id);
  Write(stub->pcOffset());
}

void jit::ICTraceAttach(ICCacheIRStub* stub, ICScript* icScript,
                        ICFallbackStub* fallback, CacheKind kind) {
  if (!gState) {
    return;
  }
  const CacheIRStubInfo* info = stub->stubInfo();

  LockGuard<Mutex> guard(*gLock);
  Write('A');
  WritePtr(stub);
  WritePtr(icScript);
  WritePtr(fallback);
  Write(fallback->pcOffset());
  Write(uint8_t(kind));
  Write(info->codeLength());
  WriteBytes(info->code(), info->codeLength());
}

void jit::ICTraceClone(ICCacheIRStub* newStub, ICCacheIRStub* oldStub) {
  if (!gState) {
    return;
  }
  LockGuard<Mutex> guard(*gLock);
  Write('C');
  WritePtr(newStub);
  WritePtr(oldStub);
}

void jit::ICTraceBaselineOp(CacheOp op, bool makesGCCalls) {
  LockGuard<Mutex> guard(*gLock);
  Write('B');
  Write(uint16_t(op));
  Write(uint8_t(makesGCCalls));
}

void jit::ICTraceTranspile(CacheOp op) {
  LockGuard<Mutex> guard(*gLock);
  Write('T');
  Write(uint16_t(op));
}

static const char* MIROpcodeName(MDefinition::Opcode op) {
  static const char* const names[] = {
#define NAME(x) #x,
      MIR_OPCODE_LIST(NAME)
#undef NAME
  };
  return names[unsigned(op)];
}

void jit::ICTraceMIR(CacheOp op, MInstruction* ins) {
  AliasSet alias = ins->getAliasSet();
  uint32_t aliasBits = alias.flags() | (alias.isStore() ? 1u << 31 : 0);
  uint8_t bits = (ins->isEffectful() ? 1 : 0) | (ins->possiblyCalls() ? 2 : 0) |
                 (ins->isGuard() ? 4 : 0);

  LockGuard<Mutex> guard(*gLock);
  Write('M');
  Write(uint16_t(op));
  WriteString(MIROpcodeName(ins->op()));
  Write(aliasBits);
  Write(bits);
}
