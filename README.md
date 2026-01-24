# Auriscribe

Lightweight, offline speech-to-text for Linux desktops (tray app). Built on whisper.cpp.

## Quick Start

```bash
# Install dependencies
make deps

# Setup whisper.cpp (auto-enables Vulkan if available)
./scripts/setup-whisper.sh

# Build
make

# Run
./auriscribe
```

## Arch Linux (AUR)

- Package: `https://aur.archlinux.org/packages/auriscribe`

## Download a Model

Use the tray menu: **Download Models...** → choose a model → **Download**.
Models are saved under `~/.local/share/auriscribe/models/` and the downloaded model is auto-selected.

## Usage

- Use the tray menu **Start/Stop Recording**.
- Configure a global hotkey in **Settings...** (some desktop environments reserve `Super` shortcuts).
- On stop, Auriscribe transcribes and inserts the text into the window that was active when recording started (X11).

Autostart: enable **Start Auriscribe on login** in Settings.

## Dependencies

- GTK3
- libayatana-appindicator3
- PulseAudio
- json-c
- libcurl
- X11 (for global hotkeys on X11)
- xdotool or wtype (for text input)
- Optional (faster Whisper): Vulkan dev/runtime (e.g. `libvulkan-dev`)

## Performance knobs

- `XFCE_WHISPER_VULKAN=0` disables Vulkan build in `scripts/setup-whisper.sh`
- `XFCE_WHISPER_NO_GPU=1` forces CPU at runtime
- `XFCE_WHISPER_GPU_DEVICE=0` selects GPU device index
- `XFCE_WHISPER_THREADS=8` sets Whisper CPU thread count

## License

MIT
