# ALIENTEK DL16 Plus · sigrok + PulseView

A native [libsigrok](https://sigrok.org/) hardware driver that adds full
**ALIENTEK DL16 / DL16 Plus** logic analyzer support to **sigrok-cli** and
**PulseView**.

This is a fork of [sigrok-project/libsigrok](https://github.com/sigrokproject/libsigrok)
with one addition: the `alientek-dl16` driver. The device protocol was
reverse-engineered from ALIENTEK's GPL "ATK-Logic" host software and
reimplemented cleanly for upstreaming.

<div align="center">

| | |
|---|---|
| **Device** | ALIENTEK DL16 / DL16 Plus (16-channel logic analyzer) |
| **USB ID** | `1a86:ffcc` |
| **License** | GPL v3 or later |
| **Upstream** | [sigrok-project/libsigrok](https://github.com/sigrokproject/libsigrok) |

</div>

---

## ✨ Features

| Feature | Details |
|---------|---------|
| **16 logic channels** | `D0` … `D15` |
| **Sample rates** | 1–500 MHz (buffer mode) · 1–20 MHz (stream mode, 16ch) |
| **Capture modes** | Stream · Buffer · RLE |
| **Trigger** | Rising / Falling / High / Low / Any-edge, with buffer-mode trigger crop |
| **Threshold** | −5 V … +5 V, 0.1 V steps |
| **PWM output** | 2 channels, 1 Hz … 20 MHz |
| **Model detection** | Auto-detects "DL16" vs "DL16 Plus" |
| **Version gate** | Refuses to open devices below FPGA firmware 1.15 |

## 🔌 Hardware

| Parameter | DL16 | DL16 Plus |
|-----------|------|-----------|
| Channels | 16 | 16 |
| Max rate (buffer) | 250 MHz (16ch) · 100 MHz (3ch) | 1 GHz (8ch) · 500 MHz (16ch) · 100 MHz (3ch) |
| Max rate (stream) | 20 MHz (16ch) · 50 MHz (6ch) · 100 MHz (3ch) | same |
| Storage depth | 1 Gbit | 3.5 Gbit |
| Min pulse width | 8 ns | 2 ns |
| Input voltage | −40 V … +40 V | same |
| Input impedance | 250 kΩ / 15 pF | same |

## 📦 Status

| Platform | Status |
|----------|--------|
| **Linux** | ✅ Verified — `sigrok-cli` + PulseView, live capture decodes UART, trigger/RLE/PWM exercised |
| **Windows** | 🚧 Cross-compile in progress (MXE); release binaries will be attached |

## 🛠️ Building (Linux)

```sh
git clone https://github.com/CapnRon/alientek-dl16-sigrok-pulseview.git
cd alientek-dl16-sigrok-pulseview
./autogen.sh
./configure --disable-all-drivers --enable-alientek-dl16
make
sudo make install
```

## 📟 Usage

```sh
# Enumerate devices
sigrok-cli --scan
# → alientek-dl16 - ALIENTEK DL16 Plus with 16 channels: D0 … D15

# Capture channel 0 at 1 MHz
sigrok-cli -d alientek-dl16 -c samplerate=1m \
    --channels=D0 --samples=1M -O binary -o capture.bin

# Buffer-mode capture with a rising-edge trigger on D0
sigrok-cli -d alientek-dl16 -c samplerate=10m -c continuous=false \
    --channels=D0 --triggers D0=r --samples=100k -O binary -o trig.bin
```

In **PulseView**: the device appears under *Devices → ALIENTEK DL16 Plus* once
the driver is installed.

### Config keys

`SR_CONF_SAMPLERATE`, `SR_CONF_LIMIT_SAMPLES`, `SR_CONF_LIMIT_FRAMES`,
`SR_CONF_CAPTURE_RATIO`, `SR_CONF_RLE`, `SR_CONF_CONTINUOUS`,
`SR_CONF_VOLTAGE_THRESHOLD`, `SR_CONF_TRIGGER_MATCH`,
`SR_CONF_OUTPUT_FREQUENCY`, `SR_CONF_DUTY_CYCLE`.

## 🚀 Upstreaming

The driver is a clean **10-commit series** (DCO signed-off) based on upstream
`master`, intended for submission as a pull request to
`sigrok-project/libsigrok`. See:

```sh
git log master -- src/hardware/alientek-dl16
```

## 📁 Driver files

```
src/hardware/alientek-dl16/
├── api.c        # scan / open / close, config keys
├── protocol.c   # USB framing, CRC, capture, data transpose
└── protocol.h   # constants, device context
```

## 📄 License

GPL v3 or later — same as libsigrok.
