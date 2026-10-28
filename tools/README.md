# tools

开发期脚本，不参与设备运行。**全部只用 Python 标准库**，不需要 `pip install`。

## 已有脚本

| 脚本 | 作用 | 依赖 |
| --- | --- | --- |
| `simulate_device.py` | 模拟设备按协议收发 MQTT，用来把 broker 和面板先调通 | 无 |
| `mqtt_lite.py` | 最小 MQTT 3.1.1 客户端 + broker（也能单独当 broker 用） | 无 |
| `selftest.py` | 上面两个的自检，27 项断言 | 无 |

## 快速上手

**先看消息长什么样（不连任何服务）**

```bash
python3 tools/simulate_device.py --dry-run --scenario alarm
```

```
→ online  [R1] {"v":1,"online":true}
→ state   [R0] {"v":1,"ts":1793171248,"lock":"LOCKED","guard":"ARMED",...}
→ evt     [ 1] {"v":1,"id":1,"ts":1793171249,"t":"alarm","rms":0.42,"lock":"LOCKED"}
→ state   [R0] {"v":1,"ts":1793171249,"lock":"LOCKED","guard":"ALARM","alarm":"PLAYING",...}
→ state   [R0] {"v":1,"ts":1793171251,"lock":"LOCKED","guard":"ARMED","alarm":"IDLE",...}
```

行首的 `[R0]` / `[ 1]` 是 `retained + QoS`，与 [协议表](../docs/protocol.md#2-topic-结构)一一对应。

**跑完整链路**

```bash
# 终端 1：起 broker（内存级 retained，端口 1883）
python3 tools/mqtt_lite.py

# 终端 2：模拟设备常驻，每 5 秒一次心跳
python3 tools/simulate_device.py --scenario heartbeat --interval 5
```

**给设备发命令**（终端 3）

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, 'tools')
import mqtt_lite as M
c = M.Client(client_id="me"); c.connect()
c.publish("bikehub/7cdfa1/cmd", '{"v":1,"id":7,"cmd":"play","arg":2}', qos=1)
EOF
```

设备的 stdout 会打印 `← cmd id=7 cmd=play arg=2`，随后回 `ack` 与新的 `state`。

## 场景

| `--scenario` | 行为 |
| --- | --- |
| `boot` | online → boot 事件 → state → loc，然后退出 |
| `alarm` | 撬锁告警全流程，含"告警结束回 ARMED"（不是 DISARMED） |
| `lock` | lock → unlock 各一次，含 MOVING 中间态 |
| `heartbeat` | boot 序列后常驻，按 `--interval` 发 state/loc |
| `all` | 依次跑 boot / alarm / lock，然后进心跳 |

## 几个参数

| 参数 | 用途 |
| --- | --- |
| `--dev-id` | 设备 ID，默认 `7cdfa1`（MAC 后 6 位） |
| `--port` | broker 端口，默认 1883 |
| `--interval` | 心跳秒数，调试时给 2–5，别用默认 60 |
| `--dry-run` | 不连 broker，只打印 |
| `--fake-ts` | `ts` 固定为 0，模拟设备没取到网络时间 |

## 自检

```bash
python3 tools/selftest.py      # 返回 0 表示全过
```

覆盖：MQTT 变长长度编码、通配符匹配、半包处理、retained 补发、`state` 字段与协议文档一致、全部命令分支（含 `no_such_sound` / `alarm_absent` / `locked` / `bad_cmd`）。

**改协议或改脚本后先跑这个**，比手动敲命令快。

## 没有 broker 怎么办

`tools/mqtt_lite.py` 自己就是 broker，不用装 mosquitto。它支持 CONNECT / PUBLISH / SUBSCRIBE / PING 和通配符、retained（内存级，进程退出即丢）。

要接 Node-RED 或真设备时，换成正式的 mosquitto / EMQX 即可，协议层完全一致。

## 计划中的

| 脚本 | 作用 |
| --- | --- |
| `analyze.py` | 读 IMU 采集的 CSV，算特征、画箱线图、定阈值 |
| `replay.py` | 把 CSV 按时间轴回放成事件流 |

## 约定

- Python 3.10+，**不引入第三方依赖**（这台机器没有 pip）
- 输出图统一放 `../data/`，不提交（已在根 `.gitignore` 忽略）
- 每个脚本开头注释写清楚「输入什么、输出什么、怎么用」
