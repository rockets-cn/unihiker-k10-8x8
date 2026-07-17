# unihiker-k10-8x8

在行空板 K10(UNIHIKER K10)上,将 DFRobot MatrixLidar TOF 传感器的 8x8 深度数据以彩色网格形式实时叠加到摄像头画面上。

## 效果

- 摄像头实时画面作为背景
- 8x8 深度网格叠加显示,颜色表示距离:
  - 红色 = 近
  - 绿色 = 中距离
  - 蓝色 = 远
  - 深灰 = 无效数据(0 或超过 3500mm)
- 顶栏显示当前校准偏移量和刷新帧率

## 硬件

- 行空板 K10(UNIHIKER K10)
- DFRobot MatrixLidar TOF 传感器(I2C,地址 `0x33`,400kHz)

## 依赖库

- `unihiker_k10`
- `DFRobot_MatrixLidar`

## 使用

用 Arduino IDE 打开 `tof-camera-overlay/tof-camera-overlay.ino`,选择行空板 K10 开发板编译上传即可。

### 校准模式

网格位置可通过按键微调,使其与摄像头画面对齐:

| 按键 | 短按 | 长按(>500ms) |
|------|------|---------------|
| A    | X 偏移 +2 | X 偏移 -2 |
| B    | Y 偏移 +2 | Y 偏移 -2 |

默认偏移为 `X:0, Y:40`,每个网格单元 28x28 像素。深度数据做了 180° 旋转以匹配摄像头方向。

## 实现说明

- 画面每 200ms 刷新一次(`DRAW_INTERVAL`)
- 只重绘颜色发生变化的网格单元,减少刷屏开销
