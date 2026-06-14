---
id: FEAT-003
workflow: local
title: Indexer schema + Torznab contract parity with emulebb-rust
status: OPEN
priority: Minor
category: feature
labels: [torznab, indexer, parity, prowlarr, suite]
milestone: phase-1
created: 2026-06-14
source: suite forward program (notes 14-15); SUITE-JOINT-ROADMAP
---

# FEAT-003 - Indexer schema + Torznab contract parity with emulebb-rust

## Summary

Keep the qBittorrentBB harvester index and Torznab endpoint structurally aligned
with the emulebb-rust Kad/eD2K indexer (FEAT-002/FEAT-004 there), so Prowlarr and
the Arr stack treat both indexers identically and the Python tooling can read both
uniformly.

## Why This Matters

The suite's federated search depends on both clients presenting the same indexer
contract. Parity is a **living goal** (co-evolved, not a frozen schema): the
identity column differs (BT `infohash` vs eD2K `ed2k_hash`) but the surrounding
columns and the Torznab dialect/caps/apikey scheme should match.

## Intended Shape

- Shared column conventions (name, size, seen timestamps, swarm/source count, FTS)
  identically named/typed across the two indexers.
- Shared Torznab response shape, caps, apikey scheme, and the "everything category
  8000/Other" decision (known limitation for Arr category routing — conscious).
- Ship one suite Prowlarr indexer definition (YAML) usable for both clients.

## Scope Constraints

- Do not freeze either schema; negotiate per-field as the rust indexer formalizes.
- No content categorization in this slice.

## Acceptance Criteria

- [ ] Harvester index columns match the rust indexer's shared convention set.
- [ ] Torznab responses from both clients validate against the same contract.
- [ ] A single suite Prowlarr indexer definition recognizes both clients.

## Validation

- Contract test comparing the two Torznab responses against one schema.
- Prowlarr smoke registering both via the shared definition.

## Notes

- Depends on the rust indexer surfaces (FEAT-002/FEAT-004 in emulebb-rust) landing
  the canonical shape first.
