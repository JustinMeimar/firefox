/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <cstdint>

#include "gc/Zone.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/JitCode.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/ProcessExecutableMemory.h"
#include "jit/VMFunctions.h"
#include "vm/JSContext.h"

namespace js::jit {

static void* ResolveCppFunction(AOTCppFunctionId id) {
  switch (id) {
    case AOTCppFunctionId::PostWriteBarrier:
      return reinterpret_cast<void*>(PostWriteBarrier);
    case AOTCppFunctionId::FrameIsDebuggeeCheck:
      return reinterpret_cast<void*>(FrameIsDebuggeeCheck);
    case AOTCppFunctionId::HandleCodeCoverageAtPrologue:
      return reinterpret_cast<void*>(HandleCodeCoverageAtPrologue);
    case AOTCppFunctionId::HandleCodeCoverageAtPC:
      return reinterpret_cast<void*>(HandleCodeCoverageAtPC);
    case AOTCppFunctionId::Count:
      break;
  }
  MOZ_CRASH("Unknown AOTCppFunctionId");
}

uintptr_t RuntimePatch::getValueToPatch(const PatchContext& pc) const {
  // Patch-only kinds require union data that ResolveAOTReloc doesn't have.
  switch(kind) {
    case AOTRelocKind::DispatchTable:
      return (uintptr_t)(pc.codeBase + handlerOffset);
    case AOTRelocKind::VMWrapper: {
      TrampolinePtr ptr = pc.cx->runtime()->jitRuntime()->getVMWrapper(vmId);
      return (uintptr_t)(ptr.value);
    }
    case AOTRelocKind::DebugTrapHandler:
      return (uintptr_t)pc.cx->runtime()->jitRuntime()->debugTrapHandler(dbgKind)->raw();
    case AOTRelocKind::CppFunction:
      return (uintptr_t)ResolveCppFunction(cppFnId);
    default:
      // All other kinds resolve identically to ResolveAOTReloc.
      return ResolveAOTReloc(kind, pc.cx);
  }
}

void RuntimePatch::apply(const PatchContext& pc) const {
  uintptr_t val = getValueToPatch(pc);
  uint8_t* target = pc.codeBase + targetOffset;
#ifdef DEBUG
  uintptr_t beforeValue = *reinterpret_cast<uintptr_t*>(target);
  JitSpew(JitSpew_BaselineAOT, "Runtime patch [%s] @ offset %u: before=0x%016lx after=0x%016lx",
          AOTRelocKindName(kind), targetOffset, beforeValue, val);
#endif
  *reinterpret_cast<uintptr_t*>(target) = val;
}

JitCode* AllocateAndPatchAOTCode(JSContext* cx,
                                 const AOTBlobDirectoryEntry* entry,
                                 const uint8_t* containerBase,
                                 uint32_t headerSize, CodeKind codeKind,
                                 uint32_t dispatchTableOffset) {
  uint32_t codeSize = entry->codeSize;

  mozilla::Maybe<AutoAllocInAtomsZone> az;
  if (!cx->zone() || !cx->zone()->isAtomsZone()) {
    az.emplace(cx);
  }

  JitZone* jitZone = cx->zone()->getJitZone(cx);
  if (!jitZone) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  size_t bytesNeeded = js::AlignBytes(codeSize + headerSize, sizeof(void*));
  ExecutablePool* pool;
  auto* result = (uint8_t*)jitZone->execAlloc().alloc(cx, bytesNeeded, &pool,
                                                       codeKind);
  if (!result) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  uint8_t* codeStart = result + headerSize;
  JitCode* code =
      JitCode::New<NoGC>(cx, codeStart, bytesNeeded, headerSize, pool,
                          codeKind);
  if (!code) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  {
    AutoWritableJitCodeFallible writable(code);
    if (!writable.makeWritable()) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    memcpy(codeStart, containerBase + entry->codeOffset, codeSize);
    JitCodeHeader::FromExecutable(codeStart)->init(code);
    code->setInstructionsSize(codeSize);

    if (entry->patchesCount > 0) {
      PatchContext patchCtx({cx, codeStart, dispatchTableOffset});
      const auto* patches = reinterpret_cast<const RuntimePatch*>(
          containerBase + entry->patchesOffset);
      for (uint32_t i = 0; i < entry->patchesCount; i++) {
        patches[i].apply(patchCtx);
      }
      JitSpew(JitSpew_BaselineAOT, "Applied %u patches.", entry->patchesCount);
    }
  }

  return code;
}

}  // namespace js::jit
