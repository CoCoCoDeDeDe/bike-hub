#!/usr/bin/env python3
"""最小 MQTT 3.1.1 实现（客户端 + broker），只用标准库。

为什么不用 paho-mqtt：
    这台机器上没有 pip，`uv` 也不在默认 PATH 里。一个调试工具不该
    因为装包失败就跑不起来。MQTT 3.1.1 的 CONNECT / PUBLISH /
    SUBSCRIBE / PING 加起来不到 400 行，够本项目用。

支持范围（够用就行，不是完整实现）：
    ✅ CONNECT / CONNACK（含用户名密码、clean session）
    ✅ PUBLISH / 转发（QoS 0；QoS 1 会回 PUBACK，转发降级为 QoS 0）
    ✅ SUBSCRIBE / SUBACK（支持通配符 + 与 #）
    ✅ PINGREQ / PINGRESP / DISCONNECT
    ❌ 遗嘱消息（will）、retained 跨进程保留、TLS

broker 的 retained 是内存级的：进程活着就有效，退出即丢。
用于本地联调足够。
"""

from __future__ import annotations

import select
import socket
import struct
import sys
import time

# ---------------------------------------------------------------- 类型码

CONNECT = 1
CONNACK = 2
PUBLISH = 3
PUBACK = 4
SUBSCRIBE = 8
SUBACK = 9
UNSUBSCRIBE = 10
UNSUBACK = 11
PINGREQ = 12
PINGRESP = 13
DISCONNECT = 14

MAX_PACKET = 1 << 20        # 单包上限 1 MB，防呆
DEFAULT_PORT = 1883


# ---------------------------------------------------------------- 编码

def encode_length(n: int) -> bytes:
    """MQTT 剩余长度是 7 位一组的变长编码。"""
    if n < 0 or n > 268_435_455:
        raise ValueError(f"长度超范围: {n}")
    out = bytearray()
    while True:
        digit = n % 128
        n //= 128
        if n > 0:
            digit |= 0x80
        out.append(digit)
        if n == 0:
            return bytes(out)


def encode_str(s: str | bytes) -> bytes:
    """长度前缀（2 字节大端）+ UTF-8。"""
    b = s.encode("utf-8") if isinstance(s, str) else s
    return struct.pack("!H", len(b)) + b


def packet(ptype: int, flags: int, body: bytes) -> bytes:
    return bytes([(ptype << 4) | (flags & 0x0F)]) + encode_length(len(body)) + body


def build_connect(client_id: str, username: str | None = None,
                  password: str | None = None, keepalive: int = 60,
                  clean: bool = True) -> bytes:
    flags = 0x02 if clean else 0x00
    if username is not None:
        flags |= 0x80
    if password is not None:
        flags |= 0x40

    body = encode_str("MQTT") + bytes([4, flags])
    body += struct.pack("!H", keepalive)
    body += encode_str(client_id)
    if username is not None:
        body += encode_str(username)
    if password is not None:
        body += encode_str(password)
    return packet(CONNECT, 0, body)


def build_publish(topic: str, payload: bytes, qos: int = 0,
                  retain: bool = False, packet_id: int = 0) -> bytes:
    flags = (qos & 0x03) << 1
    if retain:
        flags |= 0x01
    body = encode_str(topic)
    if qos > 0:
        body += struct.pack("!H", packet_id)
    body += payload
    return packet(PUBLISH, flags, body)


def build_subscribe(packet_id: int, topics: list[tuple[str, int]]) -> bytes:
    body = struct.pack("!H", packet_id)
    for t, q in topics:
        body += encode_str(t) + bytes([q & 0x03])
    return packet(SUBSCRIBE, 0x02, body)      # SUBSCRIBE 固定 flags = 0b0010


def build_puback(packet_id: int) -> bytes:
    return packet(PUBACK, 0, struct.pack("!H", packet_id))


PINGREQ_BYTES = bytes([PINGREQ << 4, 0])
PINGRESP_BYTES = bytes([PINGRESP << 4, 0])
DISCONNECT_BYTES = bytes([DISCONNECT << 4, 0])


def topic_matches(pattern: str, topic: str) -> bool:
    """MQTT 通配符：+ 匹配一层，# 匹配剩余所有层（必须在末尾）。"""
    if pattern == topic:
        return True
    p_parts = pattern.split("/")
    t_parts = topic.split("/")
    for i, p in enumerate(p_parts):
        if p == "#":
            return True
        if i >= len(t_parts):
            return False
        if p == "+":
            continue
        if p != t_parts[i]:
            return False
    return len(p_parts) == len(t_parts)


# ---------------------------------------------------------------- 拆包

def parse_publish(flags: int, body: bytes):
    """→ (topic, payload, qos, retain, packet_id)"""
    qos = (flags >> 1) & 0x03
    retain = bool(flags & 0x01)
    tlen = struct.unpack("!H", body[:2])[0]
    topic = body[2:2 + tlen].decode("utf-8", "replace")
    off = 2 + tlen
    pid = 0
    if qos > 0:
        pid = struct.unpack("!H", body[off:off + 2])[0]
        off += 2
    return topic, body[off:], qos, retain, pid


class Reader:
    """把 socket 上的字节流切成 MQTT 帧。

    缓冲与读 socket 是分开的：broker 走"非阻塞 feed + try_parse"，
    client 走"阻塞 read"。两者共用同一个解析函数，避免两套逻辑。
    """

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = bytearray()
        self.closed = False

    # -- 非阻塞（broker 用） -------------------------------------------

    def feed(self) -> int:
        """把可读数据收进缓冲。返回字节数，-1 表示对端关闭。"""
        try:
            chunk = self.sock.recv(4096)
        except (BlockingIOError, socket.timeout, TimeoutError):
            return 0
        except OSError:
            self.closed = True
            return -1
        if not chunk:
            self.closed = True
            return -1
        self.buf += chunk
        return len(chunk)

    def try_parse(self):
        """缓冲里已有完整包就返回 (type, flags, body)，否则 None。"""
        if len(self.buf) < 2:
            return None

        i = 1
        mult, length = 1, 0
        while True:
            if i >= len(self.buf):
                return None                      # 长度字段还没收全
            d = self.buf[i]
            length += (d & 0x7F) * mult
            i += 1
            if length > MAX_PACKET:
                raise ValueError("包过大")
            if not (d & 0x80):
                break
            mult *= 128
            if mult > 128 ** 3:
                raise ValueError("剩余长度编码非法")

        total = i + length
        if len(self.buf) < total:
            return None                          # 正文还没收全

        head = self.buf[0]
        body = bytes(self.buf[i:total])
        del self.buf[:total]
        return (head >> 4, head & 0x0F, body)

    # -- 阻塞（client 用） ---------------------------------------------

    def read(self, timeout: float = 1.0):
        """阻塞直到拿到一个完整包，或超时/断开返回 None。"""
        deadline = time.monotonic() + timeout
        while True:
            pkt = self.try_parse()
            if pkt is not None:
                return pkt
            if self.closed:
                return None

            remain = deadline - time.monotonic()
            if remain <= 0:
                return None

            self.sock.settimeout(remain)
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, TimeoutError):
                return None
            except OSError:
                self.closed = True
                return None
            if not chunk:
                self.closed = True
                return None
            self.buf += chunk


# ---------------------------------------------------------------- 客户端

class Client:
    """阻塞式最小客户端。用法见 tools/simulate_device.py。"""

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT,
                 client_id: str = "", username: str | None = None,
                 password: str | None = None, keepalive: int = 60):
        self.host, self.port = host, port
        self.client_id = client_id or f"lite-{int(time.time() * 1000) % 100000}"
        self.username, self.password = username, password
        self.keepalive = keepalive
        self.sock: socket.socket | None = None
        self.reader: Reader | None = None
        self._pid = 0
        self._last_ping = 0.0
        self.on_message = None       # fn(topic: str, payload: bytes)

    # -- 连接 ----------------------------------------------------------

    def connect(self, timeout: float = 5.0) -> bool:
        self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self.reader = Reader(self.sock)
        self.sock.sendall(build_connect(self.client_id, self.username,
                                        self.password, self.keepalive))
        pkt = self.reader.read(timeout)
        if not pkt:
            raise ConnectionError("CONNACK 超时（broker 没响应）")
        ptype, _flags, body = pkt
        if ptype != CONNACK:
            raise ConnectionError(f"期望 CONNACK，收到 {ptype}")
        if len(body) >= 2 and body[1] != 0:
            raise ConnectionError(f"broker 拒绝连接，返回码 {body[1]}")
        self._last_ping = time.monotonic()
        return True

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.sendall(DISCONNECT_BYTES)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    # -- 收发 ----------------------------------------------------------

    def publish(self, topic: str, payload: str | bytes, qos: int = 0,
                retain: bool = False) -> None:
        if not self.sock:
            raise ConnectionError("未连接")
        raw = payload.encode("utf-8") if isinstance(payload, str) else payload
        pid = self._next_pid() if qos > 0 else 0
        self.sock.sendall(build_publish(topic, raw, qos, retain, pid))

    def subscribe(self, topics: list[tuple[str, int]]) -> None:
        if not self.sock:
            raise ConnectionError("未连接")
        self.sock.sendall(build_subscribe(self._next_pid(), topics))

    def loop(self, timeout: float = 1.0) -> bool:
        """处理一次收包。返回 False 表示连接已断。"""
        if not self.reader:
            return False
        self._maybe_ping()
        pkt = self.reader.read(timeout)
        if pkt is None:
            return not self.reader.closed
        ptype, flags, body = pkt
        if ptype == PUBLISH:
            topic, payload, qos, _retain, pid = parse_publish(flags, body)
            if qos == 1 and self.sock:
                self.sock.sendall(build_puback(pid))
            if self.on_message:
                self.on_message(topic, payload)
        return True

    def _next_pid(self) -> int:
        self._pid = self._pid % 65535 + 1
        return self._pid

    def _maybe_ping(self) -> None:
        if self.keepalive and time.monotonic() - self._last_ping > self.keepalive / 2:
            if self.sock:
                self.sock.sendall(PINGREQ_BYTES)
            self._last_ping = time.monotonic()


# ---------------------------------------------------------------- broker

class Broker:
    """单线程 select 事件循环的极简 broker。"""

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT,
                 verbose: bool = True):
        self.host, self.port = host, port
        self.verbose = verbose
        self.conns: dict[socket.socket, Reader] = {}
        self.subs: dict[socket.socket, set[str]] = {}
        self.retained: dict[str, bytes] = {}
        self._running = False
        self._srv: socket.socket | None = None

    def log(self, msg: str) -> None:
        if self.verbose:
            print(f"[broker] {msg}", flush=True)

    def serve_forever(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.port))
        srv.listen(8)
        srv.setblocking(False)
        self._srv = srv
        self._running = True
        self.log(f"监听 {self.host}:{self.port}")
        try:
            while self._running:
                rlist = [srv] + list(self.conns)
                try:
                    readable, _, _ = select.select(rlist, [], [], 0.5)
                except (InterruptedError, OSError):
                    continue
                for s in readable:
                    if s is srv:
                        self._accept(srv)
                    else:
                        self._handle(s)
        except KeyboardInterrupt:
            self.log("收到 Ctrl-C，退出")
        finally:
            for s in list(self.conns):
                try:
                    s.close()
                except OSError:
                    pass
            srv.close()
            self._srv = None

    def stop(self) -> None:
        self._running = False

    # -- 内部 ----------------------------------------------------------

    def _accept(self, srv: socket.socket) -> None:
        try:
            conn, addr = srv.accept()
        except OSError:
            return
        conn.setblocking(False)
        self.conns[conn] = Reader(conn)
        self.subs[conn] = set()
        self.log(f"连接 {addr[0]}:{addr[1]}（当前 {len(self.conns)} 个）")

    def _drop(self, conn: socket.socket) -> None:
        self.conns.pop(conn, None)
        self.subs.pop(conn, None)
        try:
            conn.close()
        except OSError:
            pass
        self.log(f"断开（剩 {len(self.conns)} 个）")

    def _handle(self, conn: socket.socket) -> None:
        reader = self.conns.get(conn)
        if not reader:
            return
        if reader.feed() < 0:
            self._drop(conn)
            return

        while True:
            try:
                pkt = reader.try_parse()
            except (ValueError, OSError):
                self._drop(conn)
                return
            if pkt is None:
                return
            if not self._dispatch(conn, pkt):
                return

    def _dispatch(self, conn: socket.socket, pkt) -> bool:
        ptype, flags, body = pkt
        try:
            if ptype == CONNECT:
                conn.sendall(packet(CONNACK, 0, bytes([0, 0])))
                self.log("CONNACK")

            elif ptype == PUBLISH:
                topic, payload, qos, retain, pid = parse_publish(flags, body)
                if qos == 1:
                    conn.sendall(build_puback(pid))
                if retain:
                    if payload:
                        self.retained[topic] = payload
                    else:
                        self.retained.pop(topic, None)
                self.log(f"PUB {topic} ({len(payload)} B, qos{qos}"
                         f"{', retained' if retain else ''})")
                self._broadcast(topic, payload)

            elif ptype == SUBSCRIBE:
                pid = struct.unpack("!H", body[:2])[0]
                off, granted = 2, []
                while off + 2 <= len(body):
                    tlen = struct.unpack("!H", body[off:off + 2])[0]
                    t = body[off + 2:off + 2 + tlen].decode("utf-8", "replace")
                    q = body[off + 2 + tlen] if off + 2 + tlen < len(body) else 0
                    off += 3 + tlen
                    self.subs[conn].add(t)
                    granted.append(min(q, 1))
                    self.log(f"SUB {t}")
                    for rt, payload in self.retained.items():
                        if topic_matches(t, rt):
                            conn.sendall(build_publish(rt, payload, 0, True))
                conn.sendall(packet(SUBACK, 0, struct.pack("!H", pid) + bytes(granted)))

            elif ptype == UNSUBSCRIBE:
                pid = struct.unpack("!H", body[:2])[0]
                off = 2
                while off + 2 <= len(body):
                    tlen = struct.unpack("!H", body[off:off + 2])[0]
                    t = body[off + 2:off + 2 + tlen].decode("utf-8", "replace")
                    off += 2 + tlen
                    self.subs[conn].discard(t)
                conn.sendall(packet(UNSUBACK, 0, struct.pack("!H", pid)))

            elif ptype == PINGREQ:
                conn.sendall(PINGRESP_BYTES)

            elif ptype == DISCONNECT:
                self._drop(conn)
                return False
        except OSError:
            self._drop(conn)
            return False
        return True

    def _broadcast(self, topic: str, payload: bytes) -> None:
        for conn, patterns in list(self.subs.items()):
            if any(topic_matches(p, topic) for p in patterns):
                try:
                    conn.sendall(build_publish(topic, payload, 0))
                except OSError:
                    self._drop(conn)


# ---------------------------------------------------------------- CLI

def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description="最小 MQTT broker（仅标准库）")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("-q", "--quiet", action="store_true", help="不打印转发日志")
    args = ap.parse_args()
    Broker(args.host, args.port, verbose=not args.quiet).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
