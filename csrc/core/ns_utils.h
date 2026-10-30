/// Copyright 2026 Theo Lincke
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

#ifndef UTILS_H
#define UTILS_H

#include "core/ns_platform.h" // HEADER_FUNC
#include "core/ns_stdtypes.h" // u32

////////////////////////////////////////////////////////////
// SIMPLE UTILITIES

const char *file_basename (const char *path);

// Good format prefix string with useful information
#define FPREFIX_STR  "%s:%d (%s):             "
#define FPREFIX_ARGS file_basename (__FILE__), __LINE__, __func__

// Container of struct elements using offset
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof (type, member)))

#define is_alpha(c)         (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || (c) == '_')
#define is_num(c)           ((c) >= '0' && (c) <= '9')
#define is_alpha_num(c)     (is_alpha (c) || is_num (c))
#define is_friendly_punc(c) ((c) == '.' || (c) == '/' || (c) == '-')
#define is_alpha_num_generous(c) (is_alpha (c) || is_num (c) || is_friendly_punc (c))

#define arrlen(a) (sizeof (a) / sizeof (*(a)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define STRINGIFY(x) #x
#define TOSTRING(x)  STRINGIFY (x)

#define case_ENUM_RETURN_STRING(en) \
  case en: return #en

#define CJOIN2(val) val, val
#define CJOIN3(val) val, val, val
#define CJOIN4(val) val, val, val, val
#define CJOIN5(val) val, val, val, val, val

HEADER_FUNC u32
snprintf_len (const int n)
{
  return n > 0 ? (u32)n : 0; // LCOV_EXCL_BR_LINE - the negative side is dead
}

#ifndef RH__CAT
#  define RH__CAT(a, b)  a##b
#  define RH__XCAT(a, b) RH__CAT (a, b)
#endif

#endif // UTILS_H
