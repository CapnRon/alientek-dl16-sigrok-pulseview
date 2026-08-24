# ALIENTEK DL16 Plus — Test & Validation Routine

Full-function validation of the `alientek-dl16` libsigrok driver against real
hardware, over a **direct USB 2.0 high-speed (480 Mbit/s) connection** (not
USB/IP, which previously capped capture at ~1–2 MHz).

## Prerequisites (shared)

- DL16 Plus on a direct USB 2.0 port (`lsusb` shows `1a86:ffcc`).
- libsigrok built with `--enable-alientek-dl16`, plus `sigrok-cli` and PulseView.
- A known test signal:
  - **Low rates:** FT232 UART @ 115200 baud → `D0` (continuous `ATK-DL16-TEST-0123456789\r\n`).
  - **High rates:** DL16's own **PWM output** (or a function generator) → `D0`.

```sh
# test signal source (UART)
python3 uart_loop.py /dev/ttyUSB0 115200     # "ATK-DL16-TEST-0123456789\r\n"

# convenience alias
SR="sigrok-cli -d alientek-dl16"
```

### Verify the USB link speed first

```sh
lsusb -v -d 1a86:ffcc | grep -E "bcdUSB|bEndpointAddress|wMaxPacketSize"
# expect: bcdUSB 2.00, EP 0x02 OUT, EP 0x81 IN, wMaxPacketSize 512
```

If the device enumerated as **full-speed (12 Mbit/s)** the high-rate tests will
fail — fix the port/cable/hub first.

---

# Part 1 — CLI (sigrok-cli)

## C1 — Device enumeration

```sh
sigrok-cli --scan
```

**Pass:** `alientek-dl16 - ALIENTEK DL16 Plus with 16 channels: D0 … D15`.

## C2 — Model / version identification

```sh
$SR --show | grep -iE "model|version"
```

**Pass:** model `ALIENTEK DL16 Plus`, version `FPGA 1.xx` (≥ 1.15).

## C3 — Samplerate sweep (stream mode, 16ch)

```sh
for r in 1m 2m 5m 10m 20m; do
  $SR -c samplerate=$r --channels=D0 --samples=100k -O binary -o /tmp/s_$r.bin
  echo "$r -> $(stat -c%s /tmp/s_$r.bin) bytes"
done
```

**Pass:** every rate captures data; rates > 20 MHz are rejected in stream mode.

## C4 — Samplerate sweep (buffer mode, 16ch)

```sh
for r in 1m 10m 50m 100m 250m 500m; do
  $SR -c samplerate=$r -c continuous=false --channels=D0 --samples=1M -O binary -o /tmp/b_$r.bin
  echo "$r -> $(stat -c%s /tmp/b_$r.bin) bytes"
done
```

**Pass:** captures up to 500 MHz (16ch). Verify against a known-frequency signal.

## C5 — Rate limits (stream 16ch)

```sh
$SR -c samplerate=20m --samples=1k --channels=D0..D15 -O binary -o /dev/null && echo OK
$SR -c samplerate=500m --samples=1k --channels=D0..D15 -O binary -o /dev/null && echo "UNEXPECTED"
```

**Pass:** 20 MHz accepted, 500 MHz rejected.

## C6 — Stream capture data integrity (UART decode)

```sh
$SR -c samplerate=1m --channels=D0 --samples=50k -O binary -o /tmp/u.bin
```

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

**Pass:** prints `ATK-DL16-TEST-0123456789\r\n…`.

## C7 — Buffer capture data integrity

```sh
$SR -c samplerate=10m -c continuous=false --channels=D0 --triggers D0=r --samples=100k -O binary -o /tmp/bt.bin
# decode as above with bit=10e6/115200
```

**Pass:** UART decodes; trigger crop aligns (order-3 `crop=` logged).

## C8 — RLE capture

```sh
$SR -c samplerate=1m -c rle=true --channels=D0 --samples=50k -O binary -o /tmp/r.bin
```

**Pass:** decodes identically to C6.

## C9 — Trigger types

```sh
for t in r f 1 0 e; do
  $SR -c samplerate=1m -c continuous=false --channels=D0 --triggers D0=$t --samples=50k -O binary -o /tmp/tr_$t.bin
  echo "trigger $t -> $(stat -c%s /tmp/tr_$t.bin) bytes"
done
```

**Pass:** all five (r/f/1/0/e) capture without error.

## C10 — Threshold

```sh
for v in 0.6 1.2 1.6 2.5 3.3; do
  $SR -c samplerate=1m -c voltage_threshold=$v --channels=D0 --samples=10k -O binary -o /tmp/th.bin
  echo "threshold $v -> $(stat -c%s /tmp/th.bin) bytes"
done
```

**Pass:** captures at each threshold; 3.3 V signal reads high ≤1.6 V, low ≥2.5 V.

## C11 — PWM output (both channels)

```sh
$SR -c output_frequency=1000 -c output_duty_cycle=50 --samples=1k -O binary -o /dev/null   # PWM1
./dl16_probe pwm 1000 50 1   # PWM2 (standalone probe)
```

**Pass:** scope shows 1 kHz 50% on both PWM pins; sweep 1 Hz→6 MHz @ 50%.

## C12 — Long continuous capture (stream)

```sh
timeout 30 $SR -c samplerate=1m --channels=D0 -O binary -o /tmp/long.bin
```

**Pass:** runs full duration, no `LIBUSB_TRANSFER` errors in `-l 5` output.

---

# Part 2 — PulseView (GUI)

Launch: `pulseview` (or `DISPLAY=:0 pulseview`).

## P1 — Device appears

**Steps:** open the device dropdown (top-left).

**Pass:** `ALIENTEK DL16 Plus` is listed with 16 channels.

## P2 — Capture & waveform display

**Steps:**
1. Select `ALIENTEK DL16 Plus`.
2. Set sample rate (e.g. 1 MHz) in the toolbar.
3. Enable `D0`.
4. Press **Run**.

**Pass:** a live waveform appears on D0; with the UART source connected you see
the square-wave UART pattern scrolling.

## P3 — Protocol decode (UART)

**Steps:**
1. With D0 capturing, click **Add decoder → UART**.
2. Map RX → D0, set baud 115200.

**Pass:** the decoded text `ATK-DL16-TEST-0123456789` is shown as annotations
on the waveform.

## P4 — Trigger

**Steps:**
1. Stop capture, click the trigger button for D0, choose **Rising edge**.
2. Press Run.

**Pass:** capture waits for a rising edge; waveform is aligned at the trigger
point (capture ratio respected).

## P5 — Threshold

**Steps:** set threshold via the device options (e.g. 1.6 V for a 3.3 V signal).

**Pass:** D0 toggles correctly at the chosen threshold.

## P6 — Buffer mode (high sample rate)

**Steps:**
1. Set device option `continuous` off (buffer mode).
2. Set sample rate 10 MHz (or higher), enable fewer channels.
3. Run.

**Pass:** a one-shot capture completes and displays (this exercises the
buffer-mode trigger crop path).

## P7 — RLE

**Steps:** enable `rle` in device options, run a capture.

**Pass:** waveform decodes identically to a non-RLE capture.

## P8 — Export

**Steps:** **File → Save As → CSV** (and/or srzip).

**Pass:** the exported file is non-empty and contains the D0 samples.

## P9 — PWM (device options)

**Steps:** set `output_frequency` / `output_duty_cycle` on `PWM1`.

**Pass:** scope shows the requested PWM frequency/duty on the PWM1 pin.

---

## Summary checklist

| CLI | | PulseView | |
|-----|-|-----------|-|
| C1 enumerate | ☐ | P1 device appears | ☐ |
| C2 model/version | ☐ | P2 capture/display | ☐ |
| C3 stream rates | ☐ | P3 UART decode | ☐ |
| C4 buffer rates →500 MHz | ☐ | P4 trigger | ☐ |
| C5 rate limits | ☐ | P5 threshold | ☐ |
| C6 stream integrity | ☐ | P6 buffer mode | ☐ |
| C7 buffer integrity | ☐ | P7 RLE | ☐ |
| C8 RLE | ☐ | P8 export | ☐ |
| C9 trigger ×5 | ☐ | P9 PWM | ☐ |
| C10 threshold | ☐ | | |
| C11 PWM ×2 | ☐ | | |
| C12 continuous | ☐ | | |

Run `sigrok-cli -l 5 …` (or `pulseview -l 5`) on any failing test and attach the log.
