# eos-webcam

Use a Canon EOS camera as a Linux webcam — in OBS, Discord, Zoom, or any V4L2-compatible app.

Works with any Canon EOS that supports USB live view via libgphoto2 (most DSLRs from the 500D/T1i onward).

## How it works

```
Canon EOS (USB/PTP)
      ↓
 libgphoto2       ← captures MJPEG live view frames
      ↓
 libturbojpeg     ← decodes MJPEG → YUYV
      ↓
 v4l2loopback     ← presents as /dev/video10
      ↓
 OBS / Discord / ffplay / etc.
```

The daemon starts automatically on login and restarts itself if the camera is unplugged and replugged.

## Requirements

- Linux (kernel 4.x+)
- A Canon EOS camera supported by libgphoto2 (see [supported cameras](http://www.gphoto.org/proj/libgphoto2/support.php))
- `v4l2loopback-dkms` kernel module
- Build tools: meson, ninja, pkg-config, C++17 compiler

## Installation

```bash
git clone https://github.com/taylorsay/eos-webcam.git
cd eos-webcam
bash install.sh
```

The installer:
1. Detects your distro (Arch, Ubuntu/Debian, Fedora) and installs build deps
2. Builds the `eos-webcam` binary and installs it to `~/.local/bin/`
3. Configures v4l2loopback to create `/dev/video10` at boot
4. Installs a udev rule so your user can access the camera without sudo
5. Installs and starts a systemd user service that auto-starts on login

## Uninstall

```bash
bash uninstall.sh
```

## Tested cameras

| Camera | USB PID | Notes |
|--------|---------|-------|
| EOS 1200D / Rebel T5 | `04a9:327f` | Confirmed working, 1056×704 @ ~9fps |

> Live view resolution and FPS vary by camera model — all are hardware limits.

## Usage after install

```bash
# Quick test
ffplay -f v4l2 /dev/video10

# Check service status / logs
systemctl --user status eos-webcam.service
journalctl --user -u eos-webcam.service -f
```

## Recovery (camera unplugged)

The service has `Restart=always` so it recovers automatically when the camera is replugged. If it gets stuck:

```bash
sudo modprobe -r v4l2loopback
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="Canon EOS" exclusive_caps=1
systemctl --user restart eos-webcam.service
```

## Known constraints

- Live view is ~9fps over USB/PTP on most Canon EOS bodies — this is a hardware limit
- Only one process can hold the camera at a time; the daemon holds it exclusively while running
- Battery drain is significant during live view; keep the camera plugged into AC if possible

## Building manually

```bash
meson setup build --buildtype=release
meson compile -C build
# Binary at: build/src/eos-webcam
```

Dependencies: `libgphoto2`, `libturbojpeg`, headers for both.
