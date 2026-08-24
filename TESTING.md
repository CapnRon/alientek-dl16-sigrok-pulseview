# ALIENTEK DL16 Plus — Test & Validation Routine

Full-function validation of the `alientek-dl16` libsigrok driver against real
hardware, over a **direct USB 2.0 high-speed (480 Mbit/s) connection** (not
USB/IP, which previously capped capture at ~1–2 MHz).

## Prerequisites

- DL16 Plus on a direct USB 2.0 port (`lsusb` shows `1a86:ffcc`, and
  `lsusb -v -d 1a86:ffcc` reports `bcdUSB 2.00` + bulk endpoints `0x02`/`0x81`).
- libsigrok built with `--enable-alientek-dl16`, plus `sigrok-cli` and PulseView.
- A known test signal:
  - **Low rates:** FT232 UART @ 115200 baud → `D0` (continuous `ATK-DL16-TEST-0123456789\r\n`).
  - **High rates:** DL16's own **PWM output** (or a function generator) → `D0`.

```sh
# test signal source (UART)
python3 uart_loop.py /dev/ttyUSB0 115200     # "ATK-DL16-TEST-0123456789\r\n"

# convenience aliases
SR="sigrok-cli -d alientek-dl16"
SCAN="sigrok-cli --scan"
```

### Verify the USB link speed first

```sh
lsusb -v -d 1a86:ffcc | grep -E "bcdUSB|bEndpointAddress|wMaxPacketSize"
# expect: bcdUSB 2.00, EP 0x02 OUT, EP 0x81 IN, wMaxPacketSize 512
```

If the device enumerated as **full-speed (12 Mbit/s)** the high-rate tests will
fail — fix the port/cable/hub first.

---

## T1 — Device enumeration

```sh
$SCAN
```

**Pass:** `alientek-dl16 - ALIENTEK DL16 Plus with 16 channels: D0 … D15`.

## T2 — Model / version identification

```sh
$SR --show | grep -iE "model|version"
```

**Pass:** model `ALIENTEK DL16 Plus`, version `FPGA 1.xx` (≥ 1.15).

## T3 — FPGA version gate

**Pass:** opening succeeds for FPGA ≥ 1.15. (To test the rejection path,
flash older firmware — skipped by default; the code path is unit-testable.)

---

## T4 — Samplerate sweep (stream mode, 16ch)

```sh
for r in 1m 2m 5m 10m 20m; do
  $SR -c samplerate=$r --channels=D0 --samples=100k -O binary -o /tmp/s_$r.bin
  echo "$r -> $(stat -c%s /tmp/s_$r.bin) bytes"
done
```

**Pass:** every rate captures data (non-zero bytes); UART decode succeeds at
≤ 2 MHz (see T7). Rates > 20 MHz are correctly rejected in stream mode.

## T5 — Samplerate sweep (buffer mode, 16ch)

```sh
for r in 1m 10m 50m 100m 250m 500m; do
  $SR -c samplerate=$r -c continuous=false --channels=D0 --samples=1M -O binary -o /tmp/b_$r.bin
  echo "$r -> $(stat -c%s /tmp/b_$r.bin) bytes"
done
```

**Pass:** captures up to 500 MHz (16ch). Verify each against a known-frequency
signal (e.g. feed the DL16 PWM output into D0 and measure the captured period).

## T6 — Channel-count / rate limits

```sh
# 16ch stream: max 20 MHz
$SR -c samplerate=20m --samples=1k --channels=D0..D15 -O binary -o /dev/null && echo OK
$SR -c samplerate=500m --samples=1k --channels=D0..D15 -O binary -o /dev/null && echo "UNEXPECTED"
# (the 500m stream request must be rejected)
```

**Pass:** 20 MHz accepted, 500 MHz rejected for 16ch stream.

---

## T7 — Stream capture data integrity (UART decode)

```sh
$SR -c samplerate=1m --channels=D0 --samples=50k -O binary -o /tmp/u.bin
```

Decode and confirm the known message:

```python
d = open('/tmp/u.bin','rb').read()
bits = [(d[i]|(d[i+1]<<8))&1 for i in range(0,len(d)-1,2)]
bit = 1e6/115200; out = bytearray(); i = 1
while i < len(bits)-int(bit*10):
    if bits[i]==0 and bits[i-1]==1:
        b=0; ok=True
        for x in range(8):
            c=i+int(bit*(1.5+x))
            if c-1<0 or c+1>=len(bits): ok=False; break
            if bits[c-1]+bits[c]+bits[c+1]>=2: b|=(1<<x)
        if ok: out.append(b); i+=int(bit*10); continue
    i+=1
print(bytes(out[:40]))
```

**Pass:** prints `ATK-DL16-TEST-0123456789\r\n…` (LSB-first, correct rate).

## T8 — Buffer capture data integrity

```sh
$SR -c samplerate=10m -c continuous=false --channels=D0 --triggers D0=r --samples=100k -O binary -o /tmp/bt.bin
# decode as above with bit=10e6/115200
```

**Pass:** UART decodes; trigger crop aligns (order-3 `crop=` logged, no errors).

## T9 — RLE capture

```sh
$SR -c samplerate=1m -c rle=true --channels=D0 --samples=50k -O binary -o /tmp/r.bin
```

**Pass:** decodes identically to T7 (RLE expansion correct).

---

## T10 — Trigger types

Run each; confirm the capture completes and decodes (trigger fires on the
continuous UART edges):

```sh
$SR -c samplerate=1m -c continuous=false --channels=D0 --triggers D0=r --samples=50k -O binary -o /tmp/tr.bin   # rising
$SR ... --triggers D0=f ...   # falling
$SR ... --triggers D0=1 ...   # high level
$SR ... --triggers D0=0 ...   # low level
$SR ... --triggers D0=e ...   # any edge
```

**Pass:** all five trigger modes capture data without error.

## T11 — Threshold

```sh
for v in 0.6 1.2 1.6 2.5 3.3; do
  $SR -c samplerate=1m -c voltage_threshold=$v --channels=D0 --samples=10k -O binary -o /tmp/th.bin
  echo "threshold $v -> $(stat -c%s /tmp/th.bin) bytes"
done
```

**Pass:** captures at each threshold; a 3.3 V signal reads high at ≤ 1.6 V and
reads low at ≥ 2.5 V (sanity-check the comparator polarity).

## T12 — PWM output (both channels)

Use the driver config keys (or the standalone probe):

```sh
# via config keys (PWM1)
sigrok-cli -d alientek-dl16 -c output_frequency=1000 -c output_duty_cycle=50 --samples=1k -O binary -o /dev/null
# or via probe
./dl16_probe pwm 1000 50 0   # PWM1, 1 kHz 50%
./dl16_probe pwm 1000 50 1   # PWM2, 1 kHz 50%
```

**Pass:** scope shows 1 kHz 50% on both PWM pins. Sweep 1 Hz → 6 MHz @ 50%
(validated sweet spot; non-50% duty deforms the waveform — expected).

## T13 — Long continuous capture (stream)

```sh
timeout 30 $SR -c samplerate=1m --channels=D0 -O binary -o /tmp/long.bin
```

**Pass:** captures continuously for the full duration without stalling or
USB errors (no `LIBUSB_TRANSFER` errors in `-l 5` output).

---

## Summary checklist

| Test | Result |
|------|--------|
| T1 enumerate | ☐ |
| T2 model/version | ☐ |
| T3 version gate | ☐ |
| T4 stream rates | ☐ |
| T5 buffer rates (→500 MHz) | ☐ |
| T6 rate limits | ☐ |
| T7 stream integrity | ☐ |
| T8 buffer integrity | ☐ |
| T9 RLE | ☐ |
| T10 trigger ×5 | ☐ |
| T11 threshold | ☐ |
| T12 PWM ×2 | ☐ |
| T13 continuous | ☐ |

Run `sigrok-cli -l 5 …` on any failing test and attach the log.
