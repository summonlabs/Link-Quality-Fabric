// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LQF_CORE_ASSERT_HPP
#define LQF_CORE_ASSERT_HPP

#include <cstdio>
#include <cstdlib>

// Invariant checks for conditions that are programming errors, never inputs.
// Debug builds trap; release builds are compiled out unless LQF_ENABLE_ASSERTS
// is defined explicitly.
#if defined(NDEBUG) && !defined(LQF_ENABLE_ASSERTS)
#define LQF_ASSERT_MSG(cond, msg) ((void)0)
#define LQF_ASSERT(cond) ((void)0)
#else
#define LQF_ASSERT_MSG(cond, msg)                                                          \
  do {                                                                                     \
    if (!(cond)) {                                                                         \
      std::fprintf(stderr, "LQF invariant violation: %s\n  at %s:%d\n", (msg), __FILE__, \
                   __LINE__);                                                              \
      std::abort();                                                                        \
    }                                                                                      \
  } while (false)
#define LQF_ASSERT(cond) LQF_ASSERT_MSG((cond), #cond)
#endif

#endif  // LQF_CORE_ASSERT_HPP
