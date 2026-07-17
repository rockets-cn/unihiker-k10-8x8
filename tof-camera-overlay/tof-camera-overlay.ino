#include "unihiker_k10.h"
#include "DFRobot_MatrixLidar.h"

UNIHIKER_K10 k10;
DFRobot_MatrixLidar_I2C tof(0x31);

uint16_t depthBuf[64];

// 校准参数
int offsetX = 0;
int offsetY = 40;
const int cellW = 28;
const int cellH = 28;

// 按钮状态
bool prevA = false;
bool prevB = false;
unsigned long pressAStart = 0;
unsigned long pressBStart = 0;
unsigned long lastAdjustA = 0;
unsigned long lastAdjustB = 0;

// 帧率控制
unsigned long lastDraw = 0;
const unsigned long DRAW_INTERVAL = 200;

// 计时
unsigned long tTof = 0;
unsigned long tDraw = 0;
int drawFps = 0;
unsigned long drawCount = 0;
unsigned long fpsTime = 0;

// 显示缓冲
uint32_t prevColor[64];
int prevOffsetX = 0;
int prevOffsetY = 40;
String prevText = "";

void rotate180(uint16_t *buf) {
  for (int i = 0; i < 32; i++) {
    uint16_t t = buf[i];
    buf[i] = buf[63 - i];
    buf[63 - i] = t;
  }
}

uint32_t distanceToColor(uint16_t mm) {
  uint32_t r = (uint32_t)mm * 200 / 3500;
  uint8_t red, grn, blu;
  if (r < 100) {
    red = 255;
    grn = r * 255 / 100;
    blu = 0;
  } else {
    red = (200 - r) * 255 / 100;
    grn = red;
    blu = (r - 100) * 255 / 100;
  }
  return ((uint32_t)red << 16) | ((uint32_t)grn << 8) | blu;
}

void drawGrid() {
  unsigned long t0 = micros();

  // 校准偏移变化：擦除旧网格区域，强制全量重绘
  if (offsetX != prevOffsetX || offsetY != prevOffsetY) {
    k10.canvas->clearLocalCanvas(prevOffsetX, prevOffsetY, 8 * cellW, 8 * cellH);
    memset(prevColor, 0xFF, sizeof(prevColor));
    prevOffsetX = offsetX;
    prevOffsetY = offsetY;
  }

  // 顶栏：内容变化时先擦文字条再重写，避免叠字残影
  String info = String("X:") + String(offsetX) + " Y:" + String(offsetY) + " F:" + String(drawFps);
  if (info != prevText) {
    k10.canvas->clearLocalCanvas(0, 0, 240, 16);
    k10.canvas->canvasText(
      info, 0, 0, 0xFFFFFF,
      k10.canvas->eCNAndENFont16, 240, false);
    prevText = info;
  }

  for (int i = 0; i < 64; i++) {
    uint16_t d = depthBuf[i];

    int x = offsetX + (i % 8) * cellW;
    int y = offsetY + (i / 8) * cellH;

    uint32_t c;
    if (d == 0 || d > 3500) {
      c = 0x111111;
    } else {
      c = distanceToColor(d);
    }

    // 不透明覆盖，只画颜色变化的格子，无需擦除
    if (c == prevColor[i]) continue;
    prevColor[i] = c;

    k10.canvas->canvasRectangle(x, y, cellW - 1, cellH - 1, c, c, false);
  }

  k10.canvas->updateCanvas();
  tDraw = micros() - t0;
}

void setup() {
  Serial.begin(115200);
  k10.begin();
  k10.initScreen(2);
  k10.initBgCamerImage();
  k10.setBgCamerImage(false);
  k10.creatCanvas();
  k10.canvas->canvasSetLineWidth(1);

  memset(prevColor, 0xFF, sizeof(prevColor));

  Wire.setClock(400000);

  Serial.println("Init TOF...");
  while (tof.begin() != 0) {
    Serial.println("TOF fail, retry...");
    delay(1000);
  }
  Serial.println("TOF OK, set 8x8...");
  while (tof.setRangingMode(eMatrix_8X8) != 0) {
    Serial.println("setRange fail, retry...");
    delay(1000);
  }
  Serial.println("TOF ready");

  k10.setBgCamerImage(true);
  fpsTime = millis();
  lastDraw = millis();
}

void loop() {
  unsigned long now = millis();

  // 读取 TOF
  unsigned long t0 = micros();
  uint8_t ret = tof.getAllData(depthBuf);
  tTof = micros() - t0;

  if (ret == 0) {
    rotate180(depthBuf);
  }

  // 按钮 A=X, B=Y
  bool curA = k10.buttonA->isPressed();
  bool curB = k10.buttonB->isPressed();

  if (curA && !prevA) { pressAStart = now; lastAdjustA = now; }
  if (curA && prevA && (now - pressAStart > 500) && (now - lastAdjustA >= 120)) {
    offsetX -= 2;
    lastAdjustA = now;
  } else if (!curA && prevA && (now - pressAStart < 500)) {
    offsetX += 2;
  }

  if (curB && !prevB) { pressBStart = now; lastAdjustB = now; }
  if (curB && prevB && (now - pressBStart > 500) && (now - lastAdjustB >= 120)) {
    offsetY -= 2;
    lastAdjustB = now;
  } else if (!curB && prevB && (now - pressBStart < 500)) {
    offsetY += 2;
  }

  prevA = curA;
  prevB = curB;

  // 钳制在屏幕范围内（240x320，网格 224x224）
  offsetX = constrain(offsetX, 0, 240 - 8 * cellW);
  offsetY = constrain(offsetY, 0, 320 - 8 * cellH);

  // 定时刷新画面
  if (now - lastDraw >= DRAW_INTERVAL) {
    drawGrid();
    drawCount++;
    lastDraw = now;
  }

  // FPS 统计
  if (now - fpsTime >= 1000) {
    drawFps = drawCount;
    drawCount = 0;
    fpsTime = now;
  }
}
