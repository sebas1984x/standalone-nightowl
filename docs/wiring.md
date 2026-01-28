# Wiring Guide – ERB RP2040

## Switches and Buttons
All switches and buttons are wired the same way:

- Common + NO -> GND
- Signal -> GPIO pin
- Active when pulled LOW
- Internal pull-ups enabled in firmware

No external resistors required.

## Potmeter (Feedrate Control)
Use a standard 3-pin potmeter (10k recommended).

- One outer pin -> 3.3V
- Other outer pin -> GND
- Middle pin (wiper) -> GPIO26 (ADC0)

⚠️ Never connect 5V to the ADC pin.

## Stepper Motors
Connect motors to the TMC2209 drivers as per the ERB board documentation.
Ensure:
- Correct motor coil pairing
- Shared ground between RP2040 and drivers
