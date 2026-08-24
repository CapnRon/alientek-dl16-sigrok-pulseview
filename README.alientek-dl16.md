# ALIENTEK DL16 / DL16 Plus driver

This fork adds a libsigrok hardware driver for the **ALIENTEK DL16 Plus**
logic analyzer (USB `1a86:ffcc`).

The protocol was reverse-engineered from ALIENTEK's GPL "ATK-Logic" host
software and reimplemented cleanly under the GPL for upstreaming into
[sigrok-project/libsigrok](https://github.com/sigrokproject/libsigrok).

## Driver

- `src/hardware/alientek-dl16/api.c` — scan/open/close, config keys
- `src/hardware/alientek-dl16/protocol.c` — USB framing, CRC, capture, data path
- `src/hardware/alientek-dl16/protocol.h` — constants, device context

Registered in `configure.ac` (`SR_DRIVER([ALIENTEK DL16], [alientek-dl16], [libusb])`)
and `Makefile.am`.

## Features

| Feature | Notes |
|---------|-------|
| 16 logic channels | `D0`–`D15` |
| Samplerate | 1–500 MHz buffer mode; 1–20 MHz stream mode (16ch) |
| Capture modes | stream / buffer / RLE |
| Trigger | rising / falling / high / low / any-edge (+ buffer-mode trigger crop) |
| Threshold | −5 V … +5 V (0.1 V step) |
| PWM output | 2 channels, 1 Hz … 20 MHz (`SR_CONF_OUTPUT_FREQUENCY` + `SR_CONF_DUTY_CYCLE`) |
| Model detection | reports "DL16 Plus" via MCU version level |
| FPGA version gate | refuses open below firmware 1.15 |

Config keys: `SR_CONF_SAMPLERATE`, `SR_CONF_LIMIT_SAMPLES/FRAMES`,
`SR_CONF_CAPTURE_RATIO`, `SR_CONF_RLE`, `SR_CONF_CONTINUOUS`,
`SR_CONF_VOLTAGE_THRESHOLD`, `SR_CONF_TRIGGER_MATCH`,
`SR_CONF_OUTPUT_FREQUENCY`, `SR_CONF_DUTY_CYCLE`.

## Status

**Linux:** verified end-to-end with `sigrok-cli` and PulseView against real
hardware — capture decodes a UART test signal correctly, trigger/PWM/RLE
exercised live.

**Windows:** cross-compile in progress (MXE). A release with the Windows
binaries will be attached once complete.

## Building (Linux)

```sh
./autogen.sh
./configure --disable-all-drivers --enable-alientek-dl16
make
sudo make install
```

Then:

```sh
sigrok-cli --scan                 # shows "ALIENTEK DL16 Plus"
sigrok-cli -d alientek-dl16 --samples=1M --channels=D0 -O binary -o capture.bin
```

## Upstreaming

The driver is a clean 10-commit series (DCO signed-off) based on upstream
`master`. It is intended for submission as a PR to
`sigrok-project/libsigrok`. Patches are available in this repository's history
(`git log master -- src/hardware/alientek-dl16`).

## License

GPL v3 or later (same as libsigrok).
