# Troubleshooting – ERB Standalone NightOwl

## Flashing does nothing
- Ensure the ERB is **really** in BOOTSEL mode
- Check `lsusb` for `2e8a:0003 Raspberry Pi RP2 Boot`
- 24V power must be connected during flashing

## Build fails
- Verify `PICO_SDK_PATH` is set
- Ensure pico-sdk submodules are initialized
- Clean build: remove `build/` and rebuild

## Potmeter has no effect
- Check wiring (wiper must go to GPIO26)
- Ensure ADC is powered from 3.3V
- Verify `ADC raw` changes in debug output

## Motors noisy or hot at low speed
- Avoid very low feed rates
- Increase minimum feed speed if needed
- Motor current may be set too high on the driver

## Nothing feeds
- Check buffer LOW/HIGH switch states
- Verify OUT switch is active on the selected lane
- Check debug output over USB
