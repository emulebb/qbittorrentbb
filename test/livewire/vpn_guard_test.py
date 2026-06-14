#!/usr/bin/env python3
"""Repeatable live-wire test for the qBittorrentBB VPN egress guard.

Drives the real qBittorrentBB build against a live VPN tunnel and verifies the
allow-list guard end to end:

  * ALLOW scenario - the VPN's exit ranges are whitelisted, so the observed
    public egress IP is allowed and BitTorrent keeps running (no leak).
  * LEAK scenario  - the range that actually contains the VPN exit IP is
    dropped from the allow-list, so the observed IP is "outside the allowed
    set": the guard must detect a leak and pause all P2P (fail closed).

All machine-local paths and the (private) VPN exit CIDR ranges live in a
git-ignored JSON (default: vpn_guard.local.json next to this script), so the
ranges are never committed. Windows-only (uses PowerShell for process/IP ops).

Usage:  python vpn_guard_test.py [path-to-config.json]
"""

from __future__ import annotations

import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE / "vpn_guard.local.json"

OBSERVED_RE = re.compile(r"observed public egress address\s+([0-9a-fA-F:.]+)")
LEAK_RE = re.compile(r"leak detected", re.IGNORECASE)


def ps(command: str) -> str:
    """Run a PowerShell command and return stdout (stripped)."""
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command", command],
        capture_output=True, text=True, check=False,
    )
    return out.stdout.strip()


def stop_qb() -> None:
    ps("Get-Process qbittorrent -ErrorAction SilentlyContinue | Stop-Process -Force")
    time.sleep(2)


def tunnel_ip(alias: str) -> str:
    return ps(
        f"Get-NetIPAddress -InterfaceAlias '{alias}' -AddressFamily IPv4 "
        "-ErrorAction SilentlyContinue | "
        "Where-Object {{ $_.AddressState -eq 'Preferred' }} | "
        "Select-Object -First 1 -ExpandProperty IPAddress"
    ).splitlines()[0].strip() if alias else ""


def resolve_host(server: str) -> str:
    """Resolve "host[:port]" to "ip[:port]" (STUN wants an IP literal)."""
    host, sep, port = server.partition(":")
    try:
        socket.inet_aton(host)
        return server  # already an IP
    except OSError:
        ip = socket.gethostbyname(host)
        return f"{ip}:{port}" if sep else ip


def ini_path(profile: str) -> Path:
    return Path(profile) / "qBittorrent" / "config" / "qBittorrent.ini"


def set_session_keys(profile: str, keys: dict[str, str]) -> None:
    """Replace/insert Session\\<key>=<value> lines inside [BitTorrent]."""
    path = ini_path(profile)
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    wanted = {f"Session\\{k}": v for k, v in keys.items()}
    # drop existing occurrences
    lines = [ln for ln in lines if ln.split("=", 1)[0] not in wanted]
    # insert right after the [BitTorrent] section header
    out: list[str] = []
    inserted = False
    for ln in lines:
        out.append(ln)
        if ln.strip() == "[BitTorrent]" and not inserted:
            out.extend(f"{k}={v}" for k, v in wanted.items())
            inserted = True
    if not inserted:  # no section yet -> create it
        out.append("[BitTorrent]")
        out.extend(f"{k}={v}" for k, v in wanted.items())
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def run_scenario(cfg: dict, name: str, allowed: list[str]) -> tuple[bool, str]:
    """Launch qb with the given allow-list, return (leak_detected, observed_ip)."""
    profile = cfg["profile"]
    log = Path(profile) / "qBittorrent" / "data" / "logs" / "qbittorrent.log"

    stop_qb()
    ip = tunnel_ip(cfg.get("interface_alias", ""))
    keys = {
        "VPNGuardStunServer": resolve_host(cfg["stun_server"]),
        "VPNGuardHttpEcho": cfg.get("http_echo", ""),
        "VPNGuardAllowedCidrs": ",".join(allowed),
    }
    if ip:
        keys["Interface"] = ""
        keys["InterfaceAddress"] = ip
    set_session_keys(profile, keys)

    offset = log.stat().st_size if log.exists() else 0
    print(f"[{name}] tunnel={ip or '?'}  stun={keys['VPNGuardStunServer']}  "
          f"allowed={keys['VPNGuardAllowedCidrs'] or '(none)'}")

    subprocess.Popen([cfg["exe"], f"--profile={profile}"])
    time.sleep(18)

    new = ""
    if log.exists():
        with log.open("r", encoding="utf-8", errors="replace") as fh:
            fh.seek(offset)
            new = fh.read()
    stop_qb()

    observed = ""
    m = OBSERVED_RE.search(new)
    if m:
        observed = m.group(1)
    leak = bool(LEAK_RE.search(new))
    for ln in new.splitlines():
        if "VPN" in ln or "egress" in ln or "leak" in ln.lower():
            print("   " + ln.strip())
    return leak, observed


def main() -> int:
    cfg_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CONFIG
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    allowed = list(cfg["allowed_cidrs"])
    drop = cfg.get("_leak_test_drop_cidr")

    results = []

    # ALLOW: full allow-list -> observed IP should be inside it -> no leak
    leak, obs = run_scenario(cfg, "ALLOW", allowed)
    ok_allow = (obs != "") and (not leak)
    results.append(("ALLOW (full allow-list -> clean)", ok_allow,
                    f"observed={obs or 'none'} leak={leak}"))

    # LEAK: drop the range that contains the VPN exit IP -> observed outside -> leak
    reduced = [c for c in allowed if c != drop] if drop else allowed
    leak, obs = run_scenario(cfg, "LEAK", reduced)
    ok_leak = leak
    results.append((f"LEAK (drop {drop} -> fail-closed)", ok_leak,
                    f"observed={obs or 'none'} leak={leak}"))

    print("\n=== RESULTS ===")
    all_ok = True
    for label, ok, detail in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}  ({detail})")
        all_ok = all_ok and ok
    print(f"=== {'ALL PASSED' if all_ok else 'FAILURES'} ===")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
