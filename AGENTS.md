# Rules

- Read `EMULEBB_WORKSPACE_ROOT\repos\emulebb-tooling\docs\WORKSPACE-POLICY.md`
  first; it is authoritative for workspace-wide rules.
- Start from
  `EMULEBB_WORKSPACE_ROOT\repos\emulebb-tooling\docs\reference\AGENT-CHECKLIST.md`
  for the repeatable operating path.

Everything below is this repo's local deltas only:

- This repo is the qBittorrentBB fork (Qt6 + a libtorrent fork). It is a managed
  BitTorrent client, separate from the eD2K/Kad eMuleBB line.
- BUILD OUTPUT: configure CMake out-of-source with the build/binary directory
  under `%EMULEBB_WORKSPACE_OUTPUT_ROOT%` (vcpkg + Qt6 via aqt). Never configure
  or build into this source tree or anywhere under `c:\prj`.
- ENV: read machine-level `EMULEBB_*` variables; never assign
  `EMULEBB_WORKSPACE_ROOT` or `EMULEBB_WORKSPACE_OUTPUT_ROOT`. Non-`EMULEBB_`
  knobs (vcpkg/Qt toolchain paths) may be set at a shell/CI boundary.
- LIVE P2P: bind the swarm through the hide.me VPN interface; before any live
  run, allow-list the client exe in the hide.me split-tunnel settings and restart
  the VPN as needed.
- LAN / control / probe traffic binds through `X_LOCAL_IP` / `--lan-bind-addr`.
  Never use `127.0.0.1` or `localhost` for harness control on the operator
  split-tunnel machine.

## Fork delta

- The exact delta over upstream qBittorrent `release-5.2.1` (fork-owned files,
  shared seams, features, rebase/acceptance) is in `fork-delta.json` — keep it
  current. The BT engine is the `emulebb-libtorrent` fork (`RC_2_0`); build/install
  it to `<out>\deps\libtorrent` (pass `--prefix`; the default targets
  `C:\Program Files`) and copy `torrent-rasterbar.dll` next to `qbittorrent.exe`.
- Stay additive: prefer new files (harvester/index/dhtindex/torznab) over edits to
  shared qBittorrent files, so upstream rebases stay conflict-driven.

## VPN binding (the egress that must not leak)

- `Session\Interface=` is left **EMPTY**; bind via `Session\InterfaceAddress=<live
  hide.me tunnel IPv4>` (re-read and rewrite it before every launch — the tunnel
  IP rotates). Putting the friendly name in `Session\Interface` is invalid on
  Windows and binds NO socket. The exe path must be in the hide.me split-tunnel
  whitelist; when the exe path changes, restart the VPN so the rule applies.
- The `emulebb-libtorrent` egress hardening + VPN guard back this up. Configure
  the guard with `Session\VPNGuardStunServer` / `VPNGuardAllowedCidrs`
  (comma-separated allow-list of VPN exit CIDRs) / `VPNGuardHttpEcho`. Verify with
  `test/livewire/vpn_guard_test.py` (reads a git-ignored `test/livewire/*.json`).

## Conventions

- ABI: settings/alert enums consumed from libtorrent are append-only upstream;
  don't reorder. No AI/Claude/Anthropic attribution. Tracked text is LF.
- Upstream sync: `.github/workflows/nightly-upstream.yml` watches for a newer
  `release-5.2.*` tag and pushes a clean rebase to `automation/upstream-nightly`
  (no CI build; master is never touched automatically).
