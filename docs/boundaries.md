# Boundaries

Link Quality Fabric is one runtime in a set. This document states what it owns
and where the neighbouring responsibilities begin, so that no operation here can
be mistaken for one of them.

## Owned here

* Metric capability declaration: which source measures which metric, in which
  units, with which sample semantics, over which lanes.
* Evidence ingestion with validation against that declaration.
* Counter continuity: wrap, reset, gap, width, sequence and incarnation
  semantics, with no fabricated differences.
* Derived quality state with the evidence and the rule generation that produced
  it.
* Threshold policy generations, published with a content hash.
* Conflict, freshness and completeness inspection.
* Versioned, integrity-checked persistence and recovery.

## Not owned here

| Neighbour | Why it is separate |
| --- | --- |
| Binary link authority (up/down) | A quality conclusion is not an administrative state, and shaping quality evidence into an up/down verdict would hide exactly the states this runtime exists to report. |
| Route eligibility and path planning | Those consume quality state; they do not produce it, and they own policy this runtime has no visibility into. |
| Remediation and port programming | Acting on a conclusion is a different authority with different failure modes. |
| Transceiver inventory and cable attachment records | Inventory is identity and physical facts, not measured quality. |
| Optical switching and wavelength assignment | That is hardware control. |
| Vendor telemetry protocols and MIBs | Not implemented. Vendor-specific values are never synthesized; a transport is described by provenance, not by a vendor identity. |

## Interface to neighbours

A neighbouring runtime consumes:

* `Fabric::query` or the `Query` message for the current conclusion,
* `Fabric::window` for history and as-of reconstruction,
* `Fabric::explain` for the derivation, including the evidence and the rule
  generation,
* `Fabric::inspect` for conflicts, stale evidence and freshness counts,
* `Fabric::stats` and `Fabric::recovery_report` for runtime health.

A neighbouring runtime supplies evidence through `Fabric::ingest` or the
`Ingest` message, and declares what it measures through
`Fabric::declare_capability`. A transport-provided state hint is ingested as a
metric of family `link-state-hint` and is classified by the published policy
like any other measurement: it is evidence, never authority.
