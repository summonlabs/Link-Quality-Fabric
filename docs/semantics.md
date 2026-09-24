# Semantics

## Metric identity and capability

A metric identity is a family plus a canonical lower-case dotted name. A source
declares, per link generation or globally, which metrics it measures: units,
sample semantics, counter width, lane coverage and an optional validity window.
A metric nobody declares is `unsupported`; a metric a source explicitly declares
as not measured is reported as declared-unsupported. Declarations are revised:
a newer revision replaces an older one, an older revision is fenced, the same
revision with identical content is a duplicate and the same revision with
different content is an identity mismatch.

Declared units must match the catalog unit for that metric identity, so a source
cannot redefine what a metric is measured in. Evidence must match the declared
capability: units, semantics, lane coverage and counter width are all checked
before anything is stored.

## Bands and classification

A rule classifies one metric against ordered bands. Bands must be contiguous and
total over the extended reals: the first starts at negative infinity, each band
starts exactly where the previous one ends, and the last has no upper bound. A
document with a gap, an overlap or a NaN bound is refused, so every finite value
classifies exactly once. Band bounds are half-open: the lower bound is included,
the upper bound is excluded.

## Counter continuity

Continuity is tracked per link generation, metric, lane and source incarnation.

* `advance` when the reading is at or above the previous one: the difference is
  exact and non-negative.
* `wrap` only when the width is declared, wrap inference is enabled for the
  metric, and the previous reading is at or above the ceiling fraction of the
  modulus while the new reading is at or below the floor fraction. The delta is
  the exact modular difference for that width. The event and the reason code
  record that the delta was inferred.
* `decrease-unproven` for every other decrease. No delta is produced, continuity
  is broken, and the new reading becomes the baseline. A reset and a wrap cannot
  be distinguished from two readings alone, so neither is claimed.
* `reset` when the source declares one: re-baselined, no delta.
* `width-changed`, `source-reincarnated` and `link-generation-changed` break
  continuity. Streams are keyed by incarnation and generation, so reincarnation
  fencing is structural rather than a check.
* `out-of-range` refuses a reading that does not fit the declared width and
  clears the baseline.
* `sequence-regressed` refuses a reading older than the accepted watermark
  without disturbing the baseline.

A delta whose interval exceeds the configured continuity gap is reported as an
interval average with `spans_gap` set. It never supports a current rate: the
assessment is `incomplete`.

## Freshness

Freshness is computed at query time. Evidence is fresh when it was received by
this running process incarnation and its age on the monotonic clock is within
the validity window for that metric (the rule, then the declared capability,
then the policy default, in that order).

Recovered evidence is never fresh, whatever the wall clock says, because the
receive stamp carries the process incarnation that received it. A counter rate
is fresh only when the newest reading is fresh and the interval the rate covers
fits inside the validity window. An as-of query excludes evidence received after
the instant being evaluated and recomputes continuity from the retained records
with the same engine and configuration.

## Conflicts and authority

Candidates are grouped by authority rank. A strictly higher rank wins and the
ignored ranks are reported. Readings at the highest rank must agree within the
metric conflict tolerance: when they do not, the state is `conflicting`, every
claimant provenance is preserved and no representative value is produced. A zero
tolerance requires exact agreement; it never means "ignore".

## Completeness

`incomplete` covers evidence that cannot support a conclusion: a counter with a
baseline but no derivable step, broken continuity, an interval too short to
divide by, or a declared lane with no fresh evidence (the aggregate then reports
the missing lanes and cannot be better than incomplete).

## Confidence

Confidence is about agreement and freshness margin, never about physics. It is
`none` for `unknown` and `unsupported`, `low` for `stale`, `incomplete` and
`conflicting`, and for a classified state it is `high` when two or more distinct
fresh sources agree, `medium` when the freshest evidence is within half of its
validity window, and `low` otherwise.

## Roll-up

The link state is the worst state among its metrics using the published severity
ranks. A conflicting metric dominates when the document says so, which the
default does. A link with no capability at all is `unsupported`; a link nobody
has observed is `unknown` with the reason recorded.
