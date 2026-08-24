# ALIENTEK DL16 Plus · sigrok + PulseView

A native [libsigrok](https://sigrok.org/) hardware driver that adds full
**ALIENTEK DL16 / DL16 Plus** logic analyzer support to **sigrok-cli** and
**PulseView**.

This is a fork of [sigrok-project/libsigrok](https://github.com/sigrokproject/libsigrok)
with one addition: the `alientek-dl16` driver. The device protocol was
reverse-engineered from ALIENTEK's GPL "ATK-Logic" host software and
reimplemented cleanly for upstreaming.

<p align="center">
  <img src="doc/img/dl16-1.png" width="220" alt="ALIENTEK DL16" />
  <img src="doc/img/dl16-2.png" width="220" alt="ALIENTEK DL16" />
  <img src="doc/img/dl16-3.png" width="220" alt="ALIENTEK DL16" />
</p>

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

## 📦 Dependencies (Linux)

One command installs everything needed to build libsigrok (this repo),
libsigrokdecode, and PulseView (Debian/Ubuntu):

```sh
sudo apt-get update && sudo apt-get install -y \
  git build-essential autoconf automake libtool pkg-config cmake \
  libglib2.0-dev libglibmm-2.4-dev \
  libusb-1.0-0-dev libzip-dev libftdi1-dev libhidapi-dev libserialport-dev \
  python3-dev swig doxygen \
  qtbase5-dev qtbase5-dev-tools qttools5-dev qttools5-dev-tools libqt5svg5-dev \
  libboost-dev libboost-filesystem-dev libboost-serialization-dev
```

## 🛠️ Building (Linux)

Build and install in this order (all into `/usr/local`). The three pieces
must come from matching git snapshots — mixing a distro PulseView with a
git libsigrok fails with an undefined-symbol / `invalid argument` error.

### 1. libsigrok — this repo (adds the `alientek-dl16` driver)

```sh
git clone https://github.com/CapnRon/alientek-dl16-sigrok-pulseview.git
cd alientek-dl16-sigrok-pulseview
chmod +x autogen.sh   # required if the script isn't executable
./autogen.sh
./configure --enable-cxx --disable-python --disable-java --disable-ruby --prefix=/usr/local
make -j$(nproc)
sudo make install
```

`--enable-cxx` is required: PulseView links `libsigrokcxx` (the C++ bindings).

> **sigrok-cli only** (no PulseView): use the faster minimal build instead —
> `./configure --disable-all-drivers --enable-alientek-dl16`

### 2. libsigrokdecode (upstream)

```sh
cd ~
git clone https://github.com/sigrokproject/libsigrokdecode.git
cd libsigrokdecode
./autogen.sh
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install
```

### 3. PulseView (upstream)

```sh
cd ~
git clone https://github.com/sigrokproject/pulseview.git
cd pulseview
mkdir -p build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
```

### 4. Refresh the loader cache

```sh
sudo ldconfig
```

### 5. USB device access (udev)

Allow non-root access to the analyzer (`1a86:ffcc`):

```sh
echo 'SUBSYSTEMS=="usb", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="ffcc", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/60-alientek-dl16.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and replug the analyzer, then verify below.

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

In **PulseView**: open *File → Connect to Device* (or use the top-left
drop-down), select **ALIENTEK DL16**, set the sample rate and channels, then
press **Run**. PWM output is exposed as the **PWM1** / **PWM2** channel groups.

### Config keys

Pass these with `-c key=value` in `sigrok-cli` (or in PulseView's device
options). The `SR_CONF_*` names are the driver's internal enum identifiers —
the **CLI key** column is what you actually type:

| CLI key | Type | Meaning / valid values |
|---------|------|------------------------|
| `samplerate` | uint64 | Sample rate. Buffer mode: `1m 2m 4m 5m 10m 20m 25m 40m 50m 100m 200m 250m 500m`. Stream mode (`continuous=true`): `1m … 20m`. |
| `continuous` | bool | `true` = stream mode (default), `false` = buffer mode. |
| `limit_samples` | uint64 | Number of samples to capture. |
| `limit_frames` | uint64 | Number of frames to capture. |
| `captureratio` | uint64 | Pre-trigger capture ratio, 0–100 (% of buffer before the trigger). |
| `rle` | bool | Run-length-encoded capture. |
| `voltage_threshold` | float | Input threshold, −6.0 … +6.0 V in 0.1 V steps. |
| `output_frequency` | float | PWM output frequency, 1 Hz … 20 MHz (set per PWM1/PWM2 group). |
| `output_duty_cycle` | float | PWM duty cycle, 0–100 %. |

Triggers use `--triggers` (not `-c`): `--triggers D0=r` rising, `D0=f` falling,
`D0=0` low, `D0=1` high, `D0=e` any edge.

## 🚀 Upstreaming

The `alientek-dl16` driver is a self-contained series of commits in
`src/hardware/alientek-dl16/`, intended for submission as a pull request to
`sigrok-project/libsigrok`:

```sh
git log master -- src/hardware/alientek-dl16
```

This fork also carries the README, build documentation, and a udev entry for
`1a86:ffcc` on top of upstream `master`.

## 📁 Driver files

```
src/hardware/alientek-dl16/
├── api.c        # scan / open / close, config keys
├── protocol.c   # USB framing, CRC, capture, data transpose
└── protocol.h   # constants, device context
```

## 📄 License

GPL v3 or later — same as libsigrok.

---


## Original upstream README (unchanged)

```text
-------------------------------------------------------------------------------
README
-------------------------------------------------------------------------------

The sigrok project aims at creating a portable, cross-platform,
Free/Libre/Open-Source signal analysis software suite that supports various
device types (such as logic analyzers, oscilloscopes, multimeters, and more).

libsigrok is a shared library written in C which provides the basic API
for talking to hardware and reading/writing the acquired data into various
input/output file formats.


Status
------

libsigrok is in a usable state and has had official tarball releases.

While the API can change from release to release, this will always be
properly documented and reflected in the package version number and
in the shared library / libtool / .so-file version numbers.

However, there are _NO_ guarantees at all for stable APIs in git snapshots!
Distro packagers should only use released tarballs (no git snapshots).


Requirements
------------

Requirements for the C library:

 - git (only needed when building from git)
 - gcc (>= 4.0) or clang
 - make
 - autoconf >= 2.63 (only needed when building from git)
 - automake >= 1.11 (only needed when building from git)
 - libtool (only needed when building from git)
 - pkg-config >= 0.22
 - libglib >= 2.32.0
 - zlib (optional, used for CRC32 calculation in STF input)
 - libzip >= 0.11
 - libtirpc (optional, used by VXI, fallback when glibc >= 2.26)
 - libserialport >= 0.1.1 (optional, used by some drivers)
 - librevisa >= 0.0.20130412 (optional, used by some drivers)
 - libusb-1.0 >= 1.0.16 (optional, used by some drivers)
 - hidapi >= 0.8.0 (optional, used for some HID based "serial cables")
 - bluez/libbluetooth >= 4.0 (optional, used for Bluetooth/BLE communication)
 - libftdi1 >= 1.0 (optional, used by some drivers)
 - libgpib (optional, used by some drivers)
 - libieee1284 (optional, used by some drivers)
 - libgio >= 2.32.0 (optional, used by some drivers)
 - nettle (optional, used by some drivers)
 - check >= 0.9.4 (optional, only needed to run unit tests)
 - doxygen (optional, only needed for the C API docs)
 - graphviz (optional, only needed for the C API docs)

Requirements for the C++ bindings:

 - libsigrok >= 0.4.0 (the libsigrok C library, see above)
 - A C++ compiler with C++11 support (-std=c++11 option), e.g.
   - g++ (>= 4.8.1)
   - clang++ (>= 3.3)
 - autoconf-archive (only needed when building from git)
 - doxygen (required for building the bindings, not only for C++ API docs!)
 - graphviz (optional, only needed for the C++ API docs)
 - Python 3 executable (development files are not needed)
 - glibmm-2.4 (>= 2.32.0) or glibmm-2.68 (>= 2.68.0)

Requirements for the Python bindings:

 - libsigrokcxx >= 0.4.0 (the libsigrok C++ bindings, see above)
 - SWIG >= 2.0.0
 - Python >= 3 (including development files!)
 - Python setuptools
 - pygobject >= 3.0.0, a.k.a python-gi
 - numpy
 - doxygen (optional, only needed for the Python API docs)
 - graphviz (optional, only needed for the Python API docs)
 - doxypy (optional, only needed for the Python API docs)

Requirements for the Ruby bindings:

 - libsigrokcxx >= 0.4.0 (the libsigrok C++ bindings, see above)
 - Ruby >= 2.5.0 (including development files!)
 - SWIG >= 3.0.8
 - YARD (optional, only needed for the Ruby API docs)

Requirements for the Java bindings:

 - libsigrokcxx >= 0.4.0 (the libsigrok C++ bindings, see above)
 - SWIG >= 2.0.0
 - Java JDK (for JNI includes and the javac/jar binaries)
 - doxygen (optional, only needed for the Java API docs)
 - graphviz (optional, only needed for the Java API docs)


Building and installing
-----------------------

In order to get the libsigrok source code and build it, run:

 $ git clone git://sigrok.org/libsigrok
 $ cd libsigrok
 $ ./autogen.sh
 $ ./configure
 $ make

For installing libsigrok:

 $ make install

See INSTALL or the following wiki page for more (OS-specific) instructions:

 http://sigrok.org/wiki/Building

Please also check the following wiki page in case you encounter any issues:

 http://sigrok.org/wiki/Building#FAQ


Device-specific issues
----------------------

Please check README.devices for some notes and hints about device- or
driver-specific issues to be aware of.


Firmware
--------

Some devices supported by libsigrok need a firmware to be uploaded before the
device can be used. See README.devices for details.


Copyright and license
---------------------

libsigrok is licensed under the terms of the GNU General Public License
(GPL), version 3 or later.

While some individual source code files are licensed under the GPLv2+, and
some files are licensed under the GPLv3+, this doesn't change the fact that
the library as a whole is licensed under the terms of the GPLv3+.

Please see the individual source files for the full list of copyright holders.


Mailing list
------------

 https://lists.sourceforge.net/lists/listinfo/sigrok-devel


IRC
---

You can find the sigrok developers in the #sigrok IRC channel on Libera.Chat.


Website
-------

 http://sigrok.org/wiki/Libsigrok

```
