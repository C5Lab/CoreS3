#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output_dir="$(mktemp -d "${TMPDIR:-/tmp}/cores3-sdr-tests.XXXXXX")"
trap 'rm -rf "$output_dir"' EXIT
includes=(-Itests/sdr/host -Imain/sdr -Icomponents/esp_sdr/include -Icomponents/esp-dsp/modules/fft/include -Icomponents/esp-dsp/modules/common/include)
sanitizers=(-fsanitize=address,undefined -g)
cc -std=c11 "${sanitizers[@]}" "${includes[@]}" -c components/esp-dsp/modules/fft/float/dsps_fft2r_fc32_ansi.c -o "$output_dir/fft.o"
cc -std=c11 "${sanitizers[@]}" "${includes[@]}" -c components/esp-dsp/modules/fft/float/dsps_fft2r_bitrev_tables_fc32.c -o "$output_dir/tables.o"
c++ -std=c++17 "${sanitizers[@]}" "${includes[@]}" tests/sdr/test_signal.cpp main/sdr/spectrum.cpp main/sdr/settings.cpp main/sdr/test_tx_frame.cpp components/esp-dsp/modules/common/misc/dsps_pwroftwo.cpp "$output_dir/fft.o" "$output_dir/tables.o" -o "$output_dir/test_signal"
"$output_dir/test_signal"
c++ -std=c++17 -pthread "${sanitizers[@]}" "${includes[@]}" tests/sdr/test_service.cpp main/sdr/service.cpp main/sdr/spectrum.cpp main/sdr/settings.cpp main/sdr/test_tx.cpp main/sdr/test_tx_frame.cpp components/esp-dsp/modules/common/misc/dsps_pwroftwo.cpp "$output_dir/fft.o" "$output_dir/tables.o" -o "$output_dir/test_service"
"$output_dir/test_service"
