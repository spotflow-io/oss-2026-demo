#!/usr/bin/env bash
# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

#
# Flash the asset tracker onto an LP-EM-CC2340R5.
#
# The board has no onboard debugger, so this goes through a TI debug probe and DSLite,
# which ships with UniFlash. `west flash` is not an option here: the board's runner wants
# TI's OpenOCD build, homebrew OpenOCD has no cc23x0 flash driver, and pyOCD has no CMSIS
# pack for this part.
#
# Two things about the .ccxml next to this script, both of which cost real time to learn:
#
#   - SWD Mode Settings must be 2. The CC2340R5 is SWD-only and a config left at the
#     JTAG default fails to connect with "Error -1170".
#   - It was exported from the UniFlash GUI. Hand-writing one is not worth the afternoon.
#
# Usage:
#   tools/flash.sh [build-dir]            # default: build/tracker
#   DSLITE=/path/to/dslite.sh tools/flash.sh

set -euo pipefail

BUILD_DIR="${1:-build/tracker}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$HERE/../.." && pwd)"

cd "$WORKSPACE"

HEX="$BUILD_DIR/zephyr/zephyr.hex"

if [[ ! -f "$HEX" ]]; then
	echo "No build at $BUILD_DIR. Build first:" >&2
	echo "  west build -b lp_em_cc2340r5 asset_tracker/app -d $BUILD_DIR" >&2
	exit 1
fi

# crc_tool patches CRC32s into the CCFG region as a post-link step and the boot ROM
# verifies them, so an image built without it links fine and then does not run.
if ! grep -q "CRC_CCFG" "$BUILD_DIR/zephyr/zephyr.map" 2>/dev/null; then
	echo "warning: no CCFG CRC symbols in the map - is crc_tool installed?" >&2
	echo "         pip install lief && pip install --no-deps ti-simplelink-crc-tool" >&2
fi

if [[ -z "${DSLITE:-}" ]]; then
	DSLITE=$(ls -1 "$HOME"/ti/uniflash_*/dslite.sh /Applications/ti/uniflash_*/dslite.sh \
		2>/dev/null | head -1 || true)
fi

if [[ -z "$DSLITE" || ! -x "$DSLITE" ]]; then
	echo "dslite.sh not found. Install TI UniFlash, or point DSLITE at it:" >&2
	echo "  DSLITE=~/ti/uniflash_<version>/dslite.sh tools/flash.sh" >&2
	exit 1
fi

# --verbose is not optional: without it DSLite prints nothing at all, success included.
echo "Flashing $HEX"
"$DSLITE" --mode flash --config="$HERE/cc2340r5_xds110.ccxml" --verbose -u "$HEX"
