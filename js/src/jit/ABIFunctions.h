/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIFunctions_h
#define jit_ABIFunctions_h

#include <initializer_list>
#include <unordered_map>
#include <vector>
#include "jstypes.h"  // JS_FUNC_TO_DATA_PTR

struct JS_PUBLIC_API JSContext;

namespace JS {
class JS_PUBLIC_API Value;
}

namespace js {
namespace jit {

// This class is used to ensure that all known targets of callWithABI are
// registered here. Otherwise, this would raise a static assertion at compile
// time.
template <typename Sig, Sig fun>
struct ABIFunctionData {
  static const bool registered = false;
#ifdef JS_SPASM
  // This is used to obtain the C++ symbol for all known targets of
  // callWithABI. This is needed in order for spasm produced assembly to refer
  // to the C++ functions and allow the linker to resolve the address.
  static const char* const symbol;
#endif
};

template <typename Sig, Sig fun>
struct ABIFunction {
#ifdef JS_SPASM
  const char* name() const { return ABIFunctionData<Sig, fun>::symbol; }
#else
  void* address() const { return JS_FUNC_TO_DATA_PTR(void*, fun); }
#endif

  // If this assertion fails, you are likely in the context of a
  // `callWithABI<Sig, fn>()` call. This error indicates that ABIFunction has
  // not been specialized for `<Sig, fn>` by the time of this call.
  //
  // This can be fixed by adding the function signature to either
  // ABIFUNCTION_LIST or ABIFUNCTION_AND_TYPE_LIST (if overloaded) within
  // `ABIFunctionList-inl.h` and to add an `#include` statement of this header
  // in the file which is making the call to `callWithABI<Sig, fn>()`.
  static_assert(ABIFunctionData<Sig, fun>::registered,
                "ABI function is not registered.");
};

// Helper to map function ptrs (of a particular type) to their linker symbol
// names.
template <typename Sig>
class ABIFunctionSignatureMap : public std::unordered_map<void*, const char*> {
 public:
  constexpr ABIFunctionSignatureMap(
      std::initializer_list<std::pair<Sig, const char*>> args) {
    this->reserve(args.size());
    for (const std::pair<Sig, const char*>& p : args) {
      this->emplace(JS_DATA_TO_FUNC_PTR(void*, p.first), p.second);
    }
  }
};

template <typename Sig>
struct ABIFunctionSignatureData {
  static const bool registered = false;
#ifdef JS_SPASM
  static const ABIFunctionSignatureMap<Sig> lookupSym;
#endif
};

template <typename Sig>
struct ABIFunctionSignature {
#ifdef JS_SPASM
  const char* name(Sig fun) const {
    auto res = ABIFunctionSignatureData<Sig>::lookupSym.find(
        JS_FUNC_TO_DATA_PTR(void*, fun));
    if (res == ABIFunctionSignatureData<Sig>::lookupSym.end()) {
      MOZ_CRASH(
          "Dynamic function symbol not found in ABIFunctionSignatureData map");
    }
    return res->second;
  }
#else
  void* address(Sig fun) const { return JS_FUNC_TO_DATA_PTR(void*, fun); }
#endif

  // If this assertion fails, you are likely in the context of a
  // `DynamicFunction<Sig>(fn)` call. This error indicates that
  // ABIFunctionSignature has not been specialized for `Sig` by the time of this
  // call.
  //
  // This can be fixed by adding the function signature to ABIFUNCTIONSIG_LIST
  // within `ABIFunctionList-inl.h` and to add an `#include` statement of this
  // header in the file which is making the call to `DynamicFunction<Sig>(fn)`.
  static_assert(ABIFunctionSignatureData<Sig>::registered,
                "ABI function signature is not registered.");
};

// This is a structure created to ensure that the dynamically computed
// function pointer is well typed.
//
// It is meant to be created only through DynamicFunction function calls. In
// extremelly rare cases, such as VMFunctions, it might be produced as a result
// of GetVMFunctionTarget.
struct DynFn {
#ifdef JS_SPASM
  const char* symbol;
#else
  void* address;
#endif
};

#ifdef JS_SIMULATOR
bool CallAnyNative(JSContext* cx, unsigned argc, JS::Value* vp);
const void* RedirectedCallAnyNative();
#endif

}  // namespace jit
}  // namespace js

#endif /* jit_VMFunctions_h */
