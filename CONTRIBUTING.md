# Contributing to Link Quality Fabric

Thank you for your interest in contributing to Link Quality Fabric. This
document describes the contribution terms for this project.

## License

By contributing to this project, you agree that your contributions are
licensed under the **Apache License, Version 2.0**. See the `LICENSE`
file for the full license text and the `NOTICE` file for attribution
and license notices. There is **no separate Contributor License
Agreement (CLA)** requirement: you retain ownership of your
contributions and grant the project a license to use them under the
terms of the Apache License 2.0.

## License headers

New source files should carry the following header:

```
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
```

## Coding standards

- C++20, CMake, MSVC on Windows and GCC/Clang elsewhere.
- Build cleanly with `/W4 /WX /permissive-` on MSVC and
  `-Wall -Wextra -Werror` plus the strict set in
  `cmake/LinkQualityFabricWarnings.cmake` elsewhere. Zero first-party
  warnings is a merge requirement.
- Do not add AI attribution or `Co-authored-by` trailers to commits.
- Time-driven behavior must go through `lqf::Clock`. Tests use
  `lqf::ManualClock`; no test may contain a timeout, a sleep-driven
  assertion, or a watchdog that converts a hang into a pass.
- Every externally derived size, count and offset must be validated before it
  is used for allocation or indexing. Checked arithmetic only.
- New behavior needs a test. Counter, freshness, conflict, policy, persistence
  and transport changes need adversarial and property tests as well.

## Architecture boundaries

- Link Quality Fabric owns *quality interpretation*: capability declarations,
  evidence ingestion, counter continuity, derived quality state, threshold
  policy generations, conflict and freshness inspection, and recovery.
- It does not own binary link authority, route eligibility, path planning,
  remediation, transceiver inventory or optical switching, and must not
  grow into them. Keep new dependencies pointing inward, never outward.
- Unsupported metric families stay `UNSUPPORTED`. Never synthesize a
  vendor-specific signal value, and never turn missing evidence into zero.
- Raw observation and derived classification stay separated. A measurement is
  not a diagnosis, and every derived state must carry the evidence and rule
  generation that produced it.

## Testing

Run the complete suite, with no filters, before submitting:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Also build and run Debug and the AddressSanitizer preset when your change touches
lifetimes, decoders, persistence or concurrency.

## Pull requests

Please keep changes focused, add tests for new behavior, and ensure the
repository builds and tests cleanly in both Release and Debug.
