# firmware

ESP32-S3 固件工程。当前已接入 LIS3DH：启动自检、50 Hz 连续采样、串口 CSV 输出。

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

## LIS3DH 接线

| LIS3DH | ESP32-S3 | 说明 |
| --- | --- | --- |
| VCC | 3V3 | 不要接 5V |
| GND | GND | |
| SDA | GPIO8 | 可在 `src/config.h` 改 |
| SCL | GPIO9 | 可在 `src/config.h` 改 |
| SA0 | GND | 决定地址：接地 0x18，接 VCC 0x19 |
| INT1 | 暂不接 | 后续低功耗唤醒用 |

模块上的 `SA0` 有的做成焊盘、有的引出排针。**驱动会自动探测 0x18 和 0x19**，接错也能识别，不影响。

## 串口输出格式

启动信息是普通文本，之后是 CSV 流，心跳以 `#` 开头（可当注释跳过）：

```
[imu] 扫描 I2C 总线…
       发现设备 0x18
[imu] 就绪  地址 0x18  WHO_AM_I 0x33
[imu] 量程 ±4g  灵敏度 2.0 mg/LSB  采样 50 Hz

# bike-hub imu stream — 断电结束
# t_ms,ax_mg,ay_mg,az_mg
1024,12,-3,998
1044,15,-2,1001
1064,9,-5,996
# [beat] up=5s heap=380000  min=378000  drop=0
```

Python 读取：

```python
df = pd.read_csv("imu.csv", comment="#", names=["t","ax","ay","az"])
```

## 为什么是 ±4 g

撬锁的冲击经常超过 2 g，±2 g 会削顶——波形顶部被切平，特征就毁了。±4 g 配 12 bit，灵敏度 2 mg/LSB，日常振动完全够用。

## 模块结构

```
firmware/
├── include/config.h      引脚、调度周期、Mock 开关
├── mock/Arduino.h        native 测试用的 Arduino 桩
├── test/test_guard/      状态机单元测试
├── src/main.cpp          初始化 + 调度 + 事件播报
└── lib/
    ├── lis3dh/           加速度计
    ├── lock/             锁舌状态 + 电机 + 防拆
    ├── light/            车灯 PWM + ID 识别
    ├── guard/            防盗状态机
    └── power/            电量
```

**规矩**：`main.cpp` 只做初始化和时间调度，业务判断全在 `lib/` 里。`guard` 消费 `lock` 的状态，`lock` 不反过来依赖 `guard`。

## 状态机统一模式

`lock` 和 `guard` 都遵循同一套写法，加新状态机时照抄：

| 要素 | 约定 |
| --- | --- |
| 状态 | `enum class XxxState` |
| 名称 | `static const char *name(XxxState)` |
| 迁移 | `enter()`：判重 → `onExit` → 切换 → `onEnter` → 通知 → 打印 |
| 钩子 | `onEnter/onExit` 只放本状态自己的副作用 |
| 输入 | `struct XxxInput` + `tick(const XxxInput &)` |
| 输出 | 通过 `setActionHandler` 回调，不直接依赖灯/喇叭 |

**这样做的两个好处**：

1. **迁移日志由模块自己打**，一定带上下文，不会漏
2. **加字段只改结构体**，不动调用签名

当前 `guard` 的迁移日志长这样：

```
[guard] DISARMED  -> ARMED     (lock=LOCKED tamper=0 rms=0.003)
[guard] ARMED     -> ALARM     (lock=LOCKED tamper=0 rms=0.412)
[guard] ALARM     -> DISARMED  (lock=UNLOCKED tamper=0 rms=0.001)
```

`main.cpp` 里已经没有状态判断，只有 `on_guard_action()` 一个分发函数。

## 串口命令

| 命令 | 作用 |
| --- | --- |
| `lock` / `unlock` | 模拟锁舌闭合 / 退出（Mock 模式） |
| `tamper` / `clear` | 模拟 / 清除防拆告警 |
| `light off \| on \| alert` | 控制车灯 |
| `stream on \| off` | 开关 50 Hz CSV 流 |
| `status` | 打印锁态、布防、电量、车灯 |
| `help` | 命令列表 |

典型流程：

```
lock                    → [lock] UNLOCKED -> LOCKED
                          [guard] DISARMED -> ARMED
（晃一下板子）
                          [guard] ARMED -> ALARM
unlock                  → [lock] LOCKED -> UNLOCKED
                          [guard] ALARM -> DISARMED
```

## 状态机边界

```
INIT ──锁上──▶ ARMED ──持续振动 1.5s──▶ ALARM
  │             ▲                        │
  └──未锁──▶ DISARMED ◀────开锁──────────┘
```

只有 `lock` 能改变布防/撤防。**没有按钮、没有手机指令能直接撤防**——这条在代码结构上就定死了。

## 单元测试

状态机逻辑在电脑上就能跑，不需要板子：

```bash
pio test -e native
```

```
test_unlocked_goes_disarmed                    [PASSED]
test_locked_goes_armed                         [PASSED]
test_sustained_motion_alarms                   [PASSED]
test_alarm_cools_down_to_armed                 [PASSED]
test_tamper_jumps_to_alarm_from_disarmed       [PASSED]
...
12 test cases: 12 succeeded
```

**怎么做到的**：`mock/Arduino.h` 提供最小桩，时间由 `mock_millis_ref()` 控制，测试里直接改它就能"快进"到冷却结束，不用真的等 10 秒。

```cpp
static void advance(uint32_t ms) { mock_millis_ref() += ms; }
```

新增状态机时，照着 `test/test_guard/` 的结构写用例即可。

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

- [x] IMU（LIS3DH）采样与 CSV 输出
- [x] 锁模块（Mock，真实 GPIO 待机械就绪）
- [x] 车灯接口（Mock，PWM 已实现）
- [x] 防盗状态机（lock → guard）
- [ ] 车轮霍尔计数
- [ ] 喇叭播放与告警音
- [ ] Air780EG 的 AT 通道与 GNSS 通道
- [ ] I2S 音频播放
- [ ] 低功耗调度

## 首次编译会下载工具链

约 200–300 MB，之后缓存在 `~/.platformio`。如果卡住，多半是网络问题，可以设置代理后重试：

```bash
export https_proxy=http://127.0.0.1:7890
pio run
```
