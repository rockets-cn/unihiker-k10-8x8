# unihiker-k10-8x8

[中文](README.md) | **English**

Display live 8×8 depth data from the DFRobot MatrixLidar TOF sensor as a color grid over the camera image on a UNIHIKER K10.

## Features

- Live camera image as the background
- 8×8 depth grid overlay with distance-based colors:
  - Red = near
  - Green = medium distance
  - Blue = far
  - Dark gray = invalid data (0 or greater than 3500 mm)
- Status bar showing calibration offsets and refresh rates
- WiFi web interface for viewing live depth data, saving frames, and exporting CSV files

### Live Capture

![UNIHIKER K10 overlay-v7 camera and TOF 8x8 depth grid](docs/images/tof-camera-overlay-live-v7.png)

Actual `overlay-v7` output compiled and uploaded with PlatformIO. The browser combines the camera image and 8×8 TOF data, and reports the local display, browser camera, and TOF sampling frame rates.

## Hardware

- UNIHIKER K10
- DFRobot MatrixLidar TOF sensor (I2C address `0x31`, 400 kHz)

## Dependencies

- `unihiker_k10`
- `DFRobot_MatrixLidar`
- `WiFi` and `WebServer` (included with the K10 board package)

## Usage

### PlatformIO (Recommended)

The repository includes `platformio.ini` and uses DFRobot's official `unihiker_k10` board platform. PlatformIO caches the toolchain, board framework, and libraries, so later builds use incremental compilation after the first full build.

Copy the local WiFi configuration template before the first build, then enter your network credentials. `wifi_secrets.h` is ignored by Git, so the password will not be committed to GitHub.

```bash
cp include/wifi_secrets.h.example include/wifi_secrets.h
```

```bash
# Build
pio run

# Build and upload to the K10
pio run --target upload

# Open the serial monitor
pio device monitor
```

If the `pio` command is unavailable, install [PlatformIO Core](https://platformio.org/install/cli) or open the repository with PlatformIO IDE for VS Code. The PlatformIO entry point only includes the Arduino sketch, so Arduino IDE and PlatformIO compile the same firmware source.

### Arduino IDE

Open `tof-camera-overlay/tof-camera-overlay.ino`, select the UNIHIKER K10 board, then compile and upload the sketch.

### WiFi Web Interface

1. PlatformIO reads WiFi credentials from `include/wifi_secrets.h`. Arduino IDE users can edit the default values near the top of the sketch. Without an explicit configuration, the firmware attempts to reuse network credentials stored in the ESP32 NVS.
2. After the K10 connects, its IP address appears on the second line of the display and in the serial monitor.
3. Open `http://<K10-IP>` in a browser to view the live camera and 8×8 depth overlay. Each camera frame is requested as soon as the previous frame finishes. Actual frame rate depends on WiFi throughput and JPEG encoding speed. `LCD` is the local display rate, `CAM` is the camera frame rate received by the browser, and `TOF` is the sensor sampling rate.
4. Available actions:
   - **Save Current Frame**: stores the current 8×8 data and a camera image in RAM (a 50-frame ring buffer; cleared on power loss; images are stored in PSRAM)
   - **Download CSV**: exports all saved depth frames as a CSV file
   - **Clear Records**: removes all saved frames and images
   - Click a saved thumbnail to open the corresponding full image
5. WiFi connection and reconnection are non-blocking. The local camera and TOF grid start immediately even when WiFi is unavailable. The firmware retries every 10 seconds while offline.
6. The page status bar should show `overlay-v7`. Browser caching is disabled; if a different version appears, reload the page after confirming that the latest firmware is running.

### Calibration

Use the K10 buttons to align the grid with the camera image:

| Button | Short press | Long press (>500 ms) |
|--------|-------------|----------------------|
| A | X offset +2 | X offset -2 |
| B | Y offset +2 | Y offset -2 |

The default offset is `X:0, Y:40`, and each grid cell is 28×28 pixels. Both the camera and depth data use the sensors' native orientation, rotated 180° from the earlier display implementation.

## Implementation Notes

- The local grid targets a 50 ms update interval (`DRAW_INTERVAL`); actual rate depends on TOF sampling and display transfer time
- Only grid cells whose colors changed are redrawn
- Synchronous TOF I2C reads run in a separate FreeRTOS task so they do not block web requests, camera capture, or button input
- Camera frames are downsampled from 320×240 to 160×120 before JPEG conversion, reducing software encoding work to one quarter of the original pixel count
- WebServer runs in a separate FreeRTOS task, so camera requests do not wait for local display refreshes
- WiFi connection and reconnection are fully non-blocking and cannot delay the first local frame
