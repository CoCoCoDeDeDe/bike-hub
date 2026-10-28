#!/usr/bin/env python3
"""生成报警音频（合成音部分），输出 16 kHz / 16 bit / 单声道 PCM WAV。

只用标准库，不依赖 ffmpeg。语音段落留空，用 TTS 或录音补。

用法：
    python3 tools/make_audio.py              # 写到 ../data/
    python3 tools/make_audio.py -o /tmp/audio
    python3 tools/make_audio.py --list       # 只看清单，不生成

生成的编号与 docs/prototype-plan.md 的 index.json 设计对齐：
    a1 警笛-标准   循环    8 s
    a2 警笛-急促   循环    4 s
    a3 脉冲报警    单次    6 s
    a4 提示音-寻车 单次    1 s
    a5–a9          留给语音（TTS 或录音）
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import wave
from pathlib import Path

SAMPLE_HZ = 16000          # 必须与 config.h 的 AUDIO_SAMPLE_HZ 一致
CHANNELS = 1
SAMPLE_BYTES = 2           # 16 bit
PEAK = 0.89                # ≈ −1 dBFS，留余量防削顶
WAV_HEADER_BYTES = 44      # alarm.cpp 硬编码跳过这么多字节

# 段与段之间的淡入淡出，防止扬声器"啪"一声
FADE_MS = 8


# ---------------------------------------------------------------- 波形工具

def _fade(samples: list[float], ms: int, sample_hz: int = SAMPLE_HZ) -> None:
    """原地加线性淡入淡出。"""
    n = min(int(ms * sample_hz / 1000), len(samples) // 2)
    if n <= 0:
        return
    for i in range(n):
        k = i / n
        samples[i] *= k
        samples[-1 - i] *= k


def _normalize(samples: list[float], peak: float = PEAK) -> None:
    """缩放到目标峰值，避免不同段之间音量差太多。"""
    m = max((abs(v) for v in samples), default=0.0)
    if m <= 1e-9:
        return
    scale = peak / m
    for i in range(len(samples)):
        samples[i] *= scale


def _silence(ms: int, sample_hz: int = SAMPLE_HZ) -> list[float]:
    return [0.0] * int(ms * sample_hz / 1000)


def _write_wav(path: Path, samples: list[float]) -> None:
    """把 −1.0…1.0 的浮点样本写成 16 bit PCM WAV。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for v in samples:
        x = max(-1.0, min(1.0, v))
        frames += struct.pack("<h", int(round(x * 32767)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPLE_BYTES)
        w.setframerate(SAMPLE_HZ)
        w.writeframes(bytes(frames))


def _sweep_phase(freq_fn, duration_s: float, sample_hz: int = SAMPLE_HZ) -> list[float]:
    """对瞬时频率做数值积分得到相位，保证频率连续变化时波形不断裂。

    freq_fn 接受"采样点索引"，返回该点的瞬时频率（Hz）。
    返回相位序列（弧度）。
    """
    n = int(duration_s * sample_hz)
    out = [0.0] * n
    phase = 0.0
    for i in range(n):
        out[i] = phase
        phase += 2.0 * math.pi * freq_fn(i) / sample_hz
    return out


# ---------------------------------------------------------------- 各段音频

def make_siren_standard() -> list[float]:
    """a1 警笛-标准：8 s，频率在 600–1200 Hz 正弦摆动。

    摆动周期 4 s，8 s 正好两个整周期 → 首尾频率与相位都接得上，
    循环播放不会有"接缝"。
    """
    dur = 8.0
    n = int(dur * SAMPLE_HZ)
    lfo = 2.0 * math.pi * 2.0 / dur      # 8 s 内摆 2 个来回

    def freq(i: int) -> float:
        return 900.0 + 300.0 * math.sin(lfo * i / SAMPLE_HZ)

    phase = _sweep_phase(freq, dur)
    samples = [PEAK * math.sin(p) for p in phase]
    return samples


def make_siren_urgent() -> list[float]:
    """a2 警笛-急促：4 s，800/1200 Hz 方波化交替，每档 0.25 s。

    方波比正弦刺耳，用于"已经很危险"的场景。这里用叠加奇次谐波
    近似方波，避免真正的方波在高频折返。
    """
    dur = 4.0
    n = int(dur * SAMPLE_HZ)
    step = int(0.25 * SAMPLE_HZ)
    samples = [0.0] * n
    for i in range(n):
        high = (i // step) % 2 == 1
        f = 1200.0 if high else 800.0
        t = i / SAMPLE_HZ
        # 基波 + 3/5 次谐波，听感接近方波
        v = math.sin(2 * math.pi * f * t)
        v += math.sin(2 * math.pi * 3 * f * t) / 3.0
        v += math.sin(2 * math.pi * 5 * f * t) / 5.0
        samples[i] = v / 1.53

    # 在每档切换处做 3 ms 斜坡，避免咔嗒声
    ramp = int(0.003 * SAMPLE_HZ)
    for k in range(0, n, step):
        for i in range(min(ramp, n - k)):
            samples[k + i] *= i / ramp
        for i in range(min(ramp, n - k - step)):
            samples[k + step - 1 - i] *= i / ramp
    return samples


def make_pulse_alarm() -> list[float]:
    """a3 脉冲报警：6 s，1000 Hz 短 beep 0.15 s + 静音 0.35 s。"""
    dur = 6.0
    beep_ms, gap_ms = 150, 350
    period = int((beep_ms + gap_ms) * SAMPLE_HZ / 1000)
    n = int(dur * SAMPLE_HZ)
    samples = [0.0] * n
    beep_n = int(beep_ms * SAMPLE_HZ / 1000)
    for start in range(0, n, period):
        for i in range(beep_n):
            if start + i >= n:
                break
            t = i / SAMPLE_HZ
            env = min(1.0, i / (0.004 * SAMPLE_HZ), (beep_n - i) / (0.004 * SAMPLE_HZ))
            samples[start + i] = env * math.sin(2 * math.pi * 1000.0 * t)
    return samples


def make_chirp_findme() -> list[float]:
    """a4 提示音-寻车：两声短促上扬 chirp，总长约 1 s。"""
    samples: list[float] = []
    for _ in range(2):
        dur = 0.12
        n = int(dur * SAMPLE_HZ)

        def freq(i: int) -> float:
            return 900.0 + 900.0 * (i / n)     # 900 → 1800 Hz 上扬

        phase = _sweep_phase(freq, dur)
        samples += [math.sin(p) for p in phase]
        samples += _silence(180)
    samples += _silence(300)
    return samples


# ---------------------------------------------------------------- 清单

# (id, 生成函数, 显示名, 建议音量, 是否适合循环)
SYNTH = [
    (1, make_siren_standard, "警笛-标准",   90, True),
    (2, make_siren_urgent,   "警笛-急促",   95, True),
    (3, make_pulse_alarm,    "脉冲报警",    88, True),
    (4, make_chirp_findme,   "提示音-寻车", 70, False),
]

# 语音段落留给 TTS / 录音，这里只占位，方便 index.json 一次生成到位
VOICE = [
    (5, "语音-已报警",   85, False),
    (6, "语音-请离开",   85, False),
    (7, "语音-已拍照",   85, False),
]


def build(out_dir: Path, verbose: bool = True) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    index: list[dict] = []
    total_bytes = 0

    for sid, fn, label, vol, loop in SYNTH:
        samples = fn()
        _fade(samples, FADE_MS)
        _normalize(samples)
        path = out_dir / f"a{sid}.wav"
        _write_wav(path, samples)

        dur = len(samples) / SAMPLE_HZ
        size = path.stat().st_size
        total_bytes += size

        # 校验：alarm.cpp 硬编码 44 字节头，多了会吃掉音频开头
        if size < WAV_HEADER_BYTES * 2:
            print(f"⚠ a{sid}.wav 过小", file=sys.stderr)

        if verbose:
            print(f"  a{sid}.wav  {dur:5.1f} s  {size/1024:6.1f} KB  {label}")
        index.append({
            "id": sid, "name": label, "file": f"/a{sid}.wav",
            "vol": vol, "loop": loop, "dur_ms": int(dur * 1000),
        })

    for sid, label, vol, loop in VOICE:
        index.append({
            "id": sid, "name": label, "file": f"/a{sid}.wav",
            "vol": vol, "loop": loop, "dur_ms": 0,
        })

    (out_dir / "index.json").write_text(
        json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    if verbose:
        print(f"\n  合成音 {len(SYNTH)} 段，共 {total_bytes/1024:.1f} KB "
              f"（LittleFS 预算 2 MB，余量充足）")
        print(f"  index.json 共 {len(index)} 项，其中 {len(VOICE)} 项待补语音")
        print(f"\n  输出目录：{out_dir.resolve()}")
    return 0


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="生成报警音频 WAV")
    ap.add_argument("-o", "--out", type=Path, default=here.parent / "data",
                    help="输出目录（默认 firmware/data）")
    ap.add_argument("--list", action="store_true", help="只列清单，不生成")
    args = ap.parse_args()

    if args.list:
        print(f"{'id':>3}  {'时长':>6}  {'循环':>4}  名称")
        for sid, _fn, label, _vol, loop in SYNTH:
            print(f"{sid:>3}  {'合成':>6}  {'是' if loop else '否':>4}  {label}")
        for sid, label, _vol, loop in VOICE:
            print(f"{sid:>3}  {'待补':>6}  {'是' if loop else '否':>4}  {label}")
        return 0

    print(f"生成音频 → {args.out}")
    return build(args.out)


if __name__ == "__main__":
    raise SystemExit(main())
