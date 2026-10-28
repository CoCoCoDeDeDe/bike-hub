#pragma once

/**
 * Air780 系列 4G 模组的 MQTT 传输层
 *
 * 职责：把 AT 指令的 TCP 透传，伪装成 Arduino 的 Client，
 * 好让 PubSubClient 之类的 MQTT 库直接拿来用。
 *
 *   PubSubClient ──► Air780Client ──► AtStream ──► UART ──► 模组 ──► TCP
 *
 * 为什么不用模组自带的 AT+MQTTCONN：
 *   ESP32 侧能抓每个字节、能自己控重连、换模组只改 AT 命令。
 *   理由见 docs/prototype-plan.md §3.2。
 *
 * 关键：AT 交互是"发命令 → 等回显行"的同步模型，不是 socket 语义。
 * 所以这里必须做状态机：
 *
 *   AT+CIPSTART="TCP","host",1883   →  OK  →  CONNECT OK
 *   AT+CIPSEND=42                   →  '>'  →  发 42 字节  →  SEND OK
 *   +IPD,42:{"v":1,...}             →  数据段按长度取，不按行
 *
 * 时间用 millis()，测试时靠 mock_millis_ref() 控制，所以能在电脑上跑。
 */

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

/// AT 串口抽象。真实环境包 HardwareSerial，测试注入假数据。
///
/// 只保留三个方法——能把状态机喂饱就行，多余的方法会让 mock 变重。
class AtStream {
public:
    virtual ~AtStream() = default;
    virtual void write(const uint8_t *p, size_t n) = 0;
    virtual int  available() = 0;   // 可读字节数
    virtual int  read() = 0;        // -1 表示无数据
};

/// 失败原因，落到日志里能直接看出卡在哪一步
enum class ModemErr : uint8_t {
    NONE = 0,
    TIMEOUT,        // 等回应超时
    CMD_ERROR,      // 模组回 ERROR
    CONNECT_FAIL,   // CONNECT FAIL / 注网失败
    SEND_FAIL,      // SEND FAIL
    NOT_CONNECTED,  // 未连上就收发
    TOO_LONG,       // 单次写入超过模组上限
    NO_PROMPT,      // CIPSEND 后没等到 '>'
};

const char *modemErrName(ModemErr e);

class Air780Client : public Client {
public:
    /// 接收环形缓冲。state 消息最大约 180 B，512 够撑一阵。
    static constexpr size_t RX_CAP   = 512;
    /// 单行上限。AT 回应行都很短，128 够；+IPD 头也在这条线内。
    static constexpr size_t LINE_CAP = 128;
    /// AT+CIPSEND 单次上限（模组一般 2048 或 1460）
    static constexpr size_t SEND_MAX = 1460;

    explicit Air780Client(AtStream &stream, uint32_t timeout_ms = 3000);

    // ---------------------------------------------------------- Client 接口
    int      connect(const char *host, uint16_t port) override;
    int      connect(IPAddress ip, uint16_t port) override;
    size_t   write(uint8_t b) override;
    size_t   write(const uint8_t *buf, size_t size) override;
    int      available() override;
    int      read() override;
    int      read(uint8_t *buf, size_t size) override;
    int      peek() override;
    void     flush() override;
    void     stop() override;
    uint8_t  connected() override;
    operator bool() override;

    // ---------------------------------------------------------- 诊断
    ModemErr lastError() const { return err_; }
    /// 已收到但还没被读走的字节数（调试用）
    size_t   rxPending() const { return rx_count_; }
    /// 把串口上现成的字节全吃进来，处理 URC 与 +IPD。非阻塞。
    void     poll();

    /// 测试用：清空所有内部状态，不用重新构造
    void     resetForTest();

private:
    enum class Link : uint8_t { CLOSED, CONNECTED };
    enum class Resp : uint8_t {
        NONE = 0, OK, ERROR, CONNECT_OK, CONNECT_FAIL, SEND_OK, SEND_FAIL,
        PROMPT, CLOSED
    };

    void   sendRaw(const char *s);
    void   feed(uint8_t b);
    void   handleLine(const char *line);
    ModemErr waitFor(Resp want, uint32_t timeout_ms);
    void   pushRx(uint8_t b);
    void   resetParser();

    AtStream &stream_;
    uint32_t  timeout_ms_;

    Link    link_ = Link::CLOSED;
    ModemErr err_ = ModemErr::NONE;
    Resp    resp_ = Resp::NONE;

    // 行解析
    char     line_[LINE_CAP] = {};
    size_t   line_len_ = 0;

    // +IPD 数据段：剩余字节数，>0 时所有输入都算数据
    uint32_t ipd_left_ = 0;

    // CIPSEND 的 '>' 之后跟着空格，要连换行一起吃干净
    bool     after_prompt_ = false;

    // 接收环形缓冲
    uint8_t  rx_[RX_CAP] = {};
    size_t   rx_head_ = 0;
    size_t   rx_count_ = 0;
};

/// 真实串口实现只在真机编译。native 测试不碰它。
/// 用 ARDUINO_ARCH_ESP32 判断而不是自定义宏：native 环境本来就没这个符号，
/// 少一个要维护的开关。
#if defined(ARDUINO_ARCH_ESP32)
#include <HardwareSerial.h>

class SerialAtStream : public AtStream {
public:
    explicit SerialAtStream(HardwareSerial &port) : port_(port) {}
    void write(const uint8_t *p, size_t n) override { port_.write(p, n); }
    int  available() override { return port_.available(); }
    int  read() override { return port_.read(); }

private:
    HardwareSerial &port_;
};
#endif
