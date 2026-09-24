# Link Quality Fabric

A vendor-neutral runtime for evidence-backed signal, error, degradation and
quality state beyond binary link up/down.

Link Quality Fabric answers one question: for this exact link, generation,
interval and evidence set, what quality state is actually supported, what
changed, and how certain and fresh is that conclusion? It is a C++20 library
plus a command line runtime and a framed loopback transport. Everything in this
README is implemented and verified by tests in this repository.

## Systems boundary

Link Quality Fabric owns **quality interpretation**: metric capability
declaration, evidence ingestion, counter continuity, derived quality state,
threshold policy generations, conflict and freshness inspection, and versioned
recovery.

It does not, and by design will not:

* own or assert binary link authority (up/down),
* decide route eligibility or path planning,
* remediate, program ports, or switch optics,
* own transceiver inventory or cable attachment records,
* invent a quality state for evidence it does not have.

Those are separate systems. A link state hint that arrives as evidence is a
measurement like any other: it is classified by the published policy, never
treated as authority.

## What it guarantees

| Guarantee | How it is enforced and proved |
| --- | --- |
| A measurement is not a diagnosis | Raw observations are stored unchanged; every derived state is a separate value carrying the evidence references, the policy generation and the rule that produced it. |
| Missing evidence is never zero | A metric with no evidence reports `unknown`; a metric family nobody declared reports `unsupported`; no value is invented for either. |
| Stale evidence cannot assert current health | Freshness is computed at query time from the monotonic receive clock of the running process. Recovered evidence is structurally incapable of being fresh because it carries the previous process incarnation. |
| Counter semantics never fabricate a delta | A decrease is a delta only when it is proven to be a modular wrap of a declared width at the near-boundary; otherwise it is `decrease-unproven` with no delta at all. |
| Equal-authority disagreement stays conflicting | Sources at the same authority that differ beyond the declared tolerance produce `conflicting` with both provenances preserved and no representative value. A strictly higher authority wins and the ignored authority is reported. |
| Threshold generations reclassify without rewriting evidence | Publishing appends an immutable generation with a SHA-256 content hash; the same evidence reclassifies deterministically and the stored evidence compares equal before and after. |
| Extreme values cannot overflow a classification | Non-finite samples are refused at the boundary; band bounds are total over the extended reals; rate derivation refuses intervals that cannot produce a finite rate; a 64-bit counter maximum yields a finite rate. |
| Persistence is versioned, integrity-checked and conservative | A header with a format version and checksum, per-record CRC-32C, monotonic ordinals, a torn tail repaired only when nothing intact follows it, corruption followed by intact records refused outright, and compaction through an atomic replace that never leaves a partial file authoritative. |
| Bounded everything | Links, generations, sources, streams, retained records, deduplication windows, batch sizes, frames, decoder strings and item counts, journal queue depth, journal record size and recovery scan size all have declared limits that fail loudly. |
| Real cancellation and shutdown | Ingestion and query paths never block on persistence; the journal queue refuses instead of blocking; the transport waits on a wakeup channel that a stop signal actually fires, so a stop returns promptly instead of waiting for a socket to time out. |
| Lock correctness is audited, not assumed | First-party locks are tracked locks: re-entrant acquisition and lock-order inversions are detected before they can deadlock and are reported through `lqf::locks::violation_report`. The audit is proved able to fail by deliberately tripping it, then asserted clean across the concurrent suites. |

## Build

Requirements: CMake 3.21+, a C++20 compiler, and threads. On Windows, MSVC
17.10+ from a developer prompt with the compiler on PATH.

    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release
    ctest --test-dir build/release --output-on-failure

CMake presets are provided: `release`, `debug`, `asan` and `vs2022`.

First-party code builds warning-clean with /W4 /WX /permissive- on MSVC and
-Wall -Wextra -Werror plus the strict set in
`cmake/LinkQualityFabricWarnings.cmake` elsewhere.

Options: `LQF_BUILD_APPS`, `LQF_BUILD_TESTS`, `LQF_BUILD_BENCHMARKS`,
`LQF_BUILD_EXAMPLES`, `LQF_WARNINGS_AS_ERRORS`, `LQF_LOCK_TRACKING`,
`LQF_ENABLE_ANALYZE`, `LQF_SANITIZE`.

## Install and consume

    cmake --install build/release --prefix /somewhere
    # in another project
    find_package(LinkQualityFabric 1.0 REQUIRED)
    target_link_libraries(app PRIVATE lqf::lqf)

The installed package exports `lqf::lqf`, the headers, the `lqf` command line
tool and the license files. A downstream project that only uses the installed
package is part of the release validation.

## Quick start

    lqf serve --journal state.lqf --port 9000 --shutdown-token secret
    lqf emit --port 9000 --link link-a --source src-a --count 5 --synthetic
    lqf query --port 9000 --link link-a
    lqf explain --port 9000 --link link-a
    lqf inspect --port 9000
    lqf metrics --port 9000 --link link-a
    lqf policy --port 9000
    lqf journal-verify --journal state.lqf
    lqf journal-dump --journal state.lqf --limit 20
    lqf shutdown --port 9000 --token secret

Embedded use is the same code path:

    #include "lqf/runtime/fabric.hpp"

    lqf::FabricConfig config;
    config.journal.enabled = true;
    config.journal.path = "state.lqf";
    auto opened = lqf::Fabric::open(config);
    std::shared_ptr<lqf::Fabric> fabric = opened.value();

    fabric->declare_capability(declaration);
    fabric->ingest(observation);
    lqf::LinkQualityReport report = fabric->query(query).value();

## Exposed operations

| Operation | Library | Transport |
| --- | --- | --- |
| Metric capability registration and inspection | `declare_capability`, `capabilities` | `RegisterCapability`, `Capabilities` |
| Evidence ingestion | `ingest`, `ingest_batch` | `Ingest` |
| Link quality query | `query` | `Query` |
| Historical window query, with optional as-of assessment | `window` | `Window` |
| Derivation and explanation | `explain` | `Explain` |
| Threshold policy generation publication and retrieval | `publish_policy`, `policy_document` | `PublishPolicy`, `PolicyDocument` |
| Conflict and freshness inspection | `inspect` | `Inspect` |
| Persistence and recovery | `open`, `flush`, `compact`, `close`, `recovery_report` | `Flush`, `Compact` |

## Evidence model

A metric identity is a family plus a canonical dotted name. The built-in
catalog is vendor-neutral: `signal-power:rx.level`, `signal-ratio:snr`,
`error-counter:errors.uncorrectable`, `loss-ratio:loss.ratio`,
`timing:latency.roundtrip`, `environmental:temperature` and others. Nothing in
the catalog names a vendor, and no vendor-specific value is ever synthesized.

Every observation carries the link identity and generation, the source identity
and incarnation, a sequence number, the metric, the lane dimension, explicit
units, the sample semantics, the reading, the source observation time, an
authority rank and provenance (transport kind, evidence class, origin, producer,
clock synchronization).

Gauges are values with an explicit validity window. Counters are cumulative
readings whose only meaningful derivations are differences, and only when
continuity is proven.

## Counter continuity

For one link generation, one metric, one lane and one source incarnation, a
counter stream tracks its baseline and reports exactly one event per step:

| Event | Meaning |
| --- | --- |
| `baseline` | First reading of an incarnation: no delta exists. |
| `advance` | Value at or above the previous one: the exact non-negative difference. |
| `wrap` | A decrease proven to be a modular wrap at the declared width boundary, with the exact modular difference. |
| `reset` | The source declared a reset: re-baselined, no delta. |
| `decrease-unproven` | A decrease that cannot be proven to be a wrap: continuity broken, no delta. |
| `width-changed`, `source-reincarnated`, `link-generation-changed`, `out-of-range`, `sequence-regressed` | Continuity broken or the reading refused, with the reason recorded. |

A difference that spans more than the configured continuity gap is reported as
an interval average and is **not** used to assert a current rate: that
assessment is `incomplete`, never `healthy`.

## Quality states

`healthy`, `marginal`, `degraded` and `severe` come from the published bands.
`unknown` means no evidence has ever been seen, or no rule classifies the value.
`stale` means evidence exists but nothing fresh enough to assert a current
state. `conflicting` means equal-authority sources disagree. `incomplete` means
evidence exists but cannot support a conclusion (broken continuity, a missing
lane, only one of the two readings a rate needs). `unsupported` means the metric
family is not measured here.

Severity ranks are policy: the defaults are unknown, unsupported, healthy,
incomplete, stale, marginal, degraded, severe, conflicting in increasing
severity, and a document that does not define a total order is refused. Every
state carries machine-readable reason codes and the rule generation that
produced it.

## Benchmarks

    build/release/benchmarks/lqf_bench --observations 2000 --queries 200 --counters 300 --journal-records 128 --batch 32

One run on the development host (not a controlled measurement), synthetic input,
counted from completed work:

| Benchmark | Completed | ops/second |
| --- | --- | --- |
| ingest | 2000 observations accepted and visible in the runtime statistics | about 288000 |
| query | 200 reports returned | about 251000 |
| counter-rate derivation | 300 accepted advances with an admissible rate | about 167000 |
| journal durability | 131 records written with an explicit flush | about 31000 |

The benchmark prints a digest over the produced report states so two runs can be
compared, and states in its own output that all input is synthetic. These
numbers say nothing about hardware.

## Real, synthetic and unsupported

**REAL** - verified here by executed tests: the runtime library, the command line
tool, in-process behaviour, restart and recovery after a process kill, the
independent process tests over loopback TCP, the Release, Debug and
AddressSanitizer builds, the installed CMake package and its downstream
consumer.

**SYNTHETIC** - every benchmark, example and property-test input is generated by
this repository, and each record declares an evidence class of synthetic plus a
provenance label. The class travels with the evidence into every report, so a
generator can never be mistaken for a measurement.

**UNSUPPORTED** - not implemented and never claimed: real switch, ASIC, RDMA,
InfiniBand, NVLink or optical hardware control; vendor telemetry protocols or
MIBs; multi-host fabrics; transceiver inventory; link up/down authority;
remediation. AddressSanitizer leak detection is not available for this toolchain
configuration, so no leak-detection claim is made.

## Documentation

* `docs/architecture.md` - layers, ownership, the concurrency contract and the lock order.
* `docs/semantics.md` - states, bands, counter continuity, freshness, conflicts, confidence.
* `docs/formats.md` - journal format, wire protocol, limits and recovery rules.
* `docs/boundaries.md` - adjacent runtimes and what stays outside this one.
* `docs/validation.md` - what was executed, what was found and fixed, and the limits.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
