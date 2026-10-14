# Meade LX200 Classic Smart Front Panel

An ESP32-S3-based front-panel retrofit concept for a Meade LX200 Classic telescope-control system. The design adds USB-C, Wi-Fi, and Bluetooth connectivity while preserving a hardware passthrough mode and providing a keypad-translation fallback mode.


## Overview

The project replaces the original passive front-panel function with a switchable hardware-and-firmware interface. Two physical switches select the input source and determine which control path is active.

- **Input sources:** RS-232, USB-C serial, or Wi-Fi/Bluetooth.
- **Mode A — passthrough:** The selected wired source is routed through the hardware switching network. When wireless is selected, the ESP32-S3 supplies the serial data. The original hand controller retains its direct hardware path.
- **Mode B — keypad translation:** The ESP32-S3 interprets external commands and translates them into keypad actions for the telescope controller. Physical hand-controller activity takes priority over automated command injection.
- **Local status display:** An OLED is designed to show the selected source, operating mode, and power telemetry.

The goal is to add modern interfaces without making the original serial-control pathway dependent on the microcontroller.

## Hardware Design

The design centers on a removable ESP32-S3 development board and a custom PCB intended to match the original panel's mounting geometry. Major subsystems include:

- Hardware switching for input selection and Mode A / Mode B routing.
- USB-to-serial and RS-232 level conversion.
- Logic-level translation between ESP32 signals and the surrounding 5 V TTL logic.
- An 18 V to 5 V power-conversion stage with input protection.
- An OLED interface and external voltage/current monitoring.
- A hardware transmit-enable gate intended to keep the motherboard-facing keypad output disabled during startup.

### Schematic Overview

The following images show the three-page schematic overview.

![Schematic overview — page 1](assets/schematic-overview-page-1.jpg)

![Schematic overview — page 2](assets/schematic-overview-page-2.jpg)

![Schematic overview — page 3](assets/schematic-overview-page-3.jpg)

### PCB Renders

Two PCB render images are included for visual inspection of the board layout and component placement.

![PCB render 1](assets/pcb-render-1.jpg)

![PCB render 2](assets/pcb-render-2.jpg)

## Firmware Architecture

The firmware is designed around FreeRTOS and separates switch monitoring, serial routing, command translation, keypad-state tracking, power telemetry, OLED rendering, and transmit-gate control into distinct modules.

| Module | Responsibility |
|---|---|
| `main.c` | Startup initialization and task scheduling |
| `switch_monitor.c` | Switch debouncing and active-state tracking |
| `router_bridge.c` | UART routing, forwarding, and command arbitration |
| `transcode_engine.c` | Command representation and translation |
| `virtual_handset.c` | Tracking expected keypad/UI state during automated sequences |
| `oled_display.c` | Thread-safe OLED rendering interface |
| `telemetry_adc.c` | Power telemetry collection and display requests |
| `failsafe_gate.c` | Control of the motherboard-facing transmit-enable gate |

The three hardware UARTs are assigned to the hand controller, motherboard keypad link, and the selected external serial path. Wireless commands in Mode B are intended to enter the translation logic directly, with UART2 isolated from unintended loopback traffic in that state.

Serial forwarding is designed to take priority over display updates. Automated keypad actions should begin from a known UI baseline, and a physical key press should interrupt an in-progress automated sequence so the user can regain control.



The hardware and firmware specifications currently differ on the handset-idle arbitration timeout (500 ms in the product requirements and 200 ms in the firmware requirements). That value should be reconciled before documenting the timing as finalized.
