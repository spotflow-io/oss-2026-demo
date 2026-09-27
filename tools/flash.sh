#!/usr/bin/env bash
# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

#
# Flash the asset tracker onto an LP-EM-CC2340R5.
#
# The board has no onboard debugger. With an LP-XDS110ET the route is TI's own OpenOCD
# build, which ships inside CCS or UniFlash and is found through TI_OPENOCD_INSTALL_DIR.
# TI documents only J-Link as tested on this board, so this script checks what it can
# and tells you the fallbacks rather than failing obscurely.
#
# STATUS: the command-line path does NOT currently work. DSLite reaches the flash loader
# and fails with "Error -615: the target failed to see a correctly formatted SWD header",
# unresolved. Flash through the UniFlash GUI instead - see README.md, "Flashing".
#
# This script is kept because everything up to the connect is right, and because the
# config it points at (tools/cc2340r5_xds110.ccxml, exported from the GUI) is the one
# that matters: a hand-written config leaves SWD disabled and fails with -1170.
#
# Usage:  tools/flash.sh [build-dir]        (default: build/tracker)

set -euo pipefail

BUILD_DIR="${1:-build/tracker}"
WORKSPACE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cd "$WORKSPACE"

if [[ ! -f "$BUILD_DIR/zephyr/zephyr.hex" ]]; then
	echo "No build at $BUILD_DIR. Build first:"
	echo "  west build -b lp_em_cc2340r5 asset_tracker/app -d $BUILD_DIR"
	exit 1
fi

# crc_tool patches CRC32s into the CCFG region as a post-link step, and the boot ROM
# checks them. A hex built without it will not run, so make sure it was on PATH.
if ! grep -q "CRC_CCFG" "$BUILD_DIR/zephyr/zephyr.map" 2>/dev/null; then
	echo "warning: no CCFG CRC symbols in the map - is crc_tool installed?" >&2
	echo "         pip install lief && pip install --no-deps ti-simplelink-crc-tool" >&2
fi

RUNNER="${RUNNER:-openocd}"

if [[ "$RUNNER" == "openocd" && -z "${TI_OPENOCD_INSTALL_DIR:-}" ]]; then
	cat >&2 <<'EOF'
TI_OPENOCD_INSTALL_DIR is not set, and the board's OpenOCD runner needs TI's build of
OpenOCD (it ships with Code Composer Studio and with UniFlash).

  export TI_OPENOCD_INSTALL_DIR=/path/to/ccs/ccs_base/DebugServer

Alternatives:
  RUNNER=jlink tools/flash.sh     J-Link on the J4 header - the path TI tests
  UniFlash GUI                    load build/tracker/zephyr/zephyr.hex by hand
EOF
	exit 1
fi

echo "Flashing $BUILD_DIR with runner '$RUNNER'"
exec .venv/bin/west flash -d "$BUILD_DIR" -r "$RUNNER"
