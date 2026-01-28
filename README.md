# Standalone NightOwl / ERB RP2040 Filament Switcher

Standalone firmware for a **NightOwl / ERB RP2040** filament switcher.  
This firmware runs **fully standalone** and does **not require Klipper integration**.  
The RP2040 handles filament loading, buffer-based feeding, and automatic lane swapping by itself.

Built for people who prefer firmware that *does the job* instead of having opinions.

---

## ✨ Features

- **2 filament lanes** (TMC2209 via STEP/DIR/EN)
- **Active-LOW switches** (C/NO to GND, internal pull-ups)
- Per lane:
  - IN switch (filament present)
  - OUT switch (preloaded / ready)
- **Filament buffer** with LOW + HIGH switches
- **Y-split switch** for swap safety
- **Autoload**
  - Insert filament → motor runs until OUT switch
- **Buffer-driven feed**
  - Feeds only when buffer LOW persists for a delay
- **Auto-swap**
  - Swap armed when active lane runs out
  - Swap executed when buffer requests feed and other lane is ready
- **Manual reverse buttons** (one per lane)
- **Potmeter-controlled feed rate**
- **Status LED** with multiple states
- **USB CDC debug output** (115200 baud)

---

## 🔧 Hardware assumptions

- All switches and buttons wired:
  - **Common + NO → GND**
  - Active when pulled LOW
- Internal pull-ups enabled in firmware
- Potmeter connected to **3.3V** ADC (**never 5V**)

---

## 📍 Pin Mapping

See:  
👉 [Pin Mapping](docs/pinout.md)

---

## 🔌 Wiring

See:  
👉 [Wiring Guide](docs/wiring.md)

---

## 🛠️ Build Instructions

### 1. Clone the Pico SDK
```bash
cd ~
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init
```

### 2. Set the SDK path
```bash
export PICO_SDK_PATH=~/pico-sdk
```

(Optional, make permanent)
```bash
echo 'export PICO_SDK_PATH=~/pico-sdk' >> ~/.bashrc
source ~/.bashrc
```

### 3. Build the firmware (always clean)
```bash
cd ~/standalone-nightowl
rm -rf build
mkdir build
cd build
cmake ..
make -j4
```

After a successful build, this file must exist:
```
erb_standalone_mmu.elf
```

---

## 🔥 Flashing (FYSETC ERB – IMPORTANT)

⚠️ **The FYSETC ERB does NOT flash like a standard Raspberry Pi Pico.**  
Use this method or flashing will silently fail.

### Requirements
- Raspberry Pi (Pi 4 / Pi 5)
- `picotool` installed
- ERB connected via USB-C
- **24 V power connected to the ERB**

### Enter BOOTSEL / DFU mode
1. Power the ERB with **24 V**
2. Connect USB-C from ERB to the Pi
3. Hold **BOOTSEL**
4. Press **RST** for ~0.5 s
5. Release **RST**
6. Release **BOOTSEL after 2–3 seconds**

Verify:
```bash
lsusb
```

You must see:
```
2e8a:0003 Raspberry Pi RP2 Boot
```

### Flash
```bash
cd ~/standalone-nightowl/build
picotool load erb_standalone_mmu.elf -f
picotool reboot
```

➡️ The ERB will reboot and run the new firmware.

---

## 🖥️ Debug (USB CDC)

```bash
sudo screen /dev/ttyACM0 115200
```

Exit:
```
Ctrl-A → K → Y
```

---

## 🧪 Troubleshooting

See:  
👉 [Troubleshooting](docs/troubleshooting.md)

---

## 📄 License

MIT © sebas1984x

Use it, modify it, break it, improve it.  
Just don’t pretend you wrote it.
