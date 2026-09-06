#!/usr/bin/env python3
"""
Verify an INSTALLED plugin tree against the published SkunkCrafts control
files — i.e. answer "did the update actually land?" with a measurement
instead of a feeling.

The updater keeps two things in sync that can drift apart:

  * skunkcrafts_updater.cfg   -> the version its UI shows
  * the whitelisted files     -> what is actually on disk

A partial update writes the cfg but leaves a file behind (a mapped .xpl that
could not be replaced while X-Plane was running is the classic case). The UI
then reports the new version while the plugin still reports the old one, and
nothing in the updater flags it. This script compares every whitelisted file's
CRC32 + size against the published lists and prints exactly which files are
stale.

Usage:
    # against the live release branch
    python3 tools/skunkcrafts/verify_install.py \\
        --tree "<X-Plane>/Resources/plugins/xp_wellys_vfr_atc"

    # against a specific module URL or a local staged tree
    python3 tools/skunkcrafts/verify_install.py --tree <dir> --module <url>
    python3 tools/skunkcrafts/verify_install.py --tree <dir> --control <dir>

Exit code 0 = in sync, 1 = drift found, 2 = could not check.
"""
import argparse
import sys
import urllib.request
import zlib
from pathlib import Path

DEFAULT_MODULE = (
    "https://raw.githubusercontent.com/rwellinger/xp_wellys_vfr_atc"
    "/refs/heads/release/"
)

WHITELIST = "skunkcrafts_updater_whitelist.txt"
SIZESLIST = "skunkcrafts_updater_sizeslist.txt"
CFG = "skunkcrafts_updater.cfg"


def crc32(fp: Path) -> int:
    checksum = 0
    with fp.open("rb") as f:
        while chunk := f.read(65536):
            checksum = zlib.crc32(chunk, checksum)
    return checksum & 0xFFFFFFFF


def fetch(base: str, name: str) -> str:
    if base.startswith(("http://", "https://")):
        # Cache-bust: raw.githubusercontent serves branch refs with a short
        # max-age, and a cached whitelist would make a stale install look fine.
        url = base.rstrip("/") + "/" + name
        req = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.read().decode("utf-8")
    return (Path(base) / name).read_text()


def parse_pairs(text: str) -> dict[str, int]:
    out = {}
    for line in text.splitlines():
        if "|" in line:
            rel, _, val = line.rpartition("|")
            out[rel] = int(val)
    return out


def cfg_version(text: str) -> str:
    for line in text.splitlines():
        if line.startswith("version|"):
            return line.split("|", 1)[1].strip()
    return "?"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True, help="installed plugin root")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--module", help=f"update host URL (default: {DEFAULT_MODULE})")
    src.add_argument("--control", help="local directory holding the control files")
    args = ap.parse_args()

    tree = Path(args.tree).expanduser().resolve()
    if not tree.is_dir():
        print(f"not a directory: {tree}", file=sys.stderr)
        return 2

    base = args.control or args.module or DEFAULT_MODULE
    try:
        remote_wl = parse_pairs(fetch(base, WHITELIST))
        remote_sz = parse_pairs(fetch(base, SIZESLIST))
        remote_cfg = fetch(base, CFG)
    except Exception as e:  # noqa: BLE001 — any fetch failure is "cannot check"
        print(f"could not read control files from {base}: {e}", file=sys.stderr)
        return 2

    remote_ver = cfg_version(remote_cfg)
    local_cfg = tree / CFG
    local_ver = cfg_version(local_cfg.read_text()) if local_cfg.is_file() else None

    print(f"tree      : {tree}")
    print(f"source    : {base}")
    print(f"published : {remote_ver}")
    print(f"installed : {local_ver if local_ver else '(no skunkcrafts_updater.cfg)'}")
    print()

    missing, stale = [], []
    for rel, want_crc in sorted(remote_wl.items()):
        path = tree / rel
        if not path.is_file():
            missing.append(rel)
            continue
        got_crc = crc32(path)
        got_size = path.stat().st_size
        want_size = remote_sz.get(rel)
        if got_crc != want_crc or (want_size is not None and got_size != want_size):
            stale.append((rel, got_size, want_size))

    for rel in missing:
        print(f"MISSING  {rel}")
    for rel, got, want in stale:
        print(f"STALE    {rel}  (on disk {got} B, published {want} B)")

    total = len(remote_wl)
    ok = total - len(missing) - len(stale)
    print()
    print(f"{ok}/{total} files match.")

    if local_ver and local_ver != remote_ver:
        print(f"cfg version differs: installed {local_ver}, published {remote_ver}")
        print("-> the updater has not run (or not finished) against this tree.")
    elif local_ver == remote_ver and (missing or stale):
        print("cfg version matches but files do not.")
        print("-> PARTIAL UPDATE: the updater recorded the new version while")
        print("   leaving the files above behind. This is the failure mode that")
        print("   makes the updater UI and the plugin's own version disagree.")
        print("   Quit X-Plane before updating (a loaded .xpl cannot always be")
        print("   replaced in place), then re-run the update.")

    return 1 if (missing or stale) else 0


if __name__ == "__main__":
    raise SystemExit(main())
