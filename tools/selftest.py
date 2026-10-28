#!/usr/bin/env python3
"""tools/ 下的自检：不依赖硬件、不依赖第三方包。

覆盖三块：
    1. mqtt_lite 的编解码与 broker 转发
    2. simulate_device 的 dry-run 输出格式（与 docs/protocol.md 对齐）
    3. 端到端：broker + 模拟设备 + 命令回执的全部错误分支

跑法：
    python3 tools/selftest.py        # 全部通过返回 0
"""

from __future__ import annotations

import json
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import mqtt_lite as M  # noqa: E402

FAILED: list[str] = []


def check(cond: bool, name: str) -> None:
    if cond:
        print(f"  ✅ {name}")
    else:
        print(f"  ❌ {name}")
        FAILED.append(name)


# ---------------------------------------------------------------- 1. 编解码

def test_codec() -> None:
    print("\n[1] mqtt_lite 编解码")

    cases = [(0, b"\x00"), (127, b"\x7f"), (128, b"\x80\x01"),
             (16383, b"\xff\x7f"), (16384, b"\x80\x80\x01")]
    check(all(M.encode_length(n) == want for n, want in cases),
          "变长长度编码（4 个边界值）")

    check(M.topic_matches("a/b", "a/b"), "精确匹配")
    check(M.topic_matches("a/+", "a/b"), "+ 匹配单层")
    check(M.topic_matches("a/#", "a/b/c"), "# 匹配多层")
    check(M.topic_matches("a/#", "a"), "# 匹配零层")
    check(not M.topic_matches("a/+", "a/b/c"), "+ 不匹配多层")
    check(not M.topic_matches("a/b", "a/c"), "不同主题不匹配")

    # PUBLISH 编解码往返
    raw = M.build_publish("t/x", b'{"v":1}', qos=1, retain=True, packet_id=7)
    r = M.Reader.__new__(M.Reader)
    r.buf, r.closed = bytearray(raw), False
    ptype, flags, body = r.try_parse()
    topic, payload, qos, retain, pid = M.parse_publish(flags, body)
    check((ptype, topic, payload, qos, retain, pid)
          == (M.PUBLISH, "t/x", b'{"v":1}', 1, True, 7),
          "PUBLISH 编解码往返")

    # 分片到达：一帧拆成两段喂进去
    r2 = M.Reader.__new__(M.Reader)
    r2.buf, r2.closed = bytearray(raw[:3]), False
    check(r2.try_parse() is None, "半包不解析")
    r2.buf += raw[3:]
    check(r2.try_parse() is not None, "补齐后解析成功")


# ---------------------------------------------------------------- 2. dry-run

def test_dry_run() -> None:
    print("\n[2] simulate_device 输出格式")

    out = subprocess.run(
        [sys.executable, str(HERE / "simulate_device.py"),
         "--dry-run", "--scenario", "alarm", "--fake-ts"],
        capture_output=True, text=True, timeout=30)
    check(out.returncode == 0, "dry-run 退出码 0")

    lines = [ln for ln in out.stdout.splitlines() if ln.startswith("→")]
    check(len(lines) >= 4, f"输出行数 ≥ 4（实际 {len(lines)}）")

    def parse(line: str):
        # "→ state   [R0] {...}"
        leaf = line[2:].split()[0]
        payload = line[line.index("{"):]
        return leaf, json.loads(payload)

    msgs = [parse(ln) for ln in lines]
    leaves = [k for k, _ in msgs]
    check("online" in leaves, "首条含 online")
    check("state" in leaves and "evt" in leaves, "含 state 与 evt")

    state = next(m for k, m in msgs if k == "state")
    check(state["v"] == 1, "state.v == 1")
    check(state["ts"] == 0, "--fake-ts 时 ts 为 0")
    check(set(state) == {"v", "ts", "lock", "guard", "light", "alarm",
                         "batt", "volt", "rssi", "up", "fw"},
          "state 字段与 protocol.md §3.1 完全一致")
    check(state["volt"] == 3.95, "volt 保留两位小数")

    evt = next(m for k, m in msgs if k == "evt")
    check(evt["t"] == "alarm" and "rms" in evt and "lock" in evt,
          "alarm 事件带 rms 与 lock")

    # 告警结束后必须回到 ARMED，而不是 DISARMED（见 protocol.md §8）
    check(msgs[-1][1]["guard"] == "ARMED", "告警结束后 guard 回到 ARMED")


# ---------------------------------------------------------------- 3. 端到端

def test_end_to_end() -> None:
    print("\n[3] 端到端：broker + 模拟设备 + 命令回执")

    port = 18899
    broker = M.Broker("127.0.0.1", port, verbose=False)
    threading.Thread(target=broker.serve_forever, daemon=True).start()
    time.sleep(0.3)

    msgs: list[tuple[str, dict]] = []
    panel = M.Client("127.0.0.1", port, client_id="selftest")
    panel.on_message = lambda t, p: msgs.append((t.rsplit("/", 1)[-1],
                                                 json.loads(p)))
    panel.connect()
    panel.subscribe([("bikehub/#", 1)])
    time.sleep(0.2)

    proc = subprocess.Popen(
        [sys.executable, str(HERE / "simulate_device.py"),
         "--scenario", "heartbeat", "--interval", "1", "--port", str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        end = time.time() + 2.5
        while time.time() < end:
            panel.loop(0.1)

        kinds = {k for k, _ in msgs}
        check({"state", "online", "loc"} <= kinds, "收到 state / online / loc")
        check(any(m.get("t") == "boot" for k, m in msgs if k == "evt"),
              "收到 boot 事件")

        def cmd(name: str, arg, cid: int) -> dict | None:
            obj = {"v": 1, "id": cid, "cmd": name}
            if arg is not None:
                obj["arg"] = arg
            panel.publish("bikehub/7cdfa1/cmd", json.dumps(obj), qos=1)
            deadline = time.time() + 3
            while time.time() < deadline:
                panel.loop(0.1)
                for k, m in msgs:
                    if k == "ack" and m.get("id") == cid:
                        return m
            return None

        cases = [
            ("play",   2,    101, True,  None),
            ("play",   99,   102, False, "no_such_sound"),
            ("play",   0,    109, False, "no_such_sound"),
            ("play",   8,    110, False, "alarm_absent"),
            ("stop",   None, 103, True,  None),
            ("status", None, 104, True,  None),
            ("lock",   None, 105, True,  None),
            ("lock",   None, 106, False, "locked"),
            ("unlock", None, 107, True,  None),
            ("reboot", None, 108, False, "bad_cmd"),
        ]
        for name, arg, cid, want_ok, want_err in cases:
            ack = cmd(name, arg, cid)
            label = f"{name} {arg if arg is not None else ''}".strip()
            if ack is None:
                check(False, f"{label} 收到 ack")
                continue
            ok = ack["ok"] == want_ok and (not want_err
                                           or ack.get("err") == want_err)
            check(ok, f"{label} → {'ok' if want_ok else 'err=' + str(ack.get('err'))}")

        states = [m for k, m in msgs if k == "state"]
        check(bool(states) and states[-1]["lock"] == "UNLOCKED",
              "unlock 后最终状态为 UNLOCKED")
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        panel.close()
        broker.stop()


# ---------------------------------------------------------------- main

def main() -> int:
    test_codec()
    test_dry_run()
    test_end_to_end()

    print()
    if FAILED:
        print(f"❌ {len(FAILED)} 项失败：")
        for f in FAILED:
            print(f"   - {f}")
        return 1
    print("✅ 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
