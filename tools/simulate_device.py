#!/usr/bin/env python3
"""模拟 bike-hub 设备，按 docs/protocol.md 收发 MQTT 消息。

用途：在没有硬件的情况下先把云端链路（broker + 面板）跑通。
消息格式与 firmware/lib/protocol/ 严格一致，包括字段顺序与可选项。

两种模式：
    --dry-run    不连 broker，只把消息打到 stdout（零依赖，立刻能用）
    默认         连 broker，同时订阅 cmd 并回 ack

场景：
    boot      上电序列：online + boot 事件 + 首个 state + loc
    heartbeat 只发 state/loc 心跳（按 --interval）
    alarm     完整告警：alarm 事件 → guard=ALARM → 播放 → 回到 ARMED
    lock      锁舌动作：lock → unlock 各一次
    all       依次跑 boot / alarm / lock

例子：
    python3 tools/simulate_device.py --dry-run --scenario alarm
    python3 tools/mqtt_lite.py &                       # 先起 broker
    python3 tools/simulate_device.py --scenario all
"""

from __future__ import annotations

import argparse
import json
import signal
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mqtt_lite as M  # noqa: E402

TOPIC_ROOT = "bikehub"

# 命令白名单，与 docs/protocol.md §4 一致
CROPS = {"play", "stop", "status", "lock", "unlock"}

# 错误码，与 protocol.md §3.4 一致
ERR_BAD_JSON = "bad_json"
ERR_BAD_CMD = "bad_cmd"
ERR_NO_SOUND = "no_such_sound"
ERR_ALARM_ABSENT = "alarm_absent"
ERR_LOCKED = "locked"


def jdump(obj: dict) -> str:
    """紧凑 JSON，模仿 Arduino 的 serializeJson（无空格、不转义中文）。"""
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


class Device:
    """一台模拟的 bike-hub。"""

    def __init__(self, dev_id: str, dry_run: bool = False,
                 real_ts: bool = True, audio_ids: set[int] | None = None):
        self.dev_id = dev_id
        self.dry_run = dry_run
        self.real_ts = real_ts
        self.audio_ids = audio_ids if audio_ids is not None else {1, 2, 3, 4}

        self.seq = 0
        self.boot_ms = time.monotonic()

        # 与 StateView 默认值对齐
        self.lock = "UNKNOWN"
        self.guard = "INIT"
        self.light = "OFF"
        self.alarm = "IDLE"
        self.batt = 86
        self.volt = 3.95
        self.rssi = 18
        self.fw = "0.4.0"

        self.client: M.Client | None = None
        self._stop = False

    # ---------------------------------------------------------- 输出

    def ts(self) -> int:
        return int(time.time()) if self.real_ts else 0

    def uptime(self) -> int:
        return int(time.monotonic() - self.boot_ms)

    def _topic(self, leaf: str) -> str:
        return f"{TOPIC_ROOT}/{self.dev_id}/{leaf}"

    def _send(self, leaf: str, payload: dict, qos: int = 0,
              retain: bool = False) -> None:
        raw = jdump(payload)
        arrow = "→"
        tag = f"{'R' if retain else ' '}{qos}"
        print(f"{arrow} {leaf:<7} [{tag}] {raw}", flush=True)
        if self.dry_run:
            return
        assert self.client
        self.client.publish(self._topic(leaf), raw, qos=qos, retain=retain)

    # ---------------------------------------------------------- 上行

    def send_state(self, qos: int = 0, retain: bool = True) -> None:
        self._send("state", {
            "v": 1,
            "ts": self.ts(),
            "lock": self.lock,
            "guard": self.guard,
            "light": self.light,
            "alarm": self.alarm,
            "batt": self.batt,
            "volt": round(self.volt, 2),
            "rssi": self.rssi,
            "up": self.uptime(),
            "fw": self.fw,
        }, qos=qos, retain=retain)

    def send_event(self, t: str, rms: float | None = None,
                   with_lock: bool = False, **extra) -> None:
        self.seq += 1
        msg: dict = {"v": 1, "id": self.seq, "ts": self.ts(), "t": t}
        if rms is not None:
            msg["rms"] = round(rms, 2)
        if with_lock:
            msg["lock"] = self.lock
        msg.update(extra)
        self._send("evt", msg, qos=1)

    def send_loc(self, lat: float = 31.2304, lon: float = 121.4737,
                 acc: float = 8.5, spd: float | None = None) -> None:
        msg: dict = {"v": 1, "ts": self.ts(),
                     "lat": round(lat, 6), "lon": round(lon, 6), "acc": acc}
        if spd is not None:
            msg["spd"] = round(spd, 1)
        self._send("loc", msg, qos=0)

    def send_ack(self, cmd_id: int, ok: bool, err: str | None = None) -> None:
        msg: dict = {"v": 1, "id": cmd_id, "ok": ok}
        if not ok and err:
            msg["err"] = err
        self._send("ack", msg, qos=1)

    def send_online(self, online: bool = True) -> None:
        # 正常上线用 retained 的 {"online": true}；
        # 协议里的 build_offline() 是遗嘱消息，由 broker 代发，模拟器不发
        self._send("online", {"v": 1, "online": online}, qos=1, retain=True)

    # ---------------------------------------------------------- 下行

    def on_message(self, topic: str, payload: bytes) -> None:
        if not topic.endswith("/cmd"):
            return
        try:
            msg = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            print(f"← cmd      解析失败: {payload!r}", flush=True)
            self.send_ack(0, False, ERR_BAD_JSON)
            return

        cmd_id = int(msg.get("id", 0) or 0)
        cmd = str(msg.get("cmd", ""))
        arg = msg.get("arg", 0)
        print(f"← cmd      id={cmd_id} cmd={cmd} arg={arg}", flush=True)

        if cmd not in CROPS:
            self.send_ack(cmd_id, False, ERR_BAD_CMD)
            return

        if cmd == "play":
            self._do_play(cmd_id, int(arg or 0))
        elif cmd == "stop":
            self.alarm = "IDLE"
            self.send_ack(cmd_id, True)
            self.send_state()
        elif cmd == "status":
            self.send_ack(cmd_id, True)
            self.send_state()
        elif cmd == "lock":
            self._do_lock(cmd_id, want="LOCKED")
        elif cmd == "unlock":
            self._do_lock(cmd_id, want="UNLOCKED")

    def _do_play(self, cmd_id: int, song: int) -> None:
        if song < 1 or song > 9:
            self.send_ack(cmd_id, False, ERR_NO_SOUND)
            return
        if song not in self.audio_ids:
            self.send_ack(cmd_id, False, ERR_ALARM_ABSENT)
            return
        self.alarm = "PLAYING"
        self.send_ack(cmd_id, True)
        self.send_state()

    def _do_lock(self, cmd_id: int, want: str) -> None:
        if self.lock == want:
            self.send_ack(cmd_id, False, ERR_LOCKED)
            return
        self.lock = "MOVING"
        self.send_state()
        # 电机动作 400 ms（config.h 的 LOCK_MOTOR_MS）
        time.sleep(0.4)
        self.lock = want
        self.guard = "ARMED" if want == "LOCKED" else "DISARMED"
        self.send_event("lock" if want == "LOCKED" else "unlock")
        self.send_ack(cmd_id, True)
        self.send_state()

    # ---------------------------------------------------------- 场景

    def scenario_boot(self) -> None:
        self.lock, self.guard = "UNLOCKED", "DISARMED"
        self.send_online(True)
        self.send_event("boot", reason="power_on")
        self.send_state()
        self.send_loc()

    def scenario_alarm(self) -> None:
        self.lock, self.guard = "LOCKED", "ARMED"
        self.send_online(True)
        self.send_state()
        time.sleep(0.5)

        # 撬锁：振动超标
        self.send_event("alarm", rms=0.42, with_lock=True)
        self.guard, self.alarm = "ALARM", "PLAYING"
        self.send_state()
        time.sleep(2.0)

        # 播放结束，回到 ARMED —— 注意不是 DISARMED，锁还锁着
        self.alarm = "IDLE"
        self.guard = "ARMED"
        self.send_state()

    def scenario_lock(self) -> None:
        self.lock, self.guard = "UNLOCKED", "DISARMED"
        self.send_online(True)
        self.send_state()
        time.sleep(0.5)

        self.lock = "MOVING"
        self.send_state()
        time.sleep(0.4)
        self.lock, self.guard = "LOCKED", "ARMED"
        self.send_event("lock")
        self.send_state()
        time.sleep(0.8)

        self.lock = "MOVING"
        self.send_state()
        time.sleep(0.4)
        self.lock, self.guard = "UNLOCKED", "DISARMED"
        self.send_event("unlock")
        self.send_state()

    def heartbeat_loop(self, interval: float) -> None:
        tick = 0
        while not self._stop:
            if not self.dry_run and self.client:
                for _ in range(int(interval * 10)):
                    if self._stop:
                        break
                    if not self.client.loop(0.1):
                        print("✗ 连接断开", flush=True)
                        self._stop = True
                        break
            else:
                time.sleep(interval)

            tick += 1
            self.batt = max(20, 86 - tick // 10)
            self.volt = 3.95 - 0.01 * (tick // 10)
            self.send_state()
            if tick % 2 == 0:
                self.send_loc(spd=12.5 + tick % 5)

    # ---------------------------------------------------------- 运行

    def run(self, scenario: str, interval: float) -> int:
        if not self.dry_run:
            self.client = M.Client(broker_host, broker_port,
                                   client_id=f"bikehub-{self.dev_id}")
            self.client.on_message = self.on_message
            try:
                self.client.connect()
            except (ConnectionError, OSError) as e:
                print(f"✗ 连不上 broker: {e}", file=sys.stderr)
                print("  先用 python3 tools/mqtt_lite.py 起一个，或加 --dry-run",
                      file=sys.stderr)
                return 2
            print(f"✓ 已连 {broker_host}:{broker_port}  devId={self.dev_id}",
                  flush=True)
            self.client.subscribe([(self._topic("cmd"), 1)])
            for _ in range(5):
                self.client.loop(0.1)

        if scenario == "boot":
            self.scenario_boot()
        elif scenario == "alarm":
            self.scenario_alarm()
        elif scenario == "lock":
            self.scenario_lock()
        elif scenario == "heartbeat":
            self.scenario_boot()
        elif scenario == "all":
            self.scenario_boot()
            time.sleep(1.0)
            self.scenario_alarm()
            time.sleep(1.0)
            self.scenario_lock()
        else:
            print(f"未知场景 {scenario}", file=sys.stderr)
            return 2

        if scenario in ("heartbeat", "all"):
            print(f"\n心跳中，每 {interval}s 一次，Ctrl-C 退出", flush=True)
            self.heartbeat_loop(interval)

        if self.client:
            self.client.close()
        return 0


# ---------------------------------------------------------------- CLI

def scan_audio(repo_root: Path) -> set[int]:
    """扫 firmware/data 里真实存在的 aN.wav。

    特意不读 index.json：那里会把"计划要有但还没录"的编号也列进去，
    真实设备上 LittleFS.open 失败才是判据，模拟器要跟它一致。
    """
    data_dir = repo_root / "firmware" / "data"
    ids: set[int] = set()
    for p in sorted(data_dir.glob("a*.wav")):
        try:
            n = int(p.stem[1:])
        except ValueError:
            continue
        if 1 <= n <= 9:
            ids.add(n)
    if ids:
        print(f"✓ 找到音频 {sorted(ids)}（{data_dir.name}/）", flush=True)
    else:
        print(f"· {data_dir} 里没有 aN.wav，play 会一律回 alarm_absent",
              flush=True)
    return ids


broker_host, broker_port = "127.0.0.1", M.DEFAULT_PORT


def main() -> int:
    global broker_host, broker_port

    ap = argparse.ArgumentParser(
        description="模拟 bike-hub 设备（协议见 docs/protocol.md）")
    ap.add_argument("--dev-id", default="7cdfa1", help="MAC 后 6 位小写十六进制")
    ap.add_argument("--scenario", default="boot",
                    choices=["boot", "alarm", "lock", "heartbeat", "all"])
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=M.DEFAULT_PORT)
    ap.add_argument("--interval", type=float, default=60.0,
                    help="心跳间隔秒数（默认 60，调试时可给 2）")
    ap.add_argument("--dry-run", action="store_true",
                    help="不连 broker，只打印 JSON")
    ap.add_argument("--fake-ts", action="store_true",
                    help="ts 固定为 0（模拟设备没取到网络时间）")
    args = ap.parse_args()

    broker_host, broker_port = args.broker, args.port
    dev = Device(args.dev_id, dry_run=args.dry_run,
                 real_ts=not args.fake_ts,
                 audio_ids=scan_audio(Path(__file__).resolve().parent.parent))

    def on_sigint(_sig, _frm):
        dev._stop = True
        print("\n· 收到 Ctrl-C", flush=True)

    signal.signal(signal.SIGINT, on_sigint)
    return dev.run(args.scenario, args.interval)


if __name__ == "__main__":
    sys.exit(main())
