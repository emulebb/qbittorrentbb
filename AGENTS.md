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
