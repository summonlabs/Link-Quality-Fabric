# Validation

Everything below was executed on the development host (Windows, MSVC 19.44,
Ninja, CMake 4.3.2). No test uses a timeout: the CTest entries carry no TIMEOUT
property, there are no shell or clock wrappers, and a hang is treated as a
defect. The only bounded wait in the suite is a readiness poll for a spawned
child process, and it fails the test rather than passing it.

## Suites

| Suite | What it proves |
| --- | --- |
| `units_metric` | Unit classes and compatibility, metric identity grammar, catalog registration conflict and duplicate rules, vendor-neutral built-in descriptors, counter width semantics. |
| `identity_capability` | Identity grammars, capability validation, revision fencing, declaration scope, source reincarnation, generation fencing and bounded generation eviction. |
| `counter_semantics` | Baseline, advance, proven wrap at both widths, declared reset, unproven decrease, width change, out-of-range, reincarnation, generation change, gaps, missing samples, sequence regression, interval admissibility and extreme values. |
| `counter_property` | A differential property test against an independent reference model over randomized streams with wraps, resets, reincarnations and gaps, plus per-step invariants. Failures print the seed and the reproduction command. |
| `gauge_freshness` | Freshness windows, staleness, unsupported metrics, counter rate prerequisites, resets, capability gating and the observation age rule. |
| `policy_generation` | Document validation (gaps, overlaps, non-total bands, NaN bounds, bad wrap fractions, non-total severity ranks), generation numbering, content hashing, deterministic reclassification and evidence immutability, retention bounds. |
| `classification_extremes` | Half-open band edges, denormals, maximum magnitudes, refused non-finite samples, extreme counter values producing finite rates. |
| `conflicts` | Equal-authority disagreement preserved with both provenances, agreement inside tolerance, authority precedence, stale sources excluded from conflict detection. |
| `window_history` | Window filtering by link, metric, lane, source and time axis; limits and truncation; as-of reconstruction; bounded retention. |
| `explain_derivation` | Derivations naming the policy generation, hash, rule and evidence; byte-identical rendering across instances; policy and capability rendering. |
| `persistence` | Close and reopen, recovered evidence stale but retained with original times, replay refused as a duplicate after restart, torn tail repair, corruption refused, discarded temporary files, compaction round trip preserving state and bounding growth, disabled persistence refusing flush and compaction. |
| `persistence_kill` | A real child process ingests over loopback TCP, is killed without a graceful shutdown, restarted, and then reports recovered-but-stale evidence, continues the same capability, refuses a replayed frame as a duplicate, refuses the same identity with different content, and exits zero on a protocol shutdown. |
| `concurrency` | Concurrent ingestion and queries under invariant checks (a healthy state always has fresh evidence, an unknown state never carries a value), and concurrent ingestion, policy publication and compaction followed by recovery of a consistent journal. |
| `shutdown` | Idempotent close, refusal of new work after close, durability of accepted work across a close that races with ingestion, prompt stop with idle connections attached, and the connection bound. |
| `wire_codec` | Round trips for every message, encoder and decoder symmetry, and adversarial decoders: truncation at every offset, trailing bytes, oversized counts, oversized strings, out-of-range enums, malformed booleans, bad variant indices and NaN preservation. |
| `transport` | The full protocol over real loopback TCP, protocol errors answered and survivable, oversized frames refused before allocation, the wakeup channel, and a refusal for an unconnected client. |
| `multiprocess` | The full protocol across two operating system processes, malformed framing survivable by the server process, and four concurrent client processes with distinct source incarnations producing exactly the expected accepted count. |
| `lock_audit` | The audit detects re-entrancy and order inversion when they are deliberately created, and the runtime workload then reports zero violations. |

## Results

* Release: 18 of 18 suites pass.
* Debug: 18 of 18 suites pass.
* AddressSanitizer (`-DLQF_SANITIZE=address`, RelWithDebInfo): 18 of 18 suites
  pass with no sanitizer report. MSVC AddressSanitizer does not provide leak
  detection on this platform, so no leak claim is made.
* Install and downstream: `cmake --install` produces the package, and an
  independent project outside the repository builds against it with
  `find_package(LinkQualityFabric 1.0 REQUIRED)` and runs correctly.
* The command line tool was exercised end to end over the wire: serve, emit,
  query, explain, inspect, metrics, policy, flush, journal-verify, journal-dump
  and shutdown, including the refusal paths for a wrong token, a missing journal
  and malformed arguments.
* Examples and benchmark build and run from the release tree.

## Defects found and fixed during validation

These were found by the suites in this repository and fixed before release:

1. **Snapshot replay discarded every recovered record.** The replay fence was
   written before the evidence it fences, so each recovered record looked like a
   duplicate of itself. Evidence is now written first and the fence last.
2. **Recovered evidence lost its origin.** The evidence store overwrote the
   origin with `live`, so a restart reported recovered records as live. The
   origin is now supplied by the caller and preserved.
3. **Every startup appended a duplicate policy record and restarted the record
   ordinal.** The journal was considered fresh before the replay had run, and
   the ordinal continued from zero in the middle of a file. Both are now derived
   after recovery.
4. **A 32-bit counter wrap produced a 64-bit modular difference.** The wrap delta
   is now computed in the declared width. The differential property test against
   the reference model catches this class of error.
5. **A torn tail and mid-file corruption were treated identically,** so a damaged
   record with intact records after it could be truncated away. Corruption is now
   distinguished from a torn tail and refused.
6. **Stopping the server stalled for two minutes.** A blocked receive on Windows
   is not reliably woken by a shutdown issued from another thread. Each
   connection now has its own wakeup channel and every read waits for
   readability first, so a stop returns immediately.
7. **A lock re-entrancy in the protocol error path.** The server statistics lock
   was taken recursively while answering a malformed frame, which aborted the
   process on the first malformed frame. Statistics updates now take and release
   the lock in one helper, and the lock is tracked so the audit reports this
   class of defect.
8. **The journal file was opened with sharing denied,** so a second handle for a
   size probe or a verification failed while the runtime held the file.
9. **Capability units were not checked against the catalog,** so a source could
   declare a metric in a unit that contradicts its identity.
10. **A capability declaration for one link leaked unsupported metrics into its
    view**, and a fresh counter reading with no derivable rate was reported stale
    rather than incomplete.

## Remaining limitations

* **Not implemented and not claimed:** switch, ASIC, RDMA, InfiniBand, NVLink or
  optical hardware control; vendor telemetry protocols or MIBs; multi-host
  fabrics; transceiver inventory; link up/down authority; remediation.
* Evidence is retained in bounded windows: per stream and globally. A historical
  query returns what is retained and reports truncation; it does not claim to
  return everything that ever arrived.
* An as-of reconstruction of a counter rate is computed from the retained
  records of that stream, so it is bounded by the same retention window.
* The transport is loopback TCP in one host. Two processes on one machine are
  proved; a multi-host deployment is not, and no authentication or encryption is
  provided beyond the shutdown token.
* Freshness depends on the monotonic clock of the running process. A wall clock
  step does not affect freshness, but it does affect the reported age of
  recovered evidence.
* AddressSanitizer leak detection is unavailable for this toolchain
  configuration.
