# Command Journal, Durability, and Recovery

## Purpose

The journal records the command stream that drives the single-instrument matching engine. On restart, replaying the same commands through the same matching code reconstructs book state, trade IDs, client-ID history, and command sequence.

## Record format

Each record is 48 bytes and is encoded field-by-field in little endian. The implementation does not dump a C++ struct, so compiler padding is not part of the file format.

```text
0   : uint32 magic
4   : uint16 format version
6   : uint8  record type (new order or cancel)
7   : reserved
8   : uint64 ordinal
16  : uint64 order ID
24  : side
25  : order type
26  : time in force
27  : flags
28  : int64 price bits
36  : uint32 quantity
40  : reserved
44  : uint32 CRC32 over bytes [0, 44)
```

Replay validates magic, version, record type, exact ordinal continuity, full-record length, and CRC32 before applying a command. A partial final record is treated as truncation, not silently ignored. Append mode scans and validates the existing file before writing the next ordinal.

## Write ordering

In `matching_engine_main`, the journal append occurs before command application:

```text
pop command -> append journal -> apply matching/risk -> publish trades/report
```

If append fails, the command is not applied and the engine exits fail-stop. This avoids continuing with state that cannot be reconstructed from the journal.

## Durability modes

### Buffered

The kernel receives each record through `write`, but the engine calls `fdatasync` only during orderly shutdown. This offers much lower hot-path overhead but can lose the most recent kernel-buffered records after power loss or an unrecoverable OS crash.

### Per-record `fdatasync`

`--sync-journal` requests `fdatasync` after every record. This strengthens crash durability but adds storage latency to every command. It is intentionally explicit rather than hidden behind a misleading “durable” label.

Neither mode alone provides replicated durability, atomic multi-node failover, or protection against storage-device failure.

## Recovery

```bash
matching_engine_main --journal /tmp/lob.journal --recover
```

Recovery validates and replays the complete journal before creating the shared-memory region and publishing `READY`. If validation fails, the process exits without exposing a partially reconstructed engine.

Offline inspection:

```bash
journal_replay_main --journal /tmp/lob.journal --depth 10
```

The inspector prints record counts, accepted/rejected commands, trades, reconstructed sequence, BBO, active order count, top depth, and invariant status.

## Configuration caveat

The journal stores commands, not deployment policy. Replay must use the same pre-trade risk limits that were active when the file was written. A production design would place immutable session/configuration metadata in a journal header or configuration event and would checkpoint state to reduce recovery time for very long logs.

## Next persistence steps

A production-shaped continuation would add:

- versioned journal/session metadata;
- periodic checksummed snapshots and replay from snapshot offset;
- segment rotation and retention policy;
- explicit corruption quarantine/repair tooling;
- replicated write-ahead logging and deterministic leader failover;
- audit exports with account/session identity and externally synchronized timestamps.
