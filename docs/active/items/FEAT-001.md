---
id: FEAT-001
workflow: local
title: Branded idempotent export of the live torrent library to the eD2K share
status: OPEN
priority: Major
category: feature
labels: [export, torrents, branding, suite, ed2k-bridge]
milestone: phase-1
created: 2026-06-14
source: suite forward program (note 1); BB-TORRENT-EXPORT-AND-HARVEST
---

# FEAT-001 - Branded idempotent export of the live torrent library to the eD2K share

## Summary

Export every non-private live torrent to a canonical `.torrent` in a user-data
export library, stamped with a parseable branded comment, and surface it through
the active eD2K core's share so the underlying files are also offered on eD2K/Kad.
Design: [`docs/BB-TORRENT-EXPORT-AND-HARVEST.md`](../BB-TORRENT-EXPORT-AND-HARVEST.md).

## Why This Matters

This is the bridge that makes a file reachable on both networks from one operator
action, and the `bb:` tag it stamps is the suite-wide join key for reconciliation,
collections, and file→torrent membership (notes 2/5/6).

## Intended Shape

- Filter: `info.private != 1` (private torrents never exported).
- Format preserved, never converted (v1→v1, v2/hybrid as-is); exporter is a
  materializer. `comment`/`created by` are outside `info`, so the brand stamp does
  not change the infohash or split swarms.
- Idempotent: skip-by-identity (target exists + infohash matches) + deterministic
  bencode (sorted keys, fixed `created by`, pinned/omitted `creation date`).
- Branded comment: `<configured brand> — <configured website>  [bb:v=1;k=<infohash>;src=qbbb]`.
  Brand + website are operator config, never hardcoded.
- Export library lives in user data (sibling to shared files), never build output.
- The export feeds the active eD2K core's share — primarily emulebb-rust
  `emulebb-metadata`; MFC `known.met` as compat.

## Scope Constraints

- Live (own) torrents only; harvested torrents are never exported (FEAT-002).
- GPL-2.0 fork hygiene; LF; no private data / real media titles.

## Acceptance Criteria

- [ ] Non-private live torrents export to canonical `.torrent` files; private ones
      are skipped.
- [ ] Re-running produces no churn (idempotent by infohash + deterministic bencode).
- [ ] The branded `bb:` comment is present and parseable; the infohash is unchanged
      by stamping.
- [ ] Exported files are surfaced through the active eD2K share.

## Validation

- Unit: deterministic serialization; private-flag filter; idempotent re-run
  produces byte-identical output; infohash unchanged after stamping.
- Local: export a fixture library; confirm eD2K share pickup.

## Notes

- Upstream consumers: the Python metadata fabric (reconcile/collections/membership)
  in `emulebb-tooling`.
