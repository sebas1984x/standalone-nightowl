# Standalone NightOwl RP2040 Filament Switcher Firmware

Standalone firmware for a **NightOwl / ERB RP2040** filament switcher.  
This runs fully standalone without Klipper integration and drives two filament lanes with auto-feed, auto-swap and manual reverse support.

## 🚀 Features

- Dual lane filament feed with TMC2209 stepper drivers  
- Active-LOW switches with internal pull-ups  
- Autoload when filament is inserted  
- Buffer-based feed rate controlled via potentiometer  
- Auto swap when the active lane runs out  
- Manual reverse per lane  
- Status LED feedback  
- USB serial debug at 115200

## 🧰 Requirements

- Raspberry Pi Pico / RP2040 variant (ERB / NightOwl board)  
- pico-sdk installed  
- USB cable for flashing and debug

## 📍 Pin Mapping

See [docs/pinout.md](docs/pinout.md)

## 🛠️ Wiring

See [docs/wiring.md](docs/wiring.md)

## 📦 Build

### 1. Clone pico-sdk
```bash
cd ~
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init
