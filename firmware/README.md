# firmware

ESP32-S3 固件工程。当前是骨架版本：能编译、能烧录、串口打印启动信息和心跳。

## 快速开始

```bash
cd firmware
pio run                      # 编译
pio run -t upload            # 烧录
pio device monitor           # 看日志（115200）
```

## 两个编译环境

区别只在于串口走哪条路：

| 环境 | 用在哪个口 | 什么时候选 |
| --- | --- | --- |
| `s3-uart`（默认） | 板上 UART 转串口芯片（CP2102/CH340） | 板子上的 **UART/COM** 口 |
| `s3-usb` | 芯片原生 USB（GPIO19/20） | 板子上的 **USB** 口 |

插上后看不到输出，八成是选错了环境：

```bash
pio run -e s3-usb -t upload && pio device monitor -e s3-usb
```

## 预期输出

```
========================================
  bike-hub  firmware 0.1.0
  built Oct 27 2026 22:30:00
========================================

[chip]
  型号      : ESP32-S3 rev 0
  核心      : 2 @ 240 MHz
  Flash     : 16 MB
  PSRAM     : 8 MB (空闲 8192 KB)
  SDK       : v4.4.7
  MAC       : 7C:DF:A1:xx:xx:xx
  复位原因  : 上电

[boot] 骨架就绪，等待接入外设
[boot] 心跳间隔 2000 ms

[beat] up=2s  heap=380000  psram=8388608  min_heap=378000
```

## 目录约定

```
firmware/
├── src/          应用层：调度、状态机
├── lib/          可复用模块（sensor / detect / audio / net / power）
├── data/         LittleFS 内容（WAV 音频、index.json）
├── test/         host 上跑的单元测试
└── platformio.ini
```

**以后的规矩**：`loop()` 里只做调度，业务逻辑全部沉到 `lib/` 的模块里。这样第二曲线的码表功能可以直接加一个 `lib/ride/`，不用动主循环。

## 还没做的

- [ ] IMU（LIS3DH）采样与 CSV 输出
- [ ] 车轮霍尔计数
- [ ] Air780EG 的 AT 通道与 GNSS 通道
- [ ] I2S 音频播放
- [ ] 低功耗调度

## 首次编译会下载工具链

约 200–300 MB，之后缓存在 `~/.platformio`。如果卡住，多半是网络问题，可以设置代理后重试：

```bash
export https_proxy=http://127.0.0.1:7890
pio run
```
