# qBittorrentBB Active Backlog — Issue Index

This directory is the active local backlog/spec layer for the **qBittorrentBB**
fork. It follows the eMuleBB backlog convention
([`BACKLOG-PROCESS`](../../../emulebb-tooling/docs/reference/BACKLOG-PROCESS.md),
[`BACKLOG-ITEM-TEMPLATE`](../../../emulebb-tooling/docs/reference/BACKLOG-ITEM-TEMPLATE.md)):
each item is `docs/active/items/<ID>.md` with the same front matter and section
vocabulary.

Active items are **GitHub-tracked** (`workflow: github`): issues live in
`emulebb/qbittorrentbb` and are aggregated on the org **eMuleBB Suite** board
(`https://github.com/orgs/emulebb/projects/3`, `Product = qBittorrentBB`,
`Phase` field). GitHub owns workflow state; these Markdown files own the durable
engineering spec. Parked ideas stay out of the tracker (see the roadmap's Active
vs Parked ledger).

## Current Snapshot

**Source of truth:** `EMULEBB_WORKSPACE_ROOT\repos\qbittorrentbb` (`master` branch)
**Phase:** Phase 1 of `emulebb-tooling/docs/active/SUITE-JOINT-ROADMAP.md`
(after emulebb-rust reaches the Phase 0 gate).
**Already implemented (context, not backlog):** the DHT harvester + local SQLite
index, DHT Index GUI tab/RSS, and the Torznab endpoint are built and running
(magnetico/btdigg-style passive + BEP-51 + ephemeral BEP-9 metadata). The backlog
below captures the **forward suite slices** on top of that base.
**Design:** [`docs/BB-TORRENT-EXPORT-AND-HARVEST.md`](../BB-TORRENT-EXPORT-AND-HARVEST.md).
**Parked (NOT backlog):** cooperative-DHT mechanisms, BEP-46 library publishing,
and a libtorrent fork for deep cooperation — see the roadmap's Active vs Parked
ledger and `emulebb-tooling/docs/ideas/IDEA-COOPERATIVE-DHT-COOPERATION.md`.

## ID Taxonomy

Same classes as eMuleBB: `BUG`, `FEAT`, `REF`, `CI`. IDs are allocated per class
and never reused. Scan `docs/active/items` before allocating the next number.

## Features (`FEAT`)

| ID | Priority | Status | Title |
|----|----------|--------|-------|
| [FEAT-001](items/FEAT-001.md) | Major | OPEN | Branded idempotent export of the live torrent library to the eD2K share |
| [FEAT-002](items/FEAT-002.md) | Major | OPEN | Persist harvested torrents to a sharded local on-disk store |
| [FEAT-003](items/FEAT-003.md) | Minor | OPEN | Indexer schema + Torznab contract parity with emulebb-rust |

## Bugs (`BUG`)

| ID | Priority | Status | Title |
|----|----------|--------|-------|
| _none yet_ | | | |

## Refactors (`REF`)

| ID | Priority | Status | Title |
|----|----------|--------|-------|
| _none yet_ | | | |

## CI / Tooling (`CI`)

| ID | Priority | Status | Title |
|----|----------|--------|-------|
| _none yet_ | | | |
