# tools

开发期脚本，不参与设备运行。

## 计划中的脚本

| 脚本 | 作用 | 依赖硬件 |
| --- | --- | --- |
| `simulate_device.py` | 模拟设备往 MQTT 发事件与位置，用来先把 Node-RED 面板调通 | 否 |
| `analyze.py` | 读取 IMU 采集的 CSV，算特征、画箱线图、定阈值 | 否 |
| `replay.py` | 把 CSV 按时间轴回放成事件流 | 否 |

`simulate_device.py` 值得优先写：**硬件还没到货时就能把云端链路走通**。

## 约定

- Python 3.10+，依赖用 `requirements.txt` 管
- 输出图统一放 `../data/`，不提交（已在根 `.gitignore` 里忽略）
- 每个脚本开头注释写清楚「输入什么、输出什么、怎么用」
