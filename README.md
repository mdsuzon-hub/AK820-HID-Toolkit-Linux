# AK820-HID-Toolkit-Linux

System-tray app (Qt6, Linux) that turns the AK820 keyboard knob into a dimmer for an ESP-based warm/white LED lamp.

## Controls

| Input | Action |
|---|---|
| `Ctrl` + knob | Warm brightness (`Ctrl+Shift` → White) |
| `Ctrl` + press | Swap Warm/White mapping |
| `Ctrl` + long press | Power on/off |
| `Ctrl` + double press | "Both" mode for 30 s |

Knob without `Ctrl` stays a normal volume knob.

## Install

```bash
./install.sh
```

Debian/Ubuntu, Wayland or Xorg. Installs dependencies, builds, sets up udev rules and `uinput`, and installs the binary to `/usr/local/bin`. Then run `ak820-controller`; it autostarts on login.

Remove with `./install.sh --uninstall`.

## Configuration

Edit constants at the top of `main.cpp` and re-run `./install.sh`:

- `ESP_IP`: lamp address (default `192.168.0.212`)
- `STEP_*`, `*_MS`: knob step sizes and timings

The ESP must expose `GET /getPWM?led=N` and `GET /setPWM?led=N&value=0-100` (warm = 3, white = 4).

## Notes

- Volume suppression while `Ctrl` is held uses an evdev grab with a uinput clone (Wayland) or `XGrabKey` (Xorg).
- GNOME needs the AppIndicator extension for the tray icon (installed by the script; re-login to activate).
