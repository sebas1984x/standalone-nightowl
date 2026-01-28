# Pin Mapping – ERB RP2040

All switch inputs are **active-LOW** with internal pull-ups enabled.

## Lane 1
- IN switch: GPIO24
- OUT switch: GPIO25
- Stepper EN: GPIO8
- Stepper DIR: GPIO9
- Stepper STEP: GPIO10

## Lane 2
- IN switch: GPIO22
- OUT switch: GPIO12
- Stepper EN: GPIO14
- Stepper DIR: GPIO15
- Stepper STEP: GPIO16

## Buffer
- Buffer LOW: GPIO6
- Buffer HIGH: GPIO7

## Y-split
- Y-split switch: GPIO2

## Manual reverse buttons
- Lane 1 reverse: GPIO28
- Lane 2 reverse: GPIO29

## Potmeter (feedrate)
- ADC pin: GPIO26 (ADC0)
- Voltage range: 0–3.3V ONLY
