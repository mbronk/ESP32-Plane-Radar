# Plane Radar + Weather

Firmware for an ESP32-C3 Super Mini with a 1.28-inch round GC9A01 display (240x240).

This fork is now maintained as its own project by mbronk. It started from earlier work and has diverged substantially in UI behavior, weather integration, and diagnostics.

## What it does

1. Wi-Fi setup portal with saved settings
2. Live ADS-B radar around your configured location
3. Weather screen with 4 forecast slots, condition icon, and detailed footer stats
4. Stats/debug screen (SSID, signal, IP, location/elevation, uptime, memory, aircraft count)

## Controls (BOOT button, GPIO 9, active LOW)

| Action | Effect |
|---|---|
| Single tap on Weather | Advance forecast slot |
| Single tap on Radar | Change radar range preset |
| Single tap on Stats | Exit Stats immediately |
| Double tap | Toggle Weather <-> Radar |
| Triple tap | Enter Stats screen |
| Hold ~0.9s to <10s | Toggle weather auto-cycle ON/OFF |
| Hold 10s | Reset Wi-Fi credentials/location/units and reboot to setup |

Notes:
- Stats is intentionally not part of double-tap cycling.
- Any tap while Stats is visible exits Stats immediately.

## Wi-Fi setup portal

AP name: PlaneRadar-Setup

Open:
- http://plane-radar.local
- or http://192.168.4.1

Config fields saved in NVS:
- Latitude / Longitude
- Display distances in miles
- Show airport runways

## Radar behavior

- Concentric radar rings and cardinal labels
- Aircraft symbols/tags for in-range targets
- Rim dots for out-of-ring targets at correct bearing
- Range presets persist across reboot

## Weather behavior

- Primary source: Open-Meteo
- Fallback source: US National Weather Service (NWS)
- Air quality: Open-Meteo air-quality endpoint
- Weather fetch interval: 10 minutes (configurable)
- Retry interval on failure: 5 seconds (configurable)

## Hardware mapping

Display and BOOT pin mapping live in include/config.h.
Current defaults target ESP32-C3 Super Mini + GC9A01 SPI round display.

## Build

PlatformIO environment: supermini

- Build: pio run
- Upload: pio run -t upload
- Monitor: pio device monitor

Merged web-flash image:
- pio run -t merge -e supermini
- Output: .pio/build/supermini/firmware-merged.bin

## Project structure

- src/main.cpp: app loop, page state, tap/hold behavior
- src/services/: Wi-Fi, ADS-B, weather, location services
- src/ui/: radar, weather, stats, and status screens
- include/: config, service/ui interfaces, display config
- scripts/: data generation and firmware merge helpers

## Credits and attribution

This repository is an independently maintained fork.

Original and upstream inspirations:
- MatixYo: ESP32-Plane-Radar (MIT)
- TurboTime29: ESP32-S3/touch/weather fork work (MIT)

Current fork maintenance and new changes:
- mbronk and contributors

Project repository:
- https://github.com/mbronk/ESP32-Plane-Radar

## License

MIT. See LICENSE.
