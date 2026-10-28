#include "modem.h"

#include <stdio.h>
#include <string.h>

namespace {

/// 从 "+IPD,<len>" 或 "+IPD,<id>,<len>" 里取长度。
/// 模组不同固件两种格式都有，所以两种都得认。
bool parseIpdHeader(const char *s, uint32_t &out) {
    if (strncmp(s, "+IPD,", 5) != 0) return false;

    const char *p = s + 5;
    uint32_t first = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        first = first * 10u + uint32_t(*p - '0');
        ++p;
        ++digits;
        if (digits > 9) return false;      // 明显不是长度
    }
    if (digits == 0) return false;

    if (*p == ',') {                       // 带连接 ID
        ++p;
        uint32_t second = 0;
        digits = 0;
        while (*p >= '0' && *p <= '9') {
            second = second * 10u + uint32_t(*p - '0');
            ++p;
            ++digits;
            if (digits > 9) return false;
        }
        if (digits == 0) return false;
        out = second;
    } else {
        out = first;
    }
    return true;
}

}  // namespace

const char *modemErrName(ModemErr e) {
    switch (e) {
        case ModemErr::NONE:          return "NONE";
        case ModemErr::TIMEOUT:       return "TIMEOUT";
        case ModemErr::CMD_ERROR:     return "CMD_ERROR";
        case ModemErr::CONNECT_FAIL:  return "CONNECT_FAIL";
        case ModemErr::SEND_FAIL:     return "SEND_FAIL";
        case ModemErr::NOT_CONNECTED: return "NOT_CONNECTED";
        case ModemErr::TOO_LONG:      return "TOO_LONG";
        case ModemErr::NO_PROMPT:     return "NO_PROMPT";
    }
    return "?";
}

// ---------------------------------------------------------------- 构造

Air780Client::Air780Client(AtStream &stream, uint32_t timeout_ms)
    : stream_(stream), timeout_ms_(timeout_ms) {}

void Air780Client::resetForTest() {
    link_ = Link::CLOSED;
    err_ = ModemErr::NONE;
    resp_ = Resp::NONE;
    line_len_ = 0;
    line_[0] = '\0';
    after_prompt_ = false;
    ipd_left_ = 0;
    rx_head_ = 0;
    rx_count_ = 0;
}

void Air780Client::resetParser() {
    line_len_ = 0;
    line_[0] = '\0';
    resp_ = Resp::NONE;
    after_prompt_ = false;
}

// ---------------------------------------------------------------- 发送

void Air780Client::sendRaw(const char *s) {
    size_t n = strlen(s);
    if (n == 0) return;
    stream_.write(reinterpret_cast<const uint8_t *>(s), n);
}

// ---------------------------------------------------------------- 连接

int Air780Client::connect(const char *host, uint16_t port) {
    if (!host) {
        err_ = ModemErr::CONNECT_FAIL;
        return 0;
    }

    if (link_ == Link::CONNECTED) stop();

    resetParser();
    err_ = ModemErr::NONE;

    char cmd[96];
    int n = snprintf(cmd, sizeof(cmd),
                     "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", host,
                     static_cast<unsigned>(port));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(cmd)) {
        err_ = ModemErr::CONNECT_FAIL;
        return 0;
    }
    sendRaw(cmd);

    // 先收 "OK"（命令受理），再收 "CONNECT OK"（真正连上）。
    // 部分固件只回 CONNECT OK，所以 OK 超时不算失败、继续等；
    // 但真的回 ERROR 必须立刻报，不能吞掉再等到超时。
    ModemErr e = waitFor(Resp::OK, timeout_ms_);
    if (e == ModemErr::CMD_ERROR || e == ModemErr::CONNECT_FAIL) {
        err_ = e;
        link_ = Link::CLOSED;
        return 0;
    }

    e = waitFor(Resp::CONNECT_OK, timeout_ms_ * 2);
    if (e != ModemErr::NONE) {
        err_ = e;
        link_ = Link::CLOSED;
        return 0;
    }

    link_ = Link::CONNECTED;
    return 1;
}

int Air780Client::connect(IPAddress ip, uint16_t port) {
    // 用 operator[] 而不是拆内部字段：ESP32 的 IPAddress 没有 .v，
    // 但 [0..3] 在 Arduino 各家里都是稳定接口
    char host[24];
    snprintf(host, sizeof(host), "%u.%u.%u.%u",
             static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
             static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
    return connect(host, port);
}

void Air780Client::stop() {
    if (link_ == Link::CONNECTED) {
        sendRaw("AT+CIPCLOSE\r\n");
        // 不必等回应：关连接是尽力而为，卡住反而耽误重连
        waitFor(Resp::OK, timeout_ms_ / 3);
    }
    link_ = Link::CLOSED;
    ipd_left_ = 0;
    resetParser();
}

uint8_t Air780Client::connected() {
    poll();                    // 顺手收掉 CLOSED 之类的 URC
    return link_ == Link::CONNECTED ? 1 : 0;
}

Air780Client::operator bool() { return connected() != 0; }

// ---------------------------------------------------------------- 写

size_t Air780Client::write(uint8_t b) { return write(&b, 1); }

size_t Air780Client::write(const uint8_t *buf, size_t size) {
    if (link_ != Link::CONNECTED) {
        err_ = ModemErr::NOT_CONNECTED;
        return 0;
    }
    if (!buf || size == 0) return 0;
    if (size > SEND_MAX) {
        err_ = ModemErr::TOO_LONG;
        return 0;
    }

    resetParser();

    char cmd[32];
    int n = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u\r\n",
                     static_cast<unsigned>(size));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(cmd)) {
        err_ = ModemErr::TOO_LONG;
        return 0;
    }
    sendRaw(cmd);

    // 等 '>' 提示符。等到之前可能先回 SEND OK（异常），交给 waitFor 判。
    ModemErr e = waitFor(Resp::PROMPT, timeout_ms_);
    if (e != ModemErr::NONE) {
        if (e == ModemErr::CMD_ERROR) {
            err_ = ModemErr::CMD_ERROR;
        } else {
            err_ = ModemErr::NO_PROMPT;
        }
        return 0;
    }

    stream_.write(buf, size);

    e = waitFor(Resp::SEND_OK, timeout_ms_);
    if (e != ModemErr::NONE) {
        err_ = (e == ModemErr::CMD_ERROR) ? ModemErr::CMD_ERROR
                                          : ModemErr::SEND_FAIL;
        return 0;
    }

    return size;
}

// ---------------------------------------------------------------- 读

void Air780Client::poll() {
    // 一次最多吃这么多，避免模组狂刷数据时主循环饿死
    int budget = 512;
    while (budget-- > 0 && stream_.available() > 0) {
        int c = stream_.read();
        if (c < 0) break;
        feed(static_cast<uint8_t>(c));
    }
}

int Air780Client::available() {
    poll();
    return static_cast<int>(rx_count_);
}

int Air780Client::read() {
    poll();
    if (rx_count_ == 0) return -1;
    uint8_t b = rx_[rx_head_];
    rx_head_ = (rx_head_ + 1) % RX_CAP;
    --rx_count_;
    return b;
}

int Air780Client::read(uint8_t *buf, size_t size) {
    if (!buf || size == 0) return 0;
    poll();
    size_t n = 0;
    while (n < size && rx_count_ > 0) {
        buf[n++] = rx_[rx_head_];
        rx_head_ = (rx_head_ + 1) % RX_CAP;
        --rx_count_;
    }
    return static_cast<int>(n);
}

int Air780Client::peek() {
    poll();
    if (rx_count_ == 0) return -1;
    return rx_[rx_head_];
}

void Air780Client::flush() {
    // 没有发送缓冲，写出去就等于发出去了
}

// ---------------------------------------------------------------- 解析

void Air780Client::pushRx(uint8_t b) {
    if (rx_count_ >= RX_CAP) {
        // 满了就丢最老的：宁可丢一帧，也不要卡死整条链路
        rx_head_ = (rx_head_ + 1) % RX_CAP;
        --rx_count_;
    }
    rx_[ (rx_head_ + rx_count_) % RX_CAP ] = b;
    ++rx_count_;
}

void Air780Client::feed(uint8_t b) {
    // 数据段里一切都算 payload，'\r' '\n' 也不例外
    if (ipd_left_ > 0) {
        pushRx(b);
        --ipd_left_;
        return;
    }

    if (b == '\r') return;                      // 行尾归一，只认 '\n'

    // 提示符单独处理：模组发的是 "\r\n> "，后面跟一个空格。
    // 不把这个后缀吃掉，下一行会变成 " SEND OK"，永远匹配不上；
    // 但又不能死等 '\n' —— 有的固件 "> " 后面直接就是下一帧。
    // 所以：空格和换行照吃，遇到第一个正经字符就收手，把它交给正常解析。
    if (after_prompt_) {
        if (b == ' ' || b == '\r' || b == '\n') return;
        after_prompt_ = false;
    }
    if (b == '>' && line_len_ == 0) {
        resp_ = Resp::PROMPT;
        after_prompt_ = true;
        return;
    }

    if (line_len_ >= LINE_CAP - 1) {
        // 超长行（正常不该出现）直接丢弃，别把后面的好数据带偏
        line_len_ = 0;
        line_[0] = '\0';
        return;
    }
    line_[line_len_++] = static_cast<char>(b);

    // +IPD 的头以 ':' 结尾，见到冒号立刻切成数据段
    if (b == ':') {
        line_[line_len_] = '\0';
        uint32_t len = 0;
        if (parseIpdHeader(line_, len) && len > 0) {
            ipd_left_ = len;
            line_len_ = 0;
            line_[0] = '\0';
            return;
        }
        // 不是 +IPD，当普通行继续攒
    }

    if (b == '\n') {
        // 空行（比如 "\r\n" 里的那一下）不能去碰 line_[-1]，
        // 那会踩到上一个成员 resp_ 上
        if (line_len_ > 0) {
            line_[line_len_ - 1] = '\0';        // 去掉 '\n'
            handleLine(line_);
            line_len_ = 0;
            line_[0] = '\0';
        }
    }
}

void Air780Client::handleLine(const char *line) {
    if (line[0] == '\0') return;

    if      (strcmp(line, "OK") == 0)            resp_ = Resp::OK;
    else if (strcmp(line, "ERROR") == 0)         resp_ = Resp::ERROR;
    else if (strcmp(line, "CONNECT OK") == 0)    resp_ = Resp::CONNECT_OK;
    else if (strcmp(line, "CONNECT FAIL") == 0)  resp_ = Resp::CONNECT_FAIL;
    else if (strcmp(line, "ALREADY CONNECT") == 0) resp_ = Resp::CONNECT_OK;
    else if (strcmp(line, "SEND OK") == 0)       resp_ = Resp::SEND_OK;
    else if (strcmp(line, "SEND FAIL") == 0)     resp_ = Resp::SEND_FAIL;
    else if (strcmp(line, "CLOSED") == 0) {
        link_ = Link::CLOSED;
        resp_ = Resp::CLOSED;
    }
    // 其它行（模组启动横幅、URC、"Recv N bytes"）不影响状态
}

ModemErr Air780Client::waitFor(Resp want, uint32_t timeout_ms) {
    const uint32_t t0 = millis();
    // 至少跑一轮：即使超时设成 0，也要把已经躺在串口里的数据吃掉
    do {
        poll();
        if (resp_ == want) {
            resp_ = Resp::NONE;
            return ModemErr::NONE;
        }
        switch (resp_) {
            case Resp::ERROR:
                resp_ = Resp::NONE;
                return ModemErr::CMD_ERROR;
            case Resp::CONNECT_FAIL:
                resp_ = Resp::NONE;
                return ModemErr::CONNECT_FAIL;
            case Resp::SEND_FAIL:
                resp_ = Resp::NONE;
                return ModemErr::SEND_FAIL;
            case Resp::CLOSED:
                resp_ = Resp::NONE;
                return ModemErr::NOT_CONNECTED;
            default:
                break;
        }
        if (timeout_ms == 0) break;
    } while (static_cast<uint32_t>(millis() - t0) < timeout_ms);

    return ModemErr::TIMEOUT;
}
