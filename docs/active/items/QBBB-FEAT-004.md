---
id: QBBB-FEAT-004
workflow: github
github_issue: https://github.com/emulebb/qbittorrentbb/issues/4
title: Make vpnReady() truly fail-closed (verify bound IP equals the tunnel) — release-blocking
status: OPEN
priority: Critical
category: feature
labels: [vpn, anonymity, safety, dht, release-blocker]
milestone: phase-1
created: 2026-06-14
source: PM quality review (2026-06-14); WORKSPACE-POLICY Network Safety invariant
---

> Workflow status is tracked in GitHub. This local document is retained as an engineering spec/evidence record.

# QBBB-FEAT-004 - Make vpnReady() truly fail-closed (verify bound IP equals the tunnel) — release-blocking

## Summary

Close the known gap where `vpnReady()` only checks the configured interface name
is up, not that libtorrent's **actual bound IP equals the tunnel**. Make the guard
truly fail-closed so BitTorrent peer/DHT egress is impossible unless the live bind
matches the tunnel. This is **release-blocking** per the P0 Network Safety
invariant.

## Why This Matters

The current guard plus the correct INI bind + split-tunnel whitelist is the
practical mitigation, but the guard itself is not fail-closed: a bind drifting off
the tunnel would not be caught. The suite's anonymity claim requires the guard to
be authoritative, not advisory.

## Current State

- `vpnReady()` checks the interface name is up; it does not assert
  `bound_ip == tunnel_ip`. Mitigation today is the verified `Session\Interface=`
  empty + `InterfaceAddress=<tunnel IP>` recipe + the split-tunnel whitelist.

## Intended Shape

- Verify libtorrent's live bound/listen IP equals the tunnel address; if it does
  not, treat the session as not-ready and refuse to start/keep P2P egress
  (fail-closed), surfaced in status.

## Acceptance Criteria

- [ ] Guard returns not-ready when the bound IP is not the tunnel IP, even if the
      interface name is up.
- [ ] No BitTorrent peer/DHT egress occurs while not-ready.
- [ ] Covered by an automated leak-test (tunnel down / wrong bind → zero egress).

## Notes

- Cross-product siblings: RUST-FEAT-003 (eD2K TCP pin), RUST-FEAT-005 (leak-test).
  All are release-blockers per WORKSPACE-POLICY Network Safety.
