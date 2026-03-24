# ESP32-CANBoard
* ESP32-S3 Dual Core SoC
* MCP2562T CAN Transceiver (up to 1Mbps)
* 16x 5V-tolerant Inputs via two ADS7830 I2C expanders (16 analog channels)
* 2x 5V Outputs - Fused at 500mA (Thermal Reset)
* USB-C for programming, with JTAG support for debugging
* ESD Protection on both USB and CAN
* JAE Automotive Connector (PCB Socket: MX23A18NF1, Cable Plug: MX23A18SF1)
* Optional pull-up resistors via fused 5V rail for each input (TH 6.3mm)
* Optional 120ohm CAN terminating resistor
* Configuration via web interface over WiFi
* Optional per-channel median filtering with selectable strength (none/low/med/high) to reduce noise
* Small PCB Footprint - 40mm x 60mm

## Device Configuration

On each boot the board enables a WiFi access point and web configuration interface; this will automatically disable after 120 seconds if no client connects.

| SSID | WPA2 Key | Web UI |
|:---|:---|:---|
| ESP32-CanBoard | canboard123 | http://192.168.4.1 |

The web UI allows you to:

- View and edit per-channel settings (name, sensor type, pull-up, **filter level** dropdown, pressure calibration).
- Configure required CAN parameters - Base ID and bus speed.
- View current input voltages and calculated values in real time.
- Backup the entire configuration to a JSON file.
- Restore configuration from a previously exported JSON file (import now requires a top-level `version` matching the firmware `CONFIG_VERSION`).

![esp32-canboard-configuration](docs/esp32-canboard-configuration.png)

| Function | Description |
|:----|:----|
| Save Config | Save current UI settings to device storage (`/spiffs/config.bin`). Changes are validated, persisted and applied immediately. |
| Backup | Download a JSON snapshot of the current configuration. The filename is prefixed with `esp32-canboard-config-` and suffixed with the client timestamp in `ddmmyy-hhmmss` format. |
| Restore | Select a previously exported JSON file. The UI will upload the JSON to the device and validate the payload. The existing configuration is backed up on the device before overwrite; if saving the imported file fails, the device will restore the previous configuration. |
| Reboot Device | Reboots the device. |

**Notes:**
- Configuration is persisted on SPIFFS at `/spiffs/config.bin` (binary) and the web UI uses JSON export/import for human-readable backups.
- The firmware measures the 5V rail (V5) at runtime and no longer stores a pull‑up reference value; the UI no longer exposes a pull‑up vRef control.


## CAN Output

The device transmits input data as a set of eight CAN frames starting at the configured base ID. All frames use DLC=8 and little-endian byte ordering.

Overview:
- Frames 0..3 (Base ID .. Base ID+3): packed analog voltages for all 16 channels, 4 channels per frame, each channel as uint16 (millivolts, LSB then MSB).
- Frames 4..7 (Base ID+4 .. Base ID+7): packed dynamic values for all 16 channels, 4 values per frame, each as uint16. Dynamic values are sensor-type dependent (zeros where not applicable).

Detailed layout:

| CAN ID | Contents |
|:---|:---|
| Base ID + 0 | Channels 0..3 → ch0 (bytes 0..1 LSB/MSB), ch1 (2..3), ch2 (4..5), ch3 (6..7) — analog mV uint16 |
| Base ID + 1 | Channels 4..7 — analog mV uint16 |
| Base ID + 2 | Channels 8..11 — analog mV uint16 |
| Base ID + 3 | Channels 12..15 — analog mV uint16 |
| Base ID + 4 | Dynamic 0..3 — per-channel dynamic outputs (uint16) |
| Base ID + 5 | Dynamic 4..7 — per-channel dynamic outputs (uint16) |
| Base ID + 6 | Dynamic 8..11 — per-channel dynamic outputs (uint16) |
| Base ID + 7 | Dynamic 12..15 — per-channel dynamic outputs (uint16) |

Encoding rules for dynamic values (one per input):
| Type | Encoding |
|:---|:---|
| Raw | uint16 = 0 (no dynamic output; use analog voltage frame) |
| Pressure | uint16 = pressure_kPa * 100 (resolution 0.01 kPa) |
| NTC | signed int16 = temperature_C (°C as integer) stored in uint16 (least-significant 16 bits) |

Receivers should interpret all measurement values as little-endian uint16s unless noted. Dynamic outputs use live V5 for conversions where applicable.

Example DBC for signal names and scaling: [dbc/esp32-canboard.dbc](dbc/esp32-canboard.dbc)

## Schematic
[View PDF](docs/esp32-canboard-schematic.pdf)

## Images
![esp32-canboard-iso](docs/esp32-canboard-iso.png)

![esp32-canboard-top](docs/esp32-canboard-top.png)

![esp32-canboard-bottom](docs/esp32-canboard-bottom.png)

## Hardware / Wiring Notes (important changes)

- Analog inputs: expanded to 16 channels using two ADS7830 I2C analog expanders (8 channels each). I2C pins: SDA = GPIO12, SCL = GPIO13.
- Internal ADCs are used to monitor rails: V5 rail (divider connected to GPIO3), USB voltage monitor on GPIO38, and external voltage monitor on GPIO9.
- CAN1 pins remapped: RX = GPIO10, TX = GPIO11.

Pin names and exact connector mapping remain as in the schematic; see [Schematic PDF](docs/esp32-canboard-schematic.pdf) for physical connector assignments.

## Web API changes

- New endpoint: `GET /api/inputs` — returns JSON: `{ "v5_rail_mv": <uint16>, "channels": [ {"index":<n>, "mv": <uint16|null>}, ... ] }` where `mv` is the measured channel voltage in millivolts or `null` if not present.
- The web UI was updated to present 16 channels and no longer shows a stored pull-up vRef control; V5 is measured live and used for NTC conversions.

## Configuration / Import-Export

- The configuration structure version has been bumped; the firmware expects `CONFIG_VERSION = 4`.
- Exported JSON now contains a top-level `"version"` field. Import is strict: the device will reject imported JSON unless the top-level `version` matches the firmware `CONFIG_VERSION` (fresh-only imports).

## Build & Test

Build locally using your ESP‑IDF environment as before:

```bash
idf.py build
```

Run hardware validation for I2C ADS7830 timing and CAN traffic on a bus monitor after flashing.