/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_String_h
#define builtin_String_h

#include "NamespaceImports.h"

#include "js/RootingAPI.h"
#include "js/Value.h"

namespace js {

class ArrayObject;
class GlobalObject;

/* Initialize the String class, returning its prototype object. */
extern JSObject* InitStringClass(JSContext* cx, Handle<GlobalObject*> global);

// String methods exposed so they can be installed in the self-hosting global.

extern bool str_fromCharCode(JSContext* cx, unsigned argc, Value* vp);

extern bool str_fromCodePoint(JSContext* cx, unsigned argc, Value* vp);

extern bool str_includes(JSContext* cx, unsigned argc, Value* vp);

extern bool str_indexOf(JSContext* cx, unsigned argc, Value* vp);

extern bool str_startsWith(JSContext* cx, unsigned argc, Value* vp);

extern bool str_toString(JSContext* cx, unsigned argc, Value* vp);

extern bool str_charCodeAt(JSContext* cx, unsigned argc, Value* vp);

extern bool str_codePointAt(JSContext* cx, unsigned argc, Value* vp);

extern bool str_endsWith(JSContext* cx, unsigned argc, Value* vp);

extern "C" ArrayObject* StringSplitString(JSContext* cx, HandleString str,
                                          HandleString sep, uint32_t limit);

extern "C" JSString* StringFlatReplaceString(JSContext* cx, HandleString string,
                                             HandleString pattern,
                                             HandleString replacement);

JSString* str_replace_string_raw(JSContext* cx, HandleString string,
                                 HandleString pattern,
                                 HandleString replacement);

JSString* str_replaceAll_string_raw(JSContext* cx, HandleString string,
                                    HandleString pattern,
                                    HandleString replacement);

extern "C" bool StringIncludes(JSContext* cx, HandleString string,
                               HandleString searchString, bool* result);

extern "C" bool StringIndexOf(JSContext* cx, HandleString string,
                              HandleString searchString, int32_t* result);

extern "C" bool StringLastIndexOf(JSContext* cx, HandleString string,
                                  HandleString searchString, int32_t* result);

extern "C" bool StringStartsWith(JSContext* cx, HandleString string,
                                 HandleString searchString, bool* result);

extern "C" bool StringEndsWith(JSContext* cx, HandleString string,
                               HandleString searchString, bool* result);

extern "C" JSLinearString* StringToLowerCase(JSContext* cx, JSString* string);

extern "C" JSLinearString* StringToUpperCase(JSContext* cx, JSString* string);

extern "C" JSString* StringTrim(JSContext* cx, HandleString string);

extern "C" JSString* StringTrimStart(JSContext* cx, HandleString string);

extern "C" JSString* StringTrimEnd(JSContext* cx, HandleString string);

extern bool StringConstructor(JSContext* cx, unsigned argc, Value* vp);

extern bool FlatStringMatch(JSContext* cx, unsigned argc, Value* vp);

extern bool FlatStringSearch(JSContext* cx, unsigned argc, Value* vp);

extern "C" JSLinearString* StringFromCharCode(JSContext* cx, int32_t charCode);

extern "C" JSLinearString* StringFromCodePoint(JSContext* cx,
                                               char32_t codePoint);

#if JS_HAS_INTL_API
bool LocaleHasDefaultCaseMapping(const char* locale);
#endif

} /* namespace js */

#endif /* builtin_String_h */
