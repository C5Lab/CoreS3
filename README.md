# CoreS3
Janos on Core S3

## SDR

The last home tile, **SDR**, runs the local ESP32-S3 snapshot receiver on the
CoreS3 itself. The existing JanOS UART features continue to use the external
device. No external SDR tuner is required.

- Spectrum and scrolling waterfall with Ocean, Fire and Gray palettes.
- Tuning from 2200 to 2700 MHz, 1/5/10 MHz steps, touch keypad, tap-to-center
  and horizontal drag tuning.
- 16/40/80 MS/s snapshots and 256/512/1024/2048-point FFT, Hann window,
  optional DC removal, linear-power averaging, peak hold and ADC clipping status.
- Hardware AGC or manual gain index, RF filter selection, relative dBFS scale
  and 5/10/15/20 spectrum updates per second.
- Save/Cancel/Defaults settings, isolated `sdr32` NVS storage, deferred writes
  after 2.5 seconds and a final flush when the screen closes.

Display theme, brightness, screen timeout and screen lock follow CoreS3.
SDR has no independent backlight setting. Plot refreshes do not count as user
activity. Settings and frequency entry pause acquisition; closing SDR stops
the local radio and frees the session buffers. A new session waits for any
previous session to finish shutting down.

This is a spectrum analyzer around 2.4 GHz, without audio demodulation or
88-108 MHz FM reception. Snapshots have gaps between captures. Levels are
relative dBFS, not calibrated dBm. The analog filter width is approximate;
`EXP` marks tuning outside 2412-2484 MHz. A successful capture does not establish
PLL lock or useful RF reception outside the nominal Wi-Fi band.

### TX

TX requires **Red Team** in CoreS3 Settings. Setup and confirmation screens
precede Start; Stop TX or leaving SDR stops transmission. RX is suspended
during TX and restored afterward. A failed PHY restoration blocks further
captures for that session; use Back to close and reopen the receiver.

The migrated behavior deliberately matches the local SDR32 variant:

- Channels 1-11 and durations 1/5/10/360 seconds.
- Stored rate selections 1/5/1000. The selection is **not an enforced rate**:
  the worker attempts one frame per loop and yields for 5 ms between iterations.
- No frame-count cap. The display shows actual sent/failed counts and time left.
- The backend calls `esp_wifi_set_max_tx_power(84)`; the SDK/PHY applies the
  supported power cap. This is not a measured RF power guarantee.
- Directed Probe Requests for `SDR32-TEST`, the device's station MAC and a
  random 32-byte vendor payload, preserving the local frame format.

### Build and verification

Use the project's ESP-IDF 6.0 environment and existing Component Manager setup:

```sh
idf.py build
bash tools/test_sdr_host.sh
bash tools/test_sdr_ui.sh
python tools/check_sram.py build/coreS3.elf
```

The build preserves the existing partition table and binary export workflow.
Only PHY certification/debug support and the DSP FFT capacity are added to
the CoreS3 configuration. The 64 KiB modem SRAM bank at
`0x3fcd0000-0x3fce0000` is reserved for the entire firmware lifetime, including
when SDR is closed. Linker assertions and the automatic ELF check guard both
the bank and its IRAM alias.

Host tests use the production DSP, settings, frame builder, TX scheduler and
worker with simulated RF and task/queue adapters. ASan and UBSan cover signal
processing, settings validation, RX pause, TX permission, exit during TX,
queued final edits, reopening and error cleanup. These tests do not validate
physical RF behavior or display/touch hardware.

The UI test uses the real LVGL renderer and production SDR screen with a
simulated worker. It exercises tap/drag tuning, Save/Cancel, keypad validation,
TX navigation, screen deletion and touch blocking by an overlay. Preview
images for both themes and the subpages are written to
`build/sdr-ui-host/previews/`. Neither the top-bar test adapter nor simulated
touch events substitute for checks on the physical CoreS3.

Hardware acceptance after flashing: test 2412/2437/2462 MHz with a known source,
axis direction, all sample rates/FFT sizes, AGC/manual gain, clipping, tuning
gestures and keypad. Verify Save/Cancel/Defaults and persistence after restart,
both themes, screen timeout/wake/lock, Red Team off/on, TX stop/exit and repeated
SDR reopening. Check that the existing UART screens still work afterward.
Hardware validation has not been performed as part of this migration; no
device has been flashed.

SDR uses private PHY interfaces tied to the local ESP-IDF build. Compilation
and an ELF check do not replace hardware validation when changing the SDK.
See [THIRD_PARTY.md](THIRD_PARTY.md) for source revisions and licensing.
