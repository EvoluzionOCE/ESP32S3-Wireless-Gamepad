# Wireless Fighting Game Gamepad

A ultra-low-latency wireless gamepad designed for fighting games, built on the
**nRF52840** using Nordic **Enhanced ShockBurst (ESB)** at 2 Mbps.

## Latency budget

| Stage | Time |
|---|---|
| GPIO interrupt → MCU read | ~0.1 ms |
| MCU processing + packet build | ~0.2 ms |
| ESB air time @ 2 Mbps (4-byte packet) | ~0.25 ms |
| Dongle receive + HID report | ~0.3 ms |
| USB HID poll @ 1000 Hz | ≤1 ms |
| **Total worst case** | **~2 ms** |

---

## Hardware

| Part | Role | Source |
|---|---|---|
| Seeed XIAO nRF52840 × 2 | Controller MCU + Dongle MCU | seeedstudio.com |
| Sanwa OBSF-30 × 8 | Action buttons (LP/MP/HP/LK/MK/HK/Start + 1) | focusattack.com |
| Sanwa OBSC-24 × 4 | Directional buttons (Up/Down/Left/Right) | focusattack.com |
| LiPo 3.7V 1000mAh | Controller battery | Adafruit #2011 |
| JST-PH 2-pin connector | Battery connector (matches XIAO) | Adafruit |

The dongle can also be a **Nordic nRF52840 Dongle (PCA10059)** — plug straight
into USB-A. Rename the overlay file to `nrf52840dongle_nrf52840.overlay`.

---

## Button layout (Controller pin map)

```
XIAO pin │ nRF52840 │ Function
─────────┼──────────┼──────────────────
D0       │ P0.02    │ D-Pad UP
D1       │ P0.03    │ D-Pad DOWN
D2       │ P0.28    │ D-Pad LEFT
D3       │ P0.29    │ D-Pad RIGHT
D4       │ P0.04    │ LP  (Light Punch)
D5       │ P0.05    │ MP  (Medium Punch)
D6       │ P1.11    │ HP  (Hard Punch)
D7       │ P1.12    │ LK  (Light Kick)
D8       │ P1.13    │ MK  (Medium Kick)
D9       │ P1.14    │ HK  (Hard Kick)
D10      │ P1.15    │ START
```

All buttons are **active-LOW** — one pin connects to the switch, the other to GND.
Internal pull-ups are enabled in firmware.

---

## SOCD Cleaning

Hold a button at power-on to select the SOCD resolution mode.
The LED flashes to confirm the mode (1 flash = Neutral, 2 = Last Win, 3 = 2nd Win).

| Held at boot | Mode | Behaviour |
|---|---|---|
| Nothing | **Neutral** | L+R = none, U+D = none (tournament default) |
| LP | **Last Win** | Most recently pressed direction takes priority |
| MP | **2nd Win** | Second pressed direction wins (Hitbox standard) |

---

## Toolchain setup

This project uses **nRF Connect SDK (NCS)** which bundles Zephyr RTOS.

### 1. Install nRF Connect for Desktop
Download from: https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop

Install the **Toolchain Manager** extension, then install **NCS v2.6.0** (or newer).

### 2. Install nRF Connect for VS Code (recommended)
Install the VS Code extension **nRF Connect for VS Code**.
It handles west, toolchain paths, and flashing automatically.

### 3. Build and flash — Controller

```bash
cd nRF52840_Controller
west build -b seeed_xiao_nrf52840
west flash
```

### 4. Build and flash — Dongle

```bash
cd nRF52840_Dongle
west build -b seeed_xiao_nrf52840
west flash
```

If using the Nordic PCA10059 dongle:
```bash
west build -b nrf52840dongle_nrf52840
# Flash via nRF Connect Programmer (drag-and-drop .hex)
```

---

## Project structure

```
nRF52840_Controller/
├── CMakeLists.txt
├── prj.conf                         # Kconfig: ESB, GPIO, logging
├── boards/
│   └── seeed_xiao_nrf52840.overlay  # Pin definitions
└── src/
    └── main.c                       # ESB TX, SOCD cleaning, input reading

nRF52840_Dongle/
├── CMakeLists.txt
├── prj.conf                         # Kconfig: ESB, USB HID @ 1000 Hz
├── boards/
│   └── seeed_xiao_nrf52840.overlay
└── src/
    └── main.c                       # ESB RX, USB HID gamepad report

ESP32S3_Sender    ← legacy ESP-NOW firmware (kept for reference)
ESP32S3_Receiver  ← legacy ESP-NOW firmware (kept for reference)
```

---

## Legacy ESP-NOW firmware

The original `ESP32S3_Sender` and `ESP32S3_Receiver` files (Arduino/ESP-NOW)
are kept for reference. They target Waveshare ESP32-S3-Pico boards and use
the Arduino IDE.
