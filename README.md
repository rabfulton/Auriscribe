# Auriscribe

Lightweight speech-to-text for Linux desktops.

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

## Download a Model

Use the tray menu: **Download Models...** → pick a model → **Download**.
The app saves it under `~/.local/share/auriscribe/models/` and auto-selects it.

## Usage

- Press **Super+Space** to toggle recording
- Speak, then press again to transcribe
- Text is typed into the active window

For Wayland, configure your compositor to send SIGUSR2:
```bash
# Sway example
bindsym $mod+space exec pkill -USR2 auriscribe
```

## Model hosting

Model downloads are sourced from Hugging Face by default.
Override the repo with `XFCE_WHISPER_HF_REPO`.

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
