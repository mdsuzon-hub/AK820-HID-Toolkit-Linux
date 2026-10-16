# AK820-HID-Toolkit-Linux

<p align="center">
  <img src="https://github.com/mdsuzon-hub/AK820-HID-Toolkit-Windows/blob/main/img/banner.png?raw=true" alt="AK820 HID Toolkit Banner" width="100%">
</p>

System-tray application (Qt6) that turns the **AJAZZ AK820** multimedia knob into a programmable dimmer for an ESP-based warm/white LED lamp — while leaving normal volume control completely untouched.

> **Windows version** → [AK820-HID-Toolkit-Windows](https://github.com/mdsuzon-hub/AK820-HID-Toolkit-Windows)

**Linux · C++17 · Qt6 · System Tray**

The HID event handling is fully generic and can be adapted for media control, OBS, macros, home automation, robotics, IoT devices, or any other custom application.

![Linux](https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black)
![Windows](https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows)
![Language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Status](https://img.shields.io/badge/status-active-brightgreen)

---

## Controls

| Gesture | Action |
|---------|--------|
| **Ctrl** + rotate | Adjust Warm brightness |
| **Ctrl+Shift** + rotate | Adjust White brightness |
| **Ctrl** + short press | Swap Warm ↔ White mapping |
| **Ctrl** + long press | Power on/off |
| **Ctrl** + double press | “Both” mode for 30 seconds |

Without **Ctrl**, the knob continues to function as a normal system volume control.

---

## Install

```bash
./install.sh
```

Supported on Debian/Ubuntu (Xorg and Wayland). The script:

- Installs all required dependencies  
- Builds the application  
- Configures udev rules and uinput  
- Installs the binary to `/usr/local/bin`  
- Enables autostart on login  

Run with:

```bash
ak820-controller
```

Uninstall:

```bash
./install.sh --uninstall
```

---

## Configuration

Edit the constants at the top of `main.cpp`, then re-run `./install.sh`:

| Setting | Default | Description |
|---------|---------|-------------|
| `ESP_IP` | `192.168.0.212` | IP address of the ESP LED controller |
| `STEP_*` | — | Brightness step sizes |
| `*_MS` | — | Timing thresholds for gestures |

The ESP must expose:

- `GET /getPWM?led={3\|4}` → current PWM value  
- `GET /setPWM?led={3\|4}&value={0-100}` → set brightness percentage  

(Warm = LED 3, White = LED 4)

---

## Notes

- Volume-key suppression while **Ctrl** is held uses an evdev grab + uinput clone on Wayland, or `XGrabKey` on Xorg.
- On GNOME, the AppIndicator extension is required for the tray icon. The install script sets it up; a re-login may be needed to activate it.

---

## License

MIT — free to use, modify, and redistribute.
