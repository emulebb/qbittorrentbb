---
id: QBBB-FEAT-005
workflow: github
github_issue: https://github.com/emulebb/qbittorrentbb/issues/5
title: Headless nox Linux build + GHCR Docker image
status: OPEN
priority: Major
category: feature
labels: [docker, ghcr, nox, packaging, bundle]
milestone: phase-2
created: 2026-06-16
source: SUITE-DOCKER design (2026-06-16)
---

> Workflow status is tracked in GitHub. This local document is retained as an engineering spec/evidence record.

# QBBB-FEAT-005 - Headless nox Linux build + GHCR Docker image

## Summary

Add a **headless `nox` Linux build** of the qBittorrentBB fork and publish it as a
**linuxserver-style** Docker image to **GHCR**
(`ghcr.io/emulebb/qbittorrentbb-nox`, `latest` + versioned), built and pushed by
this repo's CI. Design:
[`emulebb-tooling/docs/active/SUITE-DOCKER.md`](../../../emulebb-tooling/docs/active/SUITE-DOCKER.md).

## Why This Matters

The **enabling prerequisite** for the BitTorrent client in the suite Docker
bundle, which runs headless in a container. Without the image the Docker form
cannot start.

## Intended Shape

- Build `qbittorrent-nox` (no GUI) for Linux from the fork, preserving the DHT
  harvester + local SQLite index, Torznab endpoint, and BB export.
- **linuxserver convention:** s6-overlay, `PUID`/`PGID`, `TZ`, `/config` + `/data`.
- Downloads under `/data/torrents` for hardlink + atomic-move to `/data/media`.
- Runs behind an **optional Gluetun** namespace; WebUI + BT listen ports published
  on the fronting gluetun service.
- Keep the fail-closed VPN binding (QBBB-FEAT-004) coherent inside the
  container/Gluetun model.

## Acceptance Criteria

- [ ] CI produces a `qbittorrent-nox` Linux binary from the fork.
- [ ] CI builds and pushes `ghcr.io/emulebb/qbittorrentbb-nox:latest` + a version tag on release.
- [ ] WebUI + BT reachable when ports are published on a fronting service; `/config` + `/data` persisted.

## Notes

- One of the four prerequisite images (with `emulebb-rust`, `trackmulebb`,
  `bountarr`). Gluetun is the Docker analog of the Windows hide.me split-tunnel.
