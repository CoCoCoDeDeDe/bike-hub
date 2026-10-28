# hardware

外壳与结构件的建模目录。

**设计任务、验收标准和进度不在这里**，见 [硬件任务清单](../docs/hardware-checklist.md#b-机械设计)。

## 这里放什么

| 文件 | 作用 |
| --- | --- |
| `case.scad` | 主体外壳（电路仓、喇叭孔、充电口） |
| `params.scad` | 管径、壁厚、锁环长度等参数 |
| `*.stl` | 导出给切片软件 |

## 命令

```bash
openscad -o case.stl case.scad          # 命令行导出
openscad -D "d=34.9" -o case.stl case.scad   # 改参数导出
```

## 三条硬约束

1. **外壳不能是金属**——屏蔽 4G/GNSS 天线，天线区必须留塑料窗。
2. **承力件必须是金属**——3D 打印件只做外壳，锁舌和锁环走金属加工。
3. **霍尔位置可微调**——不同轮胎和辐条张力会让磁铁间隙变几毫米。

细节见 [清单 H-M 系列](../docs/hardware-checklist.md#b-机械设计)。
