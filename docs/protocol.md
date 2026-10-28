# bike-hub MQTT 协议

| 项 | 内容 |
| --- | --- |
| 版本 | v1 |
| 状态 | 已定稿，等待联调 |
| 实现 | `firmware/lib/protocol/` |
| 测试 | `firmware/test/test_protocol/` |

**为什么先定协议**：这是整个项目里"定错了返工成本最高"的部分。设备端、云端面板、手机端都按它实现，改一次要动三处。

---

## 1. 设计原则

| 原则 | 说明 |
| --- | --- |
| **JSON，不用二进制** | 原型阶段可读性 > 体积，HA 的模板能直接取值 |
| **版本号必带** | 每条消息都有 `v`，将来不兼容时能判断 |
| **命令必须幂等** | 重复执行不产生副作用，网络重传才安全 |
| **没有"撤防"命令** | 见 §5，这是安全底线，不是遗漏 |
| **状态用 retained** | 新连上的面板立刻能拿到当前状态，不用等下一次上报 |

---

## 2. Topic 结构

```
bikehub/{devId}/state     上行  状态快照    QoS0 + retained
bikehub/{devId}/evt       上行  事件        QoS1
bikehub/{devId}/loc       上行  位置        QoS0
bikehub/{devId}/ack       上行  命令回执    QoS1
bikehub/{devId}/cmd       下行  命令        QoS1
bikehub/{devId}/online    上行  遗嘱消息    QoS1 + retained
```

`devId`：MAC 后 6 位小写十六进制，例如 `7cdfa1`。

各 topic 的取舍：

| topic | QoS | retained | 理由 |
| --- | --- | --- | --- |
| `state` | 0 | ✅ | 丢了下次心跳补，但要能立刻读到 |
| `evt` | 1 | ❌ | 告警不能丢，但历史事件不该反复重放 |
| `loc` | 0 | ❌ | 位置密集，丢一两个无所谓 |
| `cmd` | 1 | ❌ | 命令必须送达，但不应残留 |
| `online` | 1 | ✅ | broker 代发遗嘱，实现"离线可见" |

---

## 3. 上行消息

### 3.1 state（状态快照）

```json
{
  "v": 1,
  "ts": 1698765432,
  "lock": "LOCKED",
  "guard": "ARMED",
  "batt": 86,
  "volt": 3.95,
  "light": "OFF",
  "alarm": "IDLE",
  "rssi": 18,
  "up": 3600,
  "fw": "0.4.0"
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `v` | int | 协议版本 |
| `ts` | int | Unix 秒；设备无时间时为 0，云端以接收时间为准 |
| `lock` | string | `LOCKED` / `UNLOCKED` / `MOVING` / `TAMPERED` |
| `guard` | string | `INIT` / `DISARMED` / `ARMED` / `ALARM` |
| `batt` | int | 电量百分比 0–100 |
| `volt` | float | 电池电压，一位小数 |
| `light` | string | `OFF` / `ON` / `ALERT` |
| `alarm` | string | `IDLE` / `PLAYING` |
| `rssi` | int | 信号强度，无网络时 -1 |
| `up` | int | 运行秒数 |
| `fw` | string | 固件版本 |

上报时机：每 60 秒一次，或状态变化时立即上报。

### 3.2 evt（事件）

```json
{
  "v": 1,
  "id": 42,
  "ts": 1698765432,
  "t": "alarm",
  "rms": 0.42,
  "lock": "LOCKED"
}
```

`t` 的取值（固定列表，不加自造类型）：

| t | 含义 | 附带字段 |
| --- | --- | --- |
| `boot` | 上电/复位 | `reason` |
| `lock` | 锁舌闭合 | — |
| `unlock` | 锁舌退出 | — |
| `alarm` | 触发告警 | `rms` |
| `tamper` | 防拆触发 | — |
| `lowbat` | 低电量 | `batt` |
| `net` | 网络状态变化 | `state` |

`id` 是设备侧递增序号，用于去重和排序。

### 3.3 loc（位置）

```json
{"v":1,"ts":1698765432,"lat":31.2304,"lon":121.4737,"acc":8.5,"spd":0.0}
```

| 字段 | 说明 |
| --- | --- |
| `lat` / `lon` | 十进制度数，6 位小数 |
| `acc` | 水平精度估计（米） |
| `spd` | 速度（km/h），静止时省略 |

### 3.4 ack（命令回执）

```json
{"v":1,"id":7,"ok":true}
{"v":1,"id":8,"ok":false,"err":"no_such_sound"}
```

`id` 与下行命令的 `id` 对应。错误码固定：

| err | 含义 |
| --- | --- |
| `bad_cmd` | 未知命令 |
| `no_such_sound` | 音频编号不存在 |
| `alarm_absent` | 音频文件缺失 |
| `locked` | 锁已在该状态，无法执行 |

---

## 4. 下行命令

```json
{"v":1,"id":7,"cmd":"play","arg":2}
{"v":1,"id":8,"cmd":"stop"}
{"v":1,"id":9,"cmd":"status"}
{"v":1,"id":10,"cmd":"lock"}
{"v":1,"id":11,"cmd":"unlock"}
```

**命令白名单**（只接受这些，其余一律回 `bad_cmd`）：

| cmd | arg | 作用 | 幂等 |
| --- | --- | --- | --- |
| `play` | 1–9 | 播放指定编号音频 | ✅ |
| `stop` | — | 停止播放 | ✅ |
| `status` | — | 立即回一条 state | ✅ |
| `lock` | — | 驱动电机上锁 | ✅ |
| `unlock` | — | 驱动电机开锁 | ✅ |

---

## 5. 安全约束：为什么没有 disarm

**协议里不存在"撤防"命令，这是设计决定的，不是没做完。**

设备侧的状态机是：

```
DISARMED ←── 锁舌退出 ── ARMED ←── 锁舌闭合 ── DISARMED
```

唯一能改变布防状态的是**锁的物理状态**。如果协议里加一个 `disarm`，那么任何拿到 MQTT 凭据的人（包括偷到手机的人）都能让设备闭嘴。

同理：

- 没有 `mute`（静音）
- 没有 `set_guard`（直接设状态）
- `unlock` 只驱动电机，能不能真的打开取决于机械钥匙

**云端能力边界**：能做的事只有"看"和"触发动作"，不能"改变保护状态"。

> 未来如果要做 BLE 开锁，链路必须是 `手机 → 锁验证凭证 → 锁舌动作 → hub 感知`，而不是 `手机 → hub 撤防`。

---

## 6. 时间与序号

| 问题 | 处理 |
| --- | --- |
| 设备没有 RTC | 上电后 `ts=0`，联网后从模组取网络时间 |
| 时钟不可信 | 云端一律以"接收时间"为准，设备 `ts` 只作参考 |
| 消息乱序 | `id` 递增，云端按 `id` 排序去重 |
| 重启后序号归零 | 可以接受；`boot` 事件标记新的会话 |

---

## 7. 兼容策略

- `v` 只在**不兼容变更**时递增（删字段、改语义）
- **新增字段不升版本**，接收方忽略未知字段
- 未知 `cmd` 回 `bad_cmd`，不崩溃
- 未知 `evt.t` 云端记录但不告警

---

## 8. 示例：一次完整的告警

```
设备  → evt    {"v":1,"id":42,"ts":0,"t":"alarm","rms":0.42,"lock":"LOCKED"}
设备  → state  {"v":1,"ts":0,"lock":"LOCKED","guard":"ALARM",...}
面板  → cmd    {"v":1,"id":7,"cmd":"stop"}
设备  → ack    {"v":1,"id":7,"ok":true}
设备  → state  {"v":1,"ts":0,"lock":"LOCKED","guard":"ARMED",...}
```

注意最后一条：`stop` 之后设备回到 `ARMED` 而不是 `DISARMED`——因为锁还锁着。

---

## 9. 实现约定

| 层 | 职责 | 依赖 |
| --- | --- | --- |
| `lib/protocol/` | JSON 构造与解析 | ArduinoJson |
| `lib/net/` | 网络状态机、重连、离线缓存 | protocol |
| `lib/modem/` | AT 指令、URC 匹配 | — |

`protocol` 层**不碰网络也不碰硬件**，所以能在 native 环境完整测试。见 `firmware/test/test_protocol/`。

### 9.1 协议联调工具

硬件没到货时，用 `tools/` 里的脚本按本协议收发：

```bash
python3 tools/simulate_device.py --dry-run --scenario alarm   # 只看 JSON
python3 tools/mqtt_lite.py                                    # 起一个最小 broker
python3 tools/simulate_device.py --scenario heartbeat         # 模拟设备
python3 tools/selftest.py                                     # 27 项自检
```

`simulate_device.py` 的字段顺序、可选项、错误码与本节完全对齐，`selftest.py` 会断言这一点。**改协议时先改这两处，再改 `lib/protocol/`**。
