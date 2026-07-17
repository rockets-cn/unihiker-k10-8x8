#include "unihiker_k10.h"
#include "DFRobot_MatrixLidar.h"
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "img_converters.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD_VALUE
#define WIFI_PASSWORD_VALUE "YOUR_WIFI_PASSWORD"
#endif

// unihiker_k10 库内部的摄像头帧队列（RGB565 QVGA）
extern QueueHandle_t xQueueCamer;

// WiFi 配置：改成你的 WiFi 名称和密码
const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;
const char* APP_VERSION = "overlay-v7";

UNIHIKER_K10 k10;
DFRobot_MatrixLidar_I2C tof(0x31);

uint16_t depthBuf[64];
portMUX_TYPE depthMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t tofSampleCount = 0;

// Web 服务器
WebServer server(80);
bool serverStarted = false;
TaskHandle_t webTaskHandle = NULL;
unsigned long lastWifiCheck = 0;

// 已保存帧（环形缓冲，最多 50 帧）
#define MAX_SAVED 50
struct SavedFrame {
  uint32_t t;       // millis() 时间戳
  uint16_t d[64];   // 8x8 深度数据
  uint8_t* jpg;     // 摄像头照片（PSRAM）
  size_t jpgLen;
};
SavedFrame savedFrames[MAX_SAVED];
int savedHead = 0;    // 下一个写入位置
int savedCount = 0;   // 已保存数量
uint16_t* jpegScaleBuf = NULL;

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
const unsigned long DRAW_INTERVAL = 50;

// 计时
volatile unsigned long tTof = 0;
unsigned long tDraw = 0;
int drawFps = 0;
int tofFps = 0;
unsigned long drawCount = 0;
uint32_t lastTofSampleCount = 0;
unsigned long fpsTime = 0;

// 显示缓冲
uint32_t prevColor[64];
int prevOffsetX = 0;
int prevOffsetY = 40;
String prevText = "";

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

void copyDepth(uint16_t* out) {
  portENTER_CRITICAL(&depthMux);
  memcpy(out, depthBuf, sizeof(depthBuf));
  portEXIT_CRITICAL(&depthMux);
}

void readTofTask(void* parameter) {
  uint16_t sample[64];
  for (;;) {
    unsigned long t0 = micros();
    if (tof.getAllData(sample) == 0) {
      portENTER_CRITICAL(&depthMux);
      memcpy(depthBuf, sample, sizeof(depthBuf));
      portEXIT_CRITICAL(&depthMux);
      tofSampleCount++;
    }
    tTof = micros() - t0;
    taskYIELD();
  }
}

// ---------- Web 服务器 ----------

// 从摄像头帧队列抓一帧，降采样到 160x120 后转成 JPEG。
// 保留传感器原始方向，相比旧版软件旋转结果正好转 180°，也省去逐像素翻转。
// 成功时 out 指向 malloc 分配的 JPEG 数据，调用方负责 free()
bool captureJpeg(uint8_t** out, size_t* outLen) {
  *out = NULL;
  *outLen = 0;
  if (xQueueCamer == NULL) return false;
  camera_fb_t* frame = NULL;
  if (xQueueReceive(xQueueCamer, &frame, pdMS_TO_TICKS(250)) != pdTRUE || frame == NULL) {
    return false;
  }
  const int scaledWidth = frame->width / 2;
  const int scaledHeight = frame->height / 2;
  if (jpegScaleBuf == NULL) {
    jpegScaleBuf = (uint16_t*)ps_malloc(scaledWidth * scaledHeight * sizeof(uint16_t));
  }
  if (jpegScaleBuf == NULL) {
    esp_camera_fb_return(frame);
    return false;
  }
  const uint16_t* source = (const uint16_t*)frame->buf;
  for (int y = 0; y < scaledHeight; y++) {
    for (int x = 0; x < scaledWidth; x++) {
      jpegScaleBuf[y * scaledWidth + x] = source[(y * 2) * frame->width + x * 2];
    }
  }
  bool ok = fmt2jpg((uint8_t*)jpegScaleBuf, scaledWidth * scaledHeight * sizeof(uint16_t),
                    scaledWidth, scaledHeight,
                    PIXFORMAT_RGB565, 20, out, outLen);
  esp_camera_fb_return(frame);
  return ok;
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>K10 TOF 8x8</title>
<style>
body{font-family:sans-serif;background:#222;color:#eee;text-align:center;margin:0;padding:12px}
#main{display:flex;justify-content:center;margin:10px auto}
#view{position:relative;width:min(640px,94vw);aspect-ratio:4/3;overflow:hidden;background:#000;border-radius:6px}
#cam{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
#grid{position:absolute;inset:0;display:grid;grid-template-columns:repeat(8,1fr);grid-template-rows:repeat(8,1fr);gap:1px;pointer-events:none}
.cell{display:flex;align-items:center;justify-content:center;font-size:clamp(8px,1.8vw,12px);color:#fff;text-shadow:0 1px 3px #000;border:1px solid rgba(255,255,255,.18);box-sizing:border-box}
.cap{color:#aaa;font-size:12px;margin-top:4px}
.version{color:#777;font-size:11px;margin-top:10px}
button,a.btn{display:inline-block;margin:4px;padding:8px 14px;background:#0a7;color:#fff;border:0;border-radius:4px;cursor:pointer;text-decoration:none;font-size:14px}
button.gray{background:#777}
#saved{margin:14px auto 0;font-size:13px;text-align:left;max-width:480px}
.row{padding:3px 6px;border-bottom:1px solid #444;display:flex;justify-content:space-between;align-items:center;gap:8px}
.row img{border-radius:2px;vertical-align:middle}
</style>
</head>
<body>
<h3>TOF 8x8 深度数据</h3>
<div id="info">加载中...</div>
<div id="main">
<div>
<div id="view">
<img id="cam" src="/cam.jpg" alt="camera">
<div id="grid"></div>
</div>
<div class="cap">摄像头 + TOF 叠加画面</div>
</div>
</div>
<div>
<button onclick="saveFrame()">保存当前帧</button>
<a class="btn" href="/saved.csv">下载 CSV</a>
<button class="gray" onclick="clearSaved()">清空记录</button>
</div>
<div id="saved"></div>
<div class="version">overlay-v7</div>
<script>
const cells=[];
const grid=document.getElementById('grid');
let cameraFps=0,cameraFrames=0,cameraFpsAt=performance.now();
for(let i=0;i<64;i++){const c=document.createElement('div');c.className='cell';grid.appendChild(c);cells.push(c);}
function distColor(mm){
  if(mm==0||mm>3500)return 'rgba(17,17,17,.32)';
  const r=Math.floor(mm*200/3500);let red,grn,blu;
  if(r<100){red=255;grn=Math.floor(r*255/100);blu=0;}
  else{red=Math.floor((200-r)*255/100);grn=red;blu=Math.floor((r-100)*255/100);}
  return `rgba(${red},${grn},${blu},.42)`;
}
async function refresh(){
  try{
    const j=await(await fetch('/data')).json();
    document.getElementById('info').textContent=`${j.v} X:${j.x} Y:${j.y} LCD:${j.f} CAM:${cameraFps} TOF:${j.tf} 已存:${j.n}`;
    j.d.forEach((mm,i)=>{cells[i].style.background=distColor(mm);cells[i].textContent=(mm==0||mm>3500)?'--':mm;});
  }catch(e){}
  setTimeout(refresh,100);
}
async function saveFrame(){await fetch('/save',{method:'POST'});loadSaved();}
async function clearSaved(){await fetch('/clear',{method:'POST'});loadSaved();}
async function loadSaved(){
  try{
    const j=await(await fetch('/saved')).json();
    let h=`<b>已保存 ${j.n} 帧</b>`;
    j.frames.forEach((f,i)=>{
      const v=f.d.filter(x=>x>0&&x<=3500);
      const mn=v.length?Math.min(...v):0, mx=v.length?Math.max(...v):0;
      h+=`<div class="row"><span>#${i+1} t=${f.t}ms</span><span>${mn}~${mx}mm</span>`+
         (f.img?`<a href="/savedimg?i=${i}" target="_blank"><img src="/savedimg?i=${i}" loading="lazy" width="80"></a>`:'<span>无图</span>')+
         `</div>`;
    });
    document.getElementById('saved').innerHTML=h;
  }catch(e){}
}
const camImg=document.getElementById('cam');
function refreshCamera(){camImg.src='/cam.jpg?t='+Date.now();}
camImg.onload=()=>{
  cameraFrames++;
  const now=performance.now();
  if(now-cameraFpsAt>=1000){cameraFps=Math.round(cameraFrames*1000/(now-cameraFpsAt));cameraFrames=0;cameraFpsAt=now;}
  setTimeout(refreshCamera,20);
};
camImg.onerror=()=>setTimeout(refreshCamera,200);
refreshCamera();refresh();loadSaved();
</script>
</body>
</html>)rawliteral";

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
  uint16_t data[64];
  copyDepth(data);
  server.sendHeader("Cache-Control", "no-store");
  String j = "{\"v\":\"" + String(APP_VERSION) + "\",\"t\":" + String(millis()) +
             ",\"x\":" + String(offsetX) +
             ",\"y\":" + String(offsetY) +
             ",\"f\":" + String(drawFps) +
             ",\"tf\":" + String(tofFps) +
             ",\"n\":" + String(savedCount) + ",\"d\":[";
  for (int i = 0; i < 64; i++) {
    j += String(data[i]);
    if (i < 63) j += ',';
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSave() {
  SavedFrame& f = savedFrames[savedHead];
  if (f.jpg) { free(f.jpg); f.jpg = NULL; f.jpgLen = 0; }
  f.t = millis();
  copyDepth(f.d);
  // 同时抓一张摄像头照片，存到 PSRAM
  uint8_t* jpg = NULL;
  size_t jpgLen = 0;
  if (captureJpeg(&jpg, &jpgLen)) {
    f.jpg = (uint8_t*)ps_malloc(jpgLen);
    if (f.jpg) {
      memcpy(f.jpg, jpg, jpgLen);
      f.jpgLen = jpgLen;
    }
    free(jpg);
  }
  savedHead = (savedHead + 1) % MAX_SAVED;
  if (savedCount < MAX_SAVED) savedCount++;
  server.send(200, "application/json", String("{\"n\":") + String(savedCount) + "}");
}

void handleClear() {
  for (int i = 0; i < MAX_SAVED; i++) {
    if (savedFrames[i].jpg) { free(savedFrames[i].jpg); savedFrames[i].jpg = NULL; }
    savedFrames[i].jpgLen = 0;
  }
  savedHead = 0;
  savedCount = 0;
  server.send(200, "application/json", "{\"n\":0}");
}

void handleCamJpg() {
  uint8_t* jpg = NULL;
  size_t jpgLen = 0;
  if (!captureJpeg(&jpg, &jpgLen)) {
    server.send(503, "text/plain", "camera unavailable");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(jpgLen);
  server.send(200, "image/jpeg", "");
  server.sendContent((const char*)jpg, jpgLen);
  free(jpg);
}

void handleSavedImg() {
  int idx = server.arg("i").toInt();
  if (idx < 0 || idx >= savedCount) {
    server.send(404, "text/plain", "no such frame");
    return;
  }
  SavedFrame& f = savedFrames[(savedHead + MAX_SAVED - savedCount + idx) % MAX_SAVED];
  if (!f.jpg) {
    server.send(404, "text/plain", "no image");
    return;
  }
  server.setContentLength(f.jpgLen);
  server.send(200, "image/jpeg", "");
  server.sendContent((const char*)f.jpg, f.jpgLen);
}

void handleSaved() {
  String j = "{\"n\":" + String(savedCount) + ",\"frames\":[";
  for (int s = 0; s < savedCount; s++) {
    // 从最旧到最新
    SavedFrame& f = savedFrames[(savedHead + MAX_SAVED - savedCount + s) % MAX_SAVED];
    if (s) j += ',';
    j += "{\"t\":" + String(f.t) + ",\"img\":" + (f.jpg ? "true" : "false") + ",\"d\":[";
    for (int i = 0; i < 64; i++) {
      j += String(f.d[i]);
      if (i < 63) j += ',';
    }
    j += "]}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSavedCsv() {
  String csv = "t";
  for (int i = 0; i < 64; i++) csv += ",d" + String(i);
  csv += "\n";
  for (int s = 0; s < savedCount; s++) {
    SavedFrame& f = savedFrames[(savedHead + MAX_SAVED - savedCount + s) % MAX_SAVED];
    csv += String(f.t);
    for (int i = 0; i < 64; i++) csv += "," + String(f.d[i]);
    csv += "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=tof_frames.csv");
  server.send(200, "text/csv", csv);
}

void webServerTask(void* parameter) {
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void startWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/saved", handleSaved);
  server.on("/saved.csv", handleSavedCsv);
  server.on("/cam.jpg", handleCamJpg);
  server.on("/savedimg", handleSavedImg);
  server.begin();
  serverStarted = true;
  if (xTaskCreatePinnedToCore(webServerTask, "webServer", 6144, NULL, 1,
                              &webTaskHandle, 0) != pdPASS) {
    webTaskHandle = NULL;
    Serial.println("Web task failed, using main loop");
  }
}

void beginWifiConnection() {
  if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
    // 未提供本地配置时，尝试使用 ESP32 NVS 中保存的网络。
    WiFi.begin();
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  beginWifiConnection();
  Serial.print("WiFi connecting");
  lastWifiCheck = millis();
}

void updateWifi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverStarted) {
      Serial.println();
      Serial.print("WiFi OK, IP: ");
      Serial.println(WiFi.localIP());
      startWebServer();
      // 屏幕第二行显示 IP（顶栏下方）
      k10.canvas->canvasText(WiFi.localIP().toString(), 0, 16, 0x00FF00,
                             k10.canvas->eCNAndENFont16, 240, false);
      k10.canvas->updateCanvas();
    }
    return;
  }

  if (now - lastWifiCheck >= 10000) {
    lastWifiCheck = now;
    Serial.print('.');
    beginWifiConnection();
  }
}

void drawGrid() {
  unsigned long t0 = micros();
  uint16_t data[64];
  copyDepth(data);

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
    uint16_t d = data[i];

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

  if (xTaskCreatePinnedToCore(readTofTask, "readTof", 4096, NULL, 2, NULL, 0) != pdPASS) {
    Serial.println("TOF task failed");
  }

  k10.setBgCamerImage(true);
  setupWifi();
  fpsTime = millis();
  lastDraw = millis();
}

void loop() {
  unsigned long now = millis();

  // WiFi 非阻塞连接；离线不影响本机摄像头与 TOF 网格刷新。
  updateWifi(now);
  if (serverStarted && webTaskHandle == NULL) server.handleClient();

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
    uint32_t samples = tofSampleCount;
    tofFps = samples - lastTofSampleCount;
    lastTofSampleCount = samples;
    fpsTime = now;
  }
}
