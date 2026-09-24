# Architecture

## Layers

    include/lqf/core        typed identities, status values, checked arithmetic, hashes,
                            clocks, text, the lock audit
    include/lqf/domain      units, metrics, identities, observations, capability, policy,
                            quality states, counter continuity, classification and rendering
    include/lqf/store       capability registry, link registry, bounded evidence store
    include/lqf/classify    the classifier and the explanation renderer
    include/lqf/persist     field archive, codecs, journal reader and background writer
    include/lqf/runtime     configuration, the Fabric facade
    include/lqf/transport   frames, sockets, the framed server and client
    apps/lqf                the command line runtime
    examples, benchmarks    consumers of the public API only
    tests                   one executable, one CTest entry per suite

Dependencies point inward. The domain layer never calls the runtime or the
transport, and the transport depends only on the runtime facade.

## Ownership

The Fabric owns all mutable state: the metric catalog, the capability registry,
the link registry, the evidence store, the policy registry, the journal and the
statistics. Nothing outside the runtime holds a reference into that state; every
public operation returns a value.

The runtime does not own binary link authority, route eligibility, remediation,
transceiver inventory or optical switching, and it exposes no operation that
could be mistaken for one.

## Concurrency contract

One audited reader/writer lock (the fabric state lock) guards every piece of
mutable runtime state.

* Ingestion, capability declaration, policy publication, link registration and
  compaction take it exclusively.
* Queries take it shared and compute the whole report inside the lock, then
  return by value. No callback, join or foreign lock is ever taken while it is
  held.
* Statistics that are updated on the read path are atomics, because readers hold
  the lock shared and must not write shared state.

### Lock order

    fabric state  ->  journal compaction  ->  journal queue

Ingestion takes the journal queue while it already holds the state lock, and
compaction takes the state lock first. The queue is never taken before the state
lock, and the journal writer thread takes only the queue lock. The lock audit
records every observed order edge and reports any acquisition that would close a
cycle.

### Journal writer

The background journal owns one writer thread, one bounded queue and the file
handle. Producers enqueue an already-encoded record and never block: a full
queue refuses the record with a status that the caller sees before it mutates
state. The writer drains the queue, writes, and flushes on request. Shutdown
stops acceptance, wakes the writer, drains, syncs and joins.

### Transport

The server owns one accept thread and one thread per connection, bounded by
configuration. Every blocking wait (accept, frame read) waits on a wakeup
channel first, so a stop signal is a real signal rather than a deadline: the
accept loop wakes, the connection threads wake, each closes its own socket, and
every thread is joined on the stop path. A stop issued from a connection handler
is refused instead of self-joining.

## Lifecycle

    Fabric::open
      validate configuration
      build the metric catalog
      validate the initial policy document
      recover the journal: read records, apply them, refuse unverifiable data,
        repair only a torn tail, continue the record ordinal
      open the journal for append and persist the initial generation if the
        journal was empty
    operating
      declare, ingest, query, window, explain, inspect, publish, flush, compact
    Fabric::close
      stop accepting work, drain and sync the journal, join the writer

Recovery never re-decides: a record that was accepted while it was journaled is
applied as state, with its original receive stamp and its original observation
time, and marked recovered. Recovered evidence is never fresh.

## Bounded resources

Every growth point has a declared limit in `lqf::FabricLimits` or
`lqf::JournalConfig`, validated at startup: links, generations per link, sources,
streams per link, retained records per stream, global retained records,
deduplication window, batch size, metrics per query, window records, inspection
entries, frames, decoder strings and items, journal queue depth, record size and
recovery scan size. Exceeding a limit is a typed refusal, never a silent
truncation.

## Failure policy

Type failures are values (`lqf::Status`), not exceptions. `UNKNOWN`,
`UNSUPPORTED`, `STALE`, `CONFLICTING`, `INCOMPLETE`, `REFUSED` and `INVALID` are
distinct codes and are never collapsed into success. A refused operation reports
why; a degraded persistence path is visible in the statistics and in the
recovery report.
