#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S tests/sdr/ui_host -B build/sdr-ui-host -G Ninja
cmake --build build/sdr-ui-host -j 8
mkdir -p build/sdr-ui-host/previews
build/sdr-ui-host/test_sdr_ui build/sdr-ui-host/previews
