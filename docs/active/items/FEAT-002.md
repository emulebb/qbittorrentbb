---
id: FEAT-002
workflow: github
github_issue: https://github.com/emulebb/qbittorrentbb/issues/2
title: Persist harvested torrents to a sharded local on-disk store
status: OPEN
priority: Major
category: feature
labels: [dht, harvester, storage, reconciliation, suite]
milestone: phase-1
created: 2026-06-14
source: suite forward program (note 3); BB-TORRENT-EXPORT-AND-HARVEST
---

> Workflow status is tracked in GitHub: https://github.com/emulebb/qbittorrentbb/issues/2. This local document is retained as an engineering spec/evidence record.

# FEAT-002 - Persist harvested torrents to a sharded local on-disk store

## Summary

When the DHT harvester obtains full metadata for an infohash, also serialize the
`torrent_info` to a `.torrent` in a sharded local store and record the path in the
harvester SQLite row, so the Python reconciliation fabric has real torrent files
to match disk content against. Design:
[`docs/BB-TORRENT-EXPORT-AND-HARVEST.md`](../BB-TORRENT-EXPORT-AND-HARVEST.md).

## Why This Matters

Turns the passive harvest index into actionable "you are sitting on seedable data"
intelligence (reconcile notes 2/4). We already hold the complete `torrent_info` at
metadata time, so persistence is a local serialize with no extra network.

## Intended Shape

- Path: `harvested/<aa>/<bb>/<infohash>.torrent`, where `aa`/`bb` are the first two
  hex byte-pairs of the infohash (git-object / magnetico fan-out). The path is a
  pure function of the infohash — derivable, no lookup.
- Persist all full-metadata torrents (a few KB each).
- Record the on-disk path in the existing harvester SQLite row (new column),
  keeping DB ↔ disk linked.

## Scope Constraints

- **Strictly local and separate from the live/shared library.** Harvested torrents
  are an index + reconciliation input only — never shared, branded, or exported.
- No content categorization (unchanged from current harvester behavior).

## Acceptance Criteria

- [ ] Each full-metadata harvested infohash writes a `.torrent` at its sharded path.
- [ ] The harvester SQLite row records the on-disk path.
- [ ] No harvested torrent is ever surfaced to the eD2K share or export library.
- [ ] Re-harvesting an existing infohash does not duplicate or churn the file.

## Validation

- Unit: sharded-path derivation; idempotent write; DB path column populated.
- Local: harvest a fixture; confirm files land at derived paths and are readable by
  the reconciliation tooling.

## Notes

- Feeds the `orphan→harvest-matched` cross-check in the Python fabric (note 4).
