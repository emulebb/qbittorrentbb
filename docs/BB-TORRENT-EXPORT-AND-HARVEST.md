# qBittorrentBB — Branded export + harvested disk store (notes 1, 3)

Status: design / direction. Captured 2026-06-14. Post-0.7.3; full development
mode. This is the qBittorrentBB-side slice of the suite metadata fabric. Suite
context: `emulebb-tooling/docs/active/SUITE-JOINT-ROADMAP.md` and
`SUITE-METADATA-FABRIC.md`. DHT harvester baseline: this fork's existing DHT
harvester (`src/base/bittorrent/harveststore.*`, `dhtharvester.*`, Torznab
endpoint).

## Two strictly separate libraries

qBittorrentBB owns both, and they must never mix:

| Library | Contents | Shared? | Branded? | Storage |
|---|---|---|---|---|
| **Live / shared** | only your own seeded torrents | yes (→ eMuleBB) | yes | export dir (user data) |
| **Harvested** | the DHT firehose with full metadata | **never** | **never** | sharded local store |

The live library is what you stand behind and publish. The harvested library is
an untrusted local index used only for reconciliation and discovery.

## Note 1 — Branded idempotent export (live library → eMuleBB)

Export every **non-private** live torrent to a canonical `.torrent` in a user-data
export directory (sibling to shared files, never build output), stamped with a
parseable branded comment, and surfaced through the eMuleBB shared path so the
underlying files are hashed and offered on eD2K/Kad.

- **Public = `info.private != 1`.** The private flag is the filter; private
  torrents are never exported. (This also self-excludes private content from any
  leak.)
- **Format preserved, never converted.** v1 → v1, v2/hybrid → as-is. The exporter
  materializes; it does not transcode. (`comment`/`created by` live outside the
  `info` dict, so stamping the brand does not change the infohash or split swarms.)
- **Idempotent.** Skip-by-identity (target exists + infohash matches → no-op) and
  deterministic bencode (sorted keys, fixed `created by`, pinned/omitted
  `creation date`, stable `comment`). Only a real metadata change rewrites a file.
- **Machine-parseable comment** is the suite-wide join key:
  `<configured brand> — <configured website>  [bb:v=1;k=<infohash>;src=qbbb]`.
  Brand + website are operator config, never hardcoded.

Implementation note: build via libtorrent `create_torrent` so serialization stays
deterministic; confirm comment/`created by` placement leaves the infohash
unchanged.

## Note 3 — Persist harvested torrents to a sharded disk store

When the harvester obtains full metadata for an infohash (it already does an
ephemeral magnet add → `metadata_received_alert` → `torrent_file()` → store →
remove handle), also serialize `torrent_file()` → bencode → write a `.torrent`
into a sharded local store and record the path in the harvester SQLite row.

```
harvested/<aa>/<bb>/<infohash>.torrent
```

`aa`/`bb` = first two hex byte-pairs of the infohash (git-object / magnetico
fan-out; keeps any directory small at millions of torrents). The path is a pure
function of the infohash — tooling derives it arithmetically, no lookup.

- **Persist all** full-metadata torrents (a few KB each).
- We already hold the complete `torrent_info` at `torrent_file()` time, so this is
  a local serialize — **no extra network**.
- **Never shared, never branded, never exported.** Quarantined to the machine as
  an index + reconciliation input for the Python metadata fabric (notes 2/4).
- Add a column to the harvester SQLite (which already mirrors
  `emulebb-metadata/schema.sql` conventions) recording the on-disk path, keeping
  DB ↔ disk linked.

## Downstream consumers

- The Python metadata fabric (`emulebb-tooling`) reconciles the **export (live)**
  library against disk (note 2), scans for orphan/mixed-content (note 4), and uses
  the **harvested** store for the `orphan→harvest-matched` cross-check.
- Collections and file→torrent membership (notes 5/6) are produced from the
  **live** library only.

## Cooperative DHT + library publishing

The cooperative-client mechanisms (rendezvous swarm, metadata-cache relay,
`bb_search`, node-ID-locality crawling, v2 file-root cross-seed, fork-enabled DHT
mining) and BEP-46 library publishing are tracked separately in
`emulebb-tooling/docs/ideas/IDEA-COOPERATIVE-DHT-COOPERATION.md`. A libtorrent
fork is on the table for the deeper plays, preferring protocol-compliant
extensions that are harmless to non-cooperating peers.

## Policy

- GPL-2.0 fork under the emulebb org; keep `LICENSE` verbatim GPLv2.
- Artifacts unsigned; no code signing.
- Tracked text is LF; run the source normalizer on new files.
- No private data, no real media titles — synthetic placeholders only.
- Egress bound fail-closed to the VPN tunnel; control plane on the local IP.
