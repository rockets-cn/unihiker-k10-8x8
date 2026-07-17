#include <Arduino.h>
#include "DFRobot_MatrixLidar.h"
#include "unihiker_k10.h"
#include <WebServer.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "img_converters.h"

// 保留 Arduino IDE 与 PlatformIO 共用同一份固件源码。
#include "../tof-camera-overlay/tof-camera-overlay.ino"
