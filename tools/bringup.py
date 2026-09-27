#!/usr/bin/env python3
# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

"""
Walk the board through the checks that need a human and a console.

Everything here is something only hardware can answer, written down so a session takes
minutes instead of an afternoon of improvised serial captures. It resets the board,
watches the boot, then prompts for each button and reports what it saw.

The one number this exists to produce is at the end: how long a 36 KiB coredump takes to
crawl over a 23-byte ATT MTU. That decides whether the demo keeps a full-RAM dump or
falls back to CONFIG_DEBUG_COREDUMP_MEMORY_DUMP_MIN.

    python3 tools/bringup.py [--port /dev/cu.usbmodemXXXX]
"""

import argparse
import re
import subprocess
import sys
import time

import serial

XDS110_RESET = (
    "/Users/jkc/ti/uniflash_9.6.0/deskdb/content/TICloudAgent/osx/ccs_base/"
    "common/uscif/xds110/xds110reset"
)
PROBE_SERIAL = "LS470H81"
DEFAULT_PORT = "/dev/cu.usbmodemLS470H811"

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def reset_board():
    r = subprocess.run([XDS110_RESET, "-s", PROBE_SERIAL], capture_output=True)
    return r.returncode == 0


def collect(port, seconds, echo=True):
    """Read the console for a while, returning (lines, first-seen timestamps)."""
    lines, seen = [], {}
    buf = b""
    start = time.time()
    with serial.Serial(port, 115200, timeout=0.2) as s:
        while time.time() - start < seconds:
            buf += s.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = ANSI.sub("", raw.decode("utf-8", "replace")).strip()
                if not text:
                    continue
                lines.append(text)
                seen.setdefault(text, time.time() - start)
                if echo:
                    print("   ", text)
    return lines, seen


def check(label, ok, detail=""):
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}{(' - ' + detail) if detail else ''}")
    return ok


def phase_boot(port):
    print("\n== boot ==")
    if not reset_board():
        print("  ! reset failed - is UniFlash holding the probe? close it and retry")
    lines, _ = collect(port, 20)
    joined = "\n".join(lines)

    results = [
        check("boot banner survives the startup burst", "asset tracker up" in joined),
        check("no metric registration failures", "not registered" not in joined),
        check("sensor initialised", "sensor ready (bmi270" in joined),
        check("duty cycle running", "duty cycle every" in joined),
        check("no heap exhaustion", "-12" not in joined,
              "still allocating" if "-12" not in joined else "ENOMEM is back"),
    ]
    temps = [int(m) for m in re.findall(r"temp=(-?\d+)", joined)]
    results.append(check("temperature valid on every sample",
                         bool(temps) and all(t != 0 for t in temps),
                         f"{sum(1 for t in temps if t)}/{len(temps)} samples"))
    return all(results)


def phase_degrade(port):
    print("\n== button 1: sensor degradation ==")
    input("  press button 1, then Enter...")
    lines, _ = collect(port, 45)
    joined = "\n".join(lines)
    kinds = set(re.findall(r"sensor read failed \((\w+)\)", joined))
    check("real bus errors appear", bool(kinds), f"kinds seen: {sorted(kinds) or 'none'}")
    check("error streak builds", "streak" in joined)
    check("stuck sensor detected (part powered down)",
          "has not changed" in joined, "needs ~1 min of degradation")
    input("  press button 1 again to recover, then Enter...")
    lines, _ = collect(port, 15)
    check("recovery restarts the part", "recovered" in "\n".join(lines))


def phase_drop(port):
    """Drop detection on real hardware: free fall, impact, and the crash that follows."""
    print("\n== drop detection ==")
    print("  Put the board in its enclosure and drop it onto the floor from waist")
    print("  height. Button 2 does the same thing without the throw, if you prefer.")
    print("  NOTE: nothing in this phase has been observed on hardware yet.")
    input("  drop it (or press button 2), then Enter...")
    lines, seen = collect(port, 150)
    joined = "\n".join(lines)

    check("drop detected", "drop detected" in joined or "drop:" in joined)
    m = re.search(r"fell (\d+) ms \(~(\d+) cm\), impact (\d+) mg", joined)
    if m:
        print(f"       -> fall {m.group(1)} ms, about {m.group(2)} cm, impact {m.group(3)} mg")
    elif "free fall" in joined:
        print("       -> free fall seen but no drop completed; check IMPACT_MIN_MG"
              " and MIN_FALL_MS in drop_detect.c against what the console shows")
    crashed = "asset tracker up" in joined
    check("device faulted and rebooted", crashed,
          "if this fails, the unaligned load did not fault - report the console verbatim")
    if crashed:
        m = re.search(r"asset tracker up: boot (\d+), reset (\w+)", joined)
        if m:
            cause = m.group(2)
            print(f"       -> boot {m.group(1)}, reset cause '{cause}'")
            # The SDK reboots deliberately after writing the dump, so a software reset is
            # the healthy answer. A watchdog reset means the dump write outran the 8 s
            # timeout and whatever uploaded will be truncated.
            if cause == "watchdog":
                print("       !! watchdog reset - the dump write outran the watchdog;"
                      " raise CONFIG_APP_WATCHDOG_TIMEOUT_MS")

    enq = next((t for l, t in seen.items() if "chunks enqueued" in l), None)
    sent = next((t for l, t in seen.items() if "successfully sent" in l), None)
    if sent is not None:
        base = enq if enq is not None else 0.0
        print(f"\n  *** coredump upload took {sent - base:.1f} s ***")
        print("      under ~30 s: keep the full 36 KiB dump")
        print("      over that:   switch to CONFIG_DEBUG_COREDUMP_MEMORY_DUMP_MIN=y")
    else:
        check("coredump uploaded", False,
              "no 'Coredump successfully sent.' - is a gateway connected?")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--skip", default="", help="comma-separated: boot,degrade,crash")
    # 'crash' still names the drop phase; the drop is what causes the crash.
    args = ap.parse_args()
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}

    print(f"console: {args.port}")
    print("a gateway must be connected for the coredump phase to complete")

    try:
        if "boot" not in skip:
            phase_boot(args.port)
        if "degrade" not in skip:
            phase_degrade(args.port)
        if "crash" not in skip:
            phase_drop(args.port)
    except serial.SerialException as e:
        sys.exit(f"serial error: {e}\nis another capture holding the port?")
    except KeyboardInterrupt:
        print("\ninterrupted")

    print("\ndone - paste the output into the session notes")


if __name__ == "__main__":
    main()
