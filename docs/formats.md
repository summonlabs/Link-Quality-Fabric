# Formats

## Journal

The journal is a sequence of length-prefixed, checksummed records in a single
file. It is versioned, bounded and recoverable.

    file header (40 bytes)
      [0,8)    magic "LQFJRNL1"
      [8,12)   u32 format version
      [12,16)  u32 flags (reserved, zero)
      [16,24)  u64 producer epoch
      [24,32)  i64 creation wall time (nanoseconds since the Unix epoch)
      [32,36)  u32 CRC-32C over bytes [0,32)
      [36,40)  u32 reserved (zero)

    record
      [0,4)                  u32 payload length (bytes after this field)
      [4,8)                  u32 CRC-32C over the payload
      payload                u64 ordinal, u8 kind, body

Record kinds: `capability`, `policy`, `link-observed`,
`link-generation-advanced`, `evidence`, `snapshot` and `dedup-window`.

The ordinal is assigned under the fabric state lock, so file order equals
ordinal order and the writer refuses a record that would break the sequence.
The `dedup-window` record carries the sequence-to-digest window of one stream so
that a replayed frame is refused after a restart exactly as it was refused
before it.

A `snapshot` body is itself a bundle of records, so recovery has exactly one
apply path and compaction cannot diverge from replay. Evidence is written before
the dedup window that fences it.

## Recovery rules

* A file that does not exist, or exists with zero length, is initialised with a
  fresh header.
* A header with the wrong magic, an unsupported format version or a bad checksum
  is refused: the runtime does not open.
* A record whose header is truncated at the end of the file, whose body is
  truncated, or whose checksum fails **with no intact record after it** is a
  torn tail. The runtime replays everything before it, reports the finding, and
  truncates the file to the last verified record before appending.
* A record whose checksum fails **with intact records after it** is corruption,
  not a torn tail: recovery stops, nothing is truncated, and the runtime refuses
  to open on a history it cannot verify.
* A record with an unrecognised kind, or a non-increasing ordinal, stops
  recovery conservatively.
* A leftover `.tmp` file from an interrupted compaction is never authoritative
  and is discarded; the finding is reported.
* Recovery stops at configured record and byte limits and reports that it did.

## Compaction

Compaction writes a snapshot bundle to a temporary file, syncs it, and replaces
the journal atomically. It runs while no producer can append, so the snapshot
ordinal is exactly the last assigned ordinal. Compaction is refused when the
snapshot body would exceed the record limit rather than writing a partial file.

## Wire protocol

Framing over loopback TCP:

    [0,4)   u32 payload length (1 + body size)
    [4,8)   u32 CRC-32C over the payload
    payload u8 message type, body

The payload length is validated against the frame limit before anything is read
into memory, and the checksum is verified before any field is interpreted.

Request messages: `Hello`, `RegisterCapability`, `PublishPolicy`, `Ingest`,
`Query`, `Window`, `Explain`, `Inspect`, `Capabilities`, `PolicyDocument`,
`Stats`, `Flush`, `Compact`, `Shutdown`.

Reply messages: `Welcome`, `CapabilityAck`, `PolicyAck`, `IngestAck`,
`QualityReport`, `WindowReply`, `ExplanationReply`, `InspectionReply`,
`CapabilitiesReply`, `PolicyDocumentReply`, `StatsReply`, `FlushAck`,
`CompactAck`, `ShutdownAck`, `Error`.

A refusal is an `Error` reply carrying a status code and a bounded message, so a
rejected operation is never a silent success. Malformed framing, an unknown
message type, an oversized length and a checksum mismatch are all answered with
an error and the connection is closed; the runtime keeps serving.

Shutdown requires a token that the operator arms at start; an unarmed runtime
refuses every shutdown request. Stopping the server closes the listener, signals
every connection channel, closes each connection socket from its own thread and
joins every thread.

## Limits

Decoder limits: maximum field string length, maximum item count per vector,
maximum encoded message size. Every vector count and string length is validated
against the remaining input before anything is allocated. Enum values outside
their declared range are refused. Boolean fields must be zero or one, optional
presence flags must be zero or one, and variant indices must exist.

Journal limits: maximum record size, queue depth, recovery record and byte
counts, compaction thresholds. Fabric limits: every bound listed in
`docs/architecture.md`.
