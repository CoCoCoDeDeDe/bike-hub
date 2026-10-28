#pragma once

/**
 * 极简 Arduino 桩，仅用于 native 单元测试。
 *
 * 只实现 guard / lock 用到的那些符号，不追求和真实 API 行为一致。
 * 时间是可控的：改 mock_millis() 就能让测试"快进"。
 */

#include <cstdint>
#include <cstddef>
#include <cstdarg>

// ---------------------------------------------------------------- 常量
#define HIGH 0x1
#define LOW  0x0

#define INPUT        0x0
#define OUTPUT       0x1
#define INPUT_PULLUP 0x2

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// ---------------------------------------------------------------- 可控时钟
inline uint32_t &mock_millis_ref() {
    static uint32_t t = 0;
    return t;
}

inline uint32_t millis() { return mock_millis_ref(); }
inline void delay(uint32_t) {}

// ---------------------------------------------------------------- 串口
struct MockSerial {
    void begin(unsigned long) {}
    void end() {}
    int available() { return 0; }
    int read() { return -1; }
    size_t write(uint8_t) { return 1; }
    size_t print(const char *) { return 0; }
    size_t println(const char *) { return 0; }
    size_t println() { return 0; }
    int printf(const char *, ...) { return 0; }
};

inline MockSerial Serial;

// ---------------------------------------------------------------- GPIO
inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
inline int  digitalRead(uint8_t) { return LOW; }

inline int  analogRead(uint8_t) { return 0; }
inline void analogReadResolution(uint8_t) {}

// ---------------------------------------------------------------- LEDC
inline bool ledcAttach(uint8_t, uint32_t, uint8_t) { return true; }
inline void ledcWrite(uint8_t, uint32_t) {}

// ---------------------------------------------------------------- Client / Stream / Print
// 签名按 ESP32 Arduino core 的 Client.h 抄，modem 模块要能 override。
// 别在这里偷偷加默认实现——真实环境是纯虚的，加了会在 check-real 才炸。
class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buf, size_t size) {
        size_t n = 0;
        while (size--) n += write(*buf++);
        return n;
    }
};

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int read(uint8_t *buf, size_t size) = 0;
    virtual int peek() = 0;
    virtual void flush() {}
};

class IPAddress {
public:
    uint32_t v = 0;
    uint8_t operator[](int i) const {
        return static_cast<uint8_t>((v >> (24 - i * 8)) & 0xFF);
    }
};

class Client : public Stream {
public:
    virtual int connect(IPAddress ip, uint16_t port) = 0;
    virtual int connect(const char *host, uint16_t port) = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buf, size_t size) = 0;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int read(uint8_t *buf, size_t size) = 0;
    virtual int peek() = 0;
    virtual void flush() {}
    virtual void stop() = 0;
    virtual uint8_t connected() = 0;
    virtual operator bool() = 0;
};

// ---------------------------------------------------------------- 复位原因
enum esp_reset_reason_t {
    ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO
};

inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
