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

#ifndef LQF_EXPORT_HPP
#define LQF_EXPORT_HPP

// Shared-library visibility. The library is built as a static archive by
// default; when built shared, only first-party symbols are exported.
#if defined(_WIN32) && defined(LQF_SHARED)
#if defined(LQF_BUILDING_LIBRARY)
#define LQF_API __declspec(dllexport)
#else
#define LQF_API __declspec(dllimport)
#endif
#else
#define LQF_API
#endif

#define LQF_NODISCARD [[nodiscard]]

#endif  // LQF_EXPORT_HPP
