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
- Potmeter connected to **3.3V** ADC (never 5V)

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

### 3. Build the firmware
```bash
mkdir build
cd build
cmake ..
make -j4
```

The generated `.uf2` file will be in the `build/` directory.

---

## 🔥 Flashing

1. Hold **BOOTSEL** on the RP2040 board  
2. Plug in USB  
3. Copy the `.uf2` file to the mounted drive  

---

## 🖥️ Debug (USB CDC)

```bash
screen /dev/ttyACM0 115200
```

Exit screen:
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
