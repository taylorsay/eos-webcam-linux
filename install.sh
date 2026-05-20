#!/usr/bin/env bash
set -euo pipefail

# eos-webcam installer
# Streams Canon EOS live view to /dev/video10 as a system webcam.
# Supports: Arch Linux, Ubuntu/Debian, Fedora/RHEL

BINARY_NAME="eos-webcam"
INSTALL_DIR="$HOME/.local/bin"
SERVICE_NAME="eos-webcam.service"
SERVICE_DIR="$HOME/.config/systemd/user"
VIDEO_DEVICE="/dev/video10"
VIDEO_NR="10"
CARD_LABEL="Canon EOS"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${GREEN}==>${NC} $*"; }
warn()    { echo -e "${YELLOW}warn:${NC} $*"; }
error()   { echo -e "${RED}error:${NC} $*" >&2; exit 1; }
need_sudo() { echo -e "${YELLOW}  (requires sudo)${NC}"; }

# ── Distro detection ──────────────────────────────────────────────────────────

detect_distro() {
    if command -v pacman &>/dev/null; then
        echo "arch"
    elif command -v apt-get &>/dev/null; then
        echo "debian"
    elif command -v dnf &>/dev/null; then
        echo "fedora"
    else
        echo "unknown"
    fi
}

install_deps() {
    local distro="$1"
    info "Installing build dependencies..."
    case "$distro" in
        arch)
            sudo pacman -S --needed --noconfirm \
                libgphoto2 libjpeg-turbo v4l2loopback-dkms \
                meson ninja pkgconf base-devel
            ;;
        debian)
            sudo apt-get update -qq
            sudo apt-get install -y \
                libgphoto2-dev libturbojpeg-dev v4l2loopback-dkms \
                meson ninja-build pkg-config build-essential
            ;;
        fedora)
            # Core build deps must succeed (set -e will catch failures)
            sudo dnf install -y \
                libgphoto2-devel turbojpeg-devel \
                meson ninja-build pkgconf-pkg-config gcc-c++
            # v4l2loopback may live in different package names (RPM Fusion vs base)
            if   sudo dnf install -y akmod-v4l2loopback 2>/dev/null; then :
            elif sudo dnf install -y kmod-v4l2loopback  2>/dev/null; then :
            elif sudo dnf install -y v4l2loopback       2>/dev/null; then :
            else
                warn "v4l2loopback not found in repos."
                warn "Enable RPM Fusion: https://rpmfusion.org/Configuration"
                warn "Then run: sudo dnf install akmod-v4l2loopback"
            fi
            ;;
        *)
            warn "Unrecognised distro. Install manually:"
            echo "  libgphoto2 (dev), libturbojpeg (dev), v4l2loopback-dkms, meson, ninja, pkg-config, C++ compiler"
            read -rp "Continue anyway? [y/N] " ans
            [[ "$ans" =~ ^[Yy]$ ]] || exit 1
            ;;
    esac
}

# ── Build ─────────────────────────────────────────────────────────────────────

build() {
    info "Building $BINARY_NAME..."
    local build_dir
    build_dir="$(mktemp -d)"
    trap "rm -rf '$build_dir'" EXIT

    meson setup "$build_dir" . --buildtype=release -Dprefix="$HOME/.local" \
        2>&1 | grep -v '^$'
    meson compile -C "$build_dir"

    mkdir -p "$INSTALL_DIR"
    cp "$build_dir/src/$BINARY_NAME" "$INSTALL_DIR/$BINARY_NAME"
    chmod +x "$INSTALL_DIR/$BINARY_NAME"
    info "Installed binary: $INSTALL_DIR/$BINARY_NAME"
}

# ── v4l2loopback ─────────────────────────────────────────────────────────────

setup_v4l2loopback() {
    info "Configuring v4l2loopback kernel module..."
    need_sudo

    local modprobe_conf="/etc/modprobe.d/eos-webcam.conf"
    local modules_conf="/etc/modules-load.d/eos-webcam.conf"

    sudo tee "$modprobe_conf" > /dev/null <<EOF
# eos-webcam: Canon EOS live view loopback device
options v4l2loopback devices=1 video_nr=$VIDEO_NR card_label="$CARD_LABEL" exclusive_caps=1
EOF

    sudo tee "$modules_conf" > /dev/null <<EOF
v4l2loopback
EOF

    # Load the module now (without reboot)
    if lsmod | grep -q v4l2loopback; then
        warn "v4l2loopback already loaded — reload to apply new options:"
        echo "  sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback"
    else
        sudo modprobe v4l2loopback \
            devices=1 "video_nr=$VIDEO_NR" "card_label=$CARD_LABEL" exclusive_caps=1 \
            || warn "modprobe failed — try rebooting, or load manually."
    fi

    info "v4l2loopback configured (device: $VIDEO_DEVICE)"
}

# ── udev rule ─────────────────────────────────────────────────────────────────

setup_udev() {
    info "Installing udev rule for Canon cameras..."
    need_sudo

    sudo tee /etc/udev/rules.d/70-eos-webcam.conf > /dev/null <<'EOF'
# eos-webcam: grant access to Canon EOS cameras for the plugdev group
SUBSYSTEM=="usb", ATTR{idVendor}=="04a9", MODE="0660", GROUP="plugdev"
EOF

    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=usb

    # Add current user to plugdev if not already a member
    if ! groups | grep -qw plugdev; then
        sudo usermod -aG plugdev "$USER"
        warn "Added $USER to the 'plugdev' group."
        warn "Log out and back in (or run: newgrp plugdev) for group membership to take effect."
    fi
}

# ── systemd user service ──────────────────────────────────────────────────────

setup_service() {
    info "Installing systemd user service..."
    mkdir -p "$SERVICE_DIR"
    cp "system/$SERVICE_NAME" "$SERVICE_DIR/$SERVICE_NAME"

    systemctl --user daemon-reload
    systemctl --user enable "$SERVICE_NAME"
    systemctl --user start  "$SERVICE_NAME" || true

    info "Service enabled and started."
    echo
    echo "  Status: systemctl --user status $SERVICE_NAME"
    echo "  Logs:   journalctl --user -u $SERVICE_NAME -f"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    echo
    echo "  eos-webcam installer"
    echo "  Canon EOS → /dev/video10 webcam bridge"
    echo

    # Verify we're in the repo root
    [[ -f "src/main.cpp" ]] || error "Run this script from the eos-webcam repo root."

    local distro
    distro=$(detect_distro)
    info "Detected distro: $distro"

    install_deps "$distro"
    build
    setup_v4l2loopback
    setup_udev
    setup_service

    echo
    info "Done! Plug in your Canon EOS camera and check:"
    echo "  ffplay -f v4l2 $VIDEO_DEVICE"
}

main "$@"
