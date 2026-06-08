# R503 Arduino Fingerprint Scanner

The goal of this project is to create a DIY fingerprint scanner for Linux with `fprint` support. The role of the Arduino board is to bridge communications between the R503 scanner and the OS. This repository contains **an Arduino sketch**, **two `libfprint-tod` virtual device drivers** (one **daemon-less**, another one **daemon-based**), and also **a bridge daemon**.

## Hardware prerequisites

- R503 fingerprint scanner (tested on V1.3)
- Arduino board (tested on Nano v3)
- Proper connection between R503 and Arduino (see [references](#References) below)
- USB serial connection between Arduino and PC

## Installation

### Arduino bridge

Upload `r503_bridge.ino` to your Arduino board.

### libfprint-tod driver (daemon-less)

> Note: A software fix can be applied on Linux to disable **DTR resets of Arduino boards**. Thus, a daemon is no longer necessary. See the legacy section below for details.
> - `stty -F /dev/ttyUSB0 -hupcl` prevents the serial port from hanging up and resetting the Arduino.
> - `tty.c_cflag &= ~HUPCL;` in the driver code does the same thing.

If you are on Arch-based distro, install `libfprint-tod` from [AUR](https://aur.archlinux.org/packages/libfprint-tod). Verify the installation afterwards:

```bash
pkg-config --modversion libfprint-2-tod-1
```

Build and install the driver:

```bash
meson setup build
ninja -C build
sudo ninja -C build install
sudo systemctl restart fprintd
```

Verify the installation:

```bash
fprintd-list $USER
```

#### Configuration for daemon-less driver

- `R503_ARDUINO_SERIAL`: path to Arduino serial port (defaults to `/dev/ttyUSB0`)
- `fprintd.service.d/r503-arduino.conf`: a systemd drop-in file which sets `R503_ARDUINO_DEVICE=1` and `DeviceAllow=/dev/ttyUSB0 rw`. Change device path if necessary.

### [Legacy] Bridge daemon and libfprint-tod driver (daemon-based)

*This section is kept for historical reference.*

> Outdated note: I've tried to adopt an approach that doesn't require a daemon or Unix socket. Unfortunately, since my Arduino Nano board **resets on every serial connection and disconnection** ([Arduino docs](https://support.arduino.cc/hc/en-us/articles/4839084114460-If-your-board-runs-the-sketch-twice)), every operation adds a significant (~3-second) delay for waiting it to finish booting which is impractical for daily usage. The daemon is a software fix which keeps the serial connection open.

Build and install the daemon and driver:

```bash
meson setup -Denable_daemon=true build-legacy
ninja -C build-legacy
sudo ninja -C build-legacy install
sudo systemctl enable --now r503-arduinod.service
sudo systemctl restart fprintd
```

#### Configuration for daemon-based driver

- `R503_ARDUINOD_SERIAL`: path to Arduino serial port (defaults to `/dev/ttyUSB0`)
- `R503_ARDUINOD_SOCKET`: path to the daemon's Unix socket (defaults to `/run/r503-arduinod.sock`)
- A systemd drop-in is installed at `fprintd.service.d/r503-arduino-legacy.conf` which sets `R503_ARDUINO_DEVICE=1` and tells fprintd to use the virtual device.

## Usage

### Arduino serial commands

| Command       | Output                                                                                                           | Description                                       |
| ------------- | ---------------------------------------------------------------------------------------------------------------- | ------------------------------------------------- |
| `PING`        | `OK`                                                                                                             | Health check                                      |
| `INFO`        | `OK:CAPACITY,<n>,USED,<n>,SEC_LEVEL,<n>,BAUD_RATE,<n>`                                                           | Query sensor parameters                           |
| `ENROLL`      | `OK:PLACE_FINGER,STEP,1,ID,<id>` → `OK:REMOVE_FINGER` → `OK:PLACE_FINGER,STEP,2,ID,<id>` → `OK:ENROLLED,ID,<id>` | Enroll fingerprint into first empty slot          |
| `VERIFY`      | `OK:PLACE_FINGER` → `OK:VERIFIED,ID,<id>,CONFIDENCE,<n>`                                                         | Verify fingerprint against stored templates       |
| `LIST`        | `OK:LIST,<id1>,<id2>,...`                                                                                        | List all occupied template IDs                    |
| `DELETE:<id>` | `OK:DELETED,ID,<id>`                                                                                             | Delete a specific template                        |
| `CLEAR`       | `OK:CLEARED`                                                                                                     | Delete all templates                              |
| `CANCEL`      | _(no output)_                                                                                                    | Cancel the current `ENROLL` or `VERIFY` operation |

### fprintd

```bash
fprintd-enroll -f right-index-finger             # ENROLL
fprintd-verify                                   # VERIFY
fprintd-list $USER                               # LIST
fprintd-delete $USER -f right-index-finger       # DELETE a specific finger
fprintd-delete $USER                             # CLEAR all prints
```

## Troubleshooting

### Check USB detection

```bash
ls -la /dev/ttyUSB*
sudo dmesg | grep ttyUSB
```

### Check driver detection

```bash
ls -la $(pkg-config --variable=tod_driversdir libfprint-2-tod-1)/libfprint-2-tod1-r503-arduino.so
sudo G_MESSAGES_DEBUG=all /usr/lib/fprintd 2>&1 | grep -E "(r503_arduino|No driver found)"
```

## Uninstall

Daemon-less driver:

```bash
sudo rm $(pkg-config --variable=tod_driversdir libfprint-2-tod-1)/libfprint-2-tod1-r503-arduino.so
sudo rm $(pkg-config --variable=systemdsystemunitdir systemd)/fprintd.service.d/r503-arduino.conf
sudo systemctl daemon-reload
sudo systemctl restart fprintd
```

Legacy daemon-based driver:

```bash
sudo rm $(pkg-config --variable=tod_driversdir libfprint-2-tod-1)/libfprint-2-tod1-r503-arduino-legacy.so
sudo rm $(pkg-config --variable=systemdsystemunitdir systemd)/fprintd.service.d/r503-arduino-legacy.conf

sudo systemctl stop r503-arduinod
sudo rm $(pkg-config --variable=systemdsystemunitdir systemd)/r503-arduinod.service
sudo rm -f /usr/bin/r503-arduinod /usr/local/bin/r503-arduinod
sudo systemctl daemon-reload
sudo systemctl restart fprintd
```

## References

- Interfacing guide: https://how2electronics.com/interfacing-r502-r503-capacitive-fingerprint-sensor-with-arduino/
- R503 manual: https://en.hzgrow.com/Download.html
- libfprint-tod: https://gitlab.freedesktop.org/3v1n0/libfprint/-/blob/tod/README.tod.md
- fprint: https://fprint.freedesktop.org/

## License

MIT License - See repository root for full license text.
