# Home Assistant 接入

bike-hub 的"看和控"界面。设备只管往 MQTT 发消息，这里负责展示、远程命令、推送。

**核心逻辑仍在设备上**（振动判定、状态机、喇叭、锁），断网也能防盗。HA 挂了不影响车被撬时响警报。

---

## 文件

| 文件 | 内容 |
| --- | --- |
| `packages/bikehub.yaml` | 12 个实体 + 5 个脚本 + 5 个自动化 + 音频下拉框 |
| `lovelace/bikehub-view.yaml` | 手机端面板（地图 / 状态 / 控制 / 音频 / 趋势 / 事件） |

协议字段定义见 [docs/protocol.md](../../docs/protocol.md)。

---

## 接入三步

### 1. 配 MQTT 集成

设置 → 设备与服务 → 添加集成 → **MQTT**，填 broker 地址。

本机调试时 broker 就是 `tools/mqtt_lite.py`：

```bash
python3 tools/mqtt_lite.py        # 监听 127.0.0.1:1883
```

如果 HA 和 broker 不在同一台机器，把 `--host 0.0.0.0` 加给 mqtt_lite。

### 2. 放配置

```bash
# 拷进 HA 的 config/ 目录
cp -r packages lovelace  /path/to/homeassistant/config/

# 把 devId 换成你的（MAC 后 6 位小写十六进制）
cd /path/to/homeassistant/config
sed -i 's/7cdfa1/YOUR_DEV_ID/g' packages/bikehub.yaml
```

`configuration.yaml` 里加一行，让 HA 加载 packages：

```yaml
homeassistant:
  packages: !include_dir_named packages
```

重启 HA。设置 → 设备与服务 → 应该出现一个叫 **BikeHub** 的设备。

### 3. 放面板

仪表盘 → ⋮ → 编辑仪表盘 → ⋮ → 原始配置编辑器，把 `lovelace/bikehub-view.yaml` 里 `views:` 那一段合并进去。

---

## 改两个占位符

| 占位符 | 改成什么 | 在哪找 |
| --- | --- | --- |
| `7cdfa1` | 你的 devId | `firmware` 里由 MAC 后 6 位生成 |
| `notify.mobile_app_myphone` | 你手机的实际服务名 | 设置 → 设备与服务 → 实体 → 搜 `notify`，形如 `notify.mobile_app_iphone15` |

手机上装 **Home Assistant** App（App Store / 应用商店，免费），登录后会自动注册 `notify.mobile_app_*` 服务。

---

## 手机 App 能做什么

面板之外，App 还有三件现成的东西：

| 能力 | 说明 |
| --- | --- |
| **推送** | 不需要 Bark / ntfy，App 直接收 |
| **可操作通知** | 通知上带按钮，点「放大警笛」就发 MQTT cmd |
| **定位上报** | App 本身可作为 device_tracker（跟自行车位置无关） |

**可操作通知是这个方案最值钱的地方** —— 手机锁屏时收到「⚠ 车被动了」，直接点按钮就能让喇叭响，不用打开任何界面。

---

## 没硬件时怎么调

用 `tools/simulate_device.py` 冒充设备：

```bash
# 终端 1
python3 tools/mqtt_lite.py

# 终端 2：常驻，每 5 秒一次心跳
python3 tools/simulate_device.py --scenario heartbeat --interval 5
```

HA 里立刻能看到电量、布防、位置在动。

再发一条命令试试联动：

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, 'tools')
import mqtt_lite as M
c = M.Client(client_id="me"); c.connect()
c.publish("bikehub/7cdfa1/cmd", '{"v":1,"id":7,"cmd":"lock"}', qos=1)
EOF
```

面板上的锁态应该变成 `LOCKED`，布防变成 `ARMED`。

**触发一次告警推送**：

```bash
python3 tools/simulate_device.py --scenario alarm
```

手机会收到带按钮的通知。

---

## 部署到云服务器

买了云主机之后：

```
设备 ──4G──► 云服务器
              ├── mosquitto（端口 1883，只对设备开放）
              ├── Home Assistant（1883 客户端）
              └── (可选) InfluxDB
```

**不要直接把 HA 的 8123 端口暴露到公网**。它设计给局域网用，公网暴露有被扫描爆破的风险。

推荐 **Tailscale**（免费，零配置 VPN）：

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up
```

手机装 Tailscale 客户端登录同一个账号，就能在任意网络下访问 `http://100.x.x.x:8123`。**不用开任何端口，不用配证书。**

其他方案：Cloudflare Tunnel（免费但要多配一步）、Nginx + Let's Encrypt（传统，要维护证书）。

---

## 常见问题

**实体显示 `unavailable`**
→ MQTT 没连上，或 broker 里没有 retained 的 `state`。先用 `tools/simulate_device.py` 造一条看看。

**地图不显示位置**
→ 检查 `device_tracker.bikehub_tracker` 的属性里有没有 `latitude` / `longitude`。用 开发者工具 → 状态 搜 `bikehub_tracker` 看。

**通知按钮点了没反应**
→ `mobile_app_notification_action` 事件的 `action` 字段是否以 `BIKEHUB_` 开头。用 开发者工具 → 事件 → 监听 `mobile_app_notification_action` 抓一次。

**`sensor.bikehub_last_event` 重启后变成 unknown**
→ 正常。`evt` 是 QoS1 不 retained，HA 重启后要等下一次事件。

**改了音频清单**
→ 同时改两处：`input_select.bikehub_sound` 的 `options`，和固件 `firmware/data/` 里实际存在的 `aN.wav`。设备只认真实文件，编号对不上会回 `alarm_absent`。

---

## 为什么不用 Node-RED

Node-RED 的连线图更接近 Niagara 的 Wire Sheet，但 HA 在**手机 App、推送、地图、历史数据**这四件事上全是开箱即用，而 Node-RED 每一样都要再接一个东西。

一人一车的量级，少装三个组件比多一张连线图值钱。

真要图形化编排，HA 有 `hass-node-red` 集成，两者可以并存。
