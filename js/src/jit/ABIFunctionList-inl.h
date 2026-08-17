/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIFunctionList_inl_h
#define jit_ABIFunctionList_inl_h

#include "jit/ABIFunctionList.h"

#include "mozilla/MacroArgs.h"  // MOZ_CONCAT
#include "mozilla/SIMD.h"       // mozilla::SIMD::memchr{,2x}{8,16}

#include "builtin/Array.h"      // js::ArrayShiftMoveElements
#include "builtin/MapObject.h"  // js::MapIteratorObject::next,
                                // js::SetIteratorObject::next
#include "builtin/Math.h"  // js::ecmaPow, js::ecmaHypot, js::hypot3, js::hypot4,
                           // js::ecmaAtan2, js::UnaryMathFunctionType, js::powi
#include "builtin/Number.h"   // js::StringToNumberPure, js::Int32ToStringPure,
                              // js::NumberToStringPure
#include "builtin/Object.h"   // js::ObjectClassToString
#include "builtin/RegExp.h"   // js::RegExpPrototypeOptimizableRaw,
                              // js::RegExpInstanceOptimizableRaw
#include "builtin/Sorting.h"  // js::ArraySortData
#include "builtin/TestingFunctions.h"  // js::FuzzilliHash*
#include "builtin/WeakMapObject.h"     // js::WeakMapObject::{get,has}Object
#include "builtin/WeakSetObject.h"     // js::WeakSetObject::hasObject
#include "irregexp/RegExpAPI.h"
// js::irregexp::CaseInsensitiveCompareNonUnicode,
// js::irregexp::CaseInsensitiveCompareUnicode,
// js::irregexp::GrowBacktrackStack,
// js::irregexp::IsCharacterInRangeArray

#include "jit/ABIFunctions.h"
#include "jit/Bailouts.h"  // js::jit::FinishBailoutToBaseline, js::jit::Bailout,
                           // js::jit::InvalidationBailout

#include "jit/Ion.h"          // js::jit::LazyLinkTopActivation
#include "jit/JitFrames.h"    // HandleException
#include "jit/VMFunctions.h"  // Rest of js::jit::* functions.

#include "js/CallArgs.h"     // JSNative
#include "js/Conversions.h"  // JS::ToInt32
// JSJitGetterOp, JSJitSetterOp, JSJitMethodOp
#include "js/experimental/JitInfo.h"

#include "proxy/Proxy.h"          // js::ProxyGetProperty
#include "util/PortableMath.h"    // js::NumberMod
#include "vm/ArgumentsObject.h"   // js::ArgumentsObject::finishForIonPure
#include "vm/Interpreter.h"       // js::TypeOfObject
#include "vm/NativeObject.h"      // js::NativeObject
#include "vm/RegExpShared.h"      // js::ExecuteRegExpAtomRaw
#include "vm/TypedArrayObject.h"  // js::TypedArraySortFromJit
#include "wasm/WasmBuiltins.h"    // js::wasm::*

#include "builtin/Boolean-inl.h"  // js::EmulatesUndefined

namespace js {

namespace wasm {

class AnyRef;

}  // namespace wasm

namespace jit {

// GCC warns when the signature does not have matching attributes (for example
// [[nodiscard]]). Squelch this warning to avoid a GCC-only footgun.
#if MOZ_IS_GCC
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

#define DEF_TEMPLATE(fp)                      \
  template <>                                 \
  struct ABIFunctionData<decltype(&fp), fp> { \
    static constexpr bool registered = true;  \
  };
ABIFUNCTION_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

#define DEF_TEMPLATE(fp, ...)                \
  template <>                                \
  struct ABIFunctionData<__VA_ARGS__, fp> {  \
    static constexpr bool registered = true; \
  };
ABIFUNCTION_AND_TYPE_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

// Define a known list of function signatures.
#define DEF_TEMPLATE(...)                        \
  template <>                                    \
  struct ABIFunctionSignatureData<__VA_ARGS__> { \
    static constexpr bool registered = true;     \
  };
ABIFUNCTIONSIG_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

#if MOZ_IS_GCC
#  pragma GCC diagnostic pop
#endif

}  // namespace jit
}  // namespace js

// Make sure that all names are fully qualified (or at least, are resolvable
// within the toplevel namespace).
//
// Previously this was accomplished just by using `::fp` to force resolution
// within the toplevel namespace, but (1) that prevented using templated
// functions with more than one parameter (eg `void foo<T, U>`) because the
// macro split on the comma and wrapping it in parens doesn't work because
// `::(foo)` is invalid; and (2) that would only check the function name itself,
// not eg template parameters.
namespace check_fully_qualified {
#define CHECK_NS_VISIBILITY(fp)                               \
  [[maybe_unused]] static constexpr decltype(&fp) MOZ_CONCAT( \
      fp_, __COUNTER__) = nullptr;
ABIFUNCTION_LIST(CHECK_NS_VISIBILITY)
#undef CHECK_NS_VISIBILITY
}  // namespace check_fully_qualified

#endif  // jit_VMFunctionList_inl_h
