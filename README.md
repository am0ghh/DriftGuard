# DriftGuard — Wireless Assistive Mouse
A wireless assistive mouse built on an ESP32, using Bluetooth Low Energy to translate analog joystick input into precise cursor control — designed as an accessible alternative to a standard computer mouse.

## Project Overview
DriftGuard addresses the limitations of conventional input devices for users with motor impairments by combining a two-axis analog joystick with a multi-layer signal processing pipeline. The firmware runs on an ESP32 and transmits processed input over BLE to a Python client, which interprets gestures and drives the system cursor. Persistent calibration, dynamic sensitivity control, and assistive clicking modes make the device practical for real-world use.

## Features
* **Persistent Calibration** — Two-step calibration captures joystick center and range extremes, saved to NVS flash so the device is ready on every subsequent boot without recalibration.
* **Multi-Layer Signal Processing** — Exponential Moving Average filtering, configurable dead zones, and a quadratic response curve combine to eliminate drift and provide smooth, precise cursor movement.
* **Dynamic Sensitivity** — A potentiometer transmits sensitivity values over BLE in real time, allowing the user to adjust cursor speed on the fly without reflashing firmware.
* **Dwell Clicking** — Cursor stillness for a configurable duration triggers an automatic left click, eliminating the need for repeated button presses.
* **Scroll Mode** — Holding the joystick button while moving the stick redirects input to scroll the active window instead of moving the cursor.
* **Hold-for-Right-Click** — Holding the button still for a configurable duration arms a right click on release, keeping the primary left click instant and undelayed.
* **Audio Feedback** — An active buzzer with PWM-controlled volume provides confirmation beeps for clicks, connection events, and disconnect events.
* **LED State Machine** — The onboard LED communicates device state through distinct blink patterns: slow pulse (advertising), solid (connected), fast blink (scroll mode), double blink (dwell countdown), and rapid flicker (calibration).
* **Bidirectional BLE** — A notify characteristic streams joystick data to the client; a write characteristic receives event commands from the client to trigger buzzer and LED feedback.

## Components Required
* **Microcontroller**: ESP32 Development Board
* **Input**: Analog Two-Axis Joystick Module (KY-023 or equivalent)
* **Sensitivity Control**: Single-turn potentiometer
* **Audio**: Active piezo buzzer
* **Voltage Dividers**: 4× 10kΩ resistors (joystick signal lines), 3× 10kΩ resistors (potentiometer signal line)
* **Software (PC)**: Python 3, `bleak`, `pynput`

## Pin Connections
### Joystick
* **VRX (X-Axis)**: GPIO 13 — analog input via 10kΩ/20kΩ voltage divider (5V supply)
* **VRY (Y-Axis)**: GPIO 14 — analog input via 10kΩ/20kΩ voltage divider (5V supply)
* **SW (Button)**: GPIO 27 — digital input, internal pull-up enabled

### Sensitivity Potentiometer
* **Wiper**: GPIO 26 — analog input via 10kΩ/20kΩ voltage divider (5V supply)

### Feedback
* **Buzzer**: GPIO 33 — PWM output, LEDC channel 0, ~10% duty cycle for reduced volume
* **Status LED**: GPIO 2 — onboard LED, digital output

## How It Works
1. **Calibration**: On first boot (or button-held boot), the firmware samples the joystick center over 100 readings, then records min/max values during a 6-second sweep. Results are saved to NVS flash via the Preferences API.
2. **Signal Processing**: Each loop iteration reads raw ADC values and passes them through an Exponential Moving Average filter (α=0.2). Filtered values are mapped to a −100 to +100 scale and zeroed within configurable dead zone thresholds.
3. **BLE Transmission**: Processed values are packed as a comma-separated string (`x,y,btn,sensitivity`) and sent as BLE notifications at 100 Hz to the connected Python client.
4. **Gesture Interpretation (Python)**: The client applies a quadratic response curve and sub-pixel accumulation for smooth cursor motion. Button hold duration and joystick movement state determine whether input resolves as a left click, right click, or scroll mode.
5. **Feedback Loop**: The Python client writes single-byte command codes back to the ESP32 write characteristic to trigger buzzer beeps and LED state transitions corresponding to click confirmations and mode changes.

## Installation
### Firmware
1. Open the project in **PlatformIO** (VS Code extension).
2. Connect the ESP32 via USB and run **Upload** from the PlatformIO toolbar.
3. On first boot, follow the calibration prompts in the serial monitor (115200 baud).

### Python Client
1. Install dependencies:
   ```
   pip install bleak pynput
   ```
2. Power the ESP32 and run:
   ```
   python python/mouse_control.py
   ```
3. The client will scan for and connect to the `DriftGuard` BLE device automatically.

## Usage
* **Move cursor**: Tilt the joystick in any direction
* **Left click**: Tap the joystick button
* **Right click**: Hold the joystick button still for 0.6 seconds, then release
* **Scroll**: Hold the joystick button and move the stick
* **Dwell click**: Hold the cursor still for 1.5 seconds
* **Adjust speed**: Turn the potentiometer
* **Recalibrate**: Hold the joystick button while pressing the EN (reset) button on the ESP32

## Future Improvements
* **User Profiles**: Store multiple sensitivity and dead zone presets in NVS, switchable at runtime for different users or use cases.
* **Idle Power Management**: Reduce BLE polling rate and dim the LED after a configurable period of inactivity.
* **Enclosure**: 3D printed housing to package the device for practical daily use.
* **Serial Configuration**: Runtime tuning of dead zones, dwell time, and hold thresholds via serial commands without reflashing.

## Schematic
[DriftGuard.pdf](DriftGuard.pdf)

## License
MIT License
