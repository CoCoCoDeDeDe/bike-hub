/**
 * Air780Client 单元测试（native，不需要硬件）
 *
 * 跑法：
 *   cd firmware && pio test -e native -f test_modem
 *
 * 测的是 AT 状态机，不是真模组：
 *   - +IPD 两种格式、分片、payload 里带 \r\n
 *   - CONNECT 成功 / 失败 / 超时
 *   - CIPSEND 的 '>' 握手、SEND OK / SEND FAIL
 *   - 环形缓冲溢出、CLOSED URC
 *
 * 时间用 mock_millis_ref() 推进。FakeModem 在串口空闲时每次 +10ms，
 * 模拟"真实时间里主循环在转"，否则 waitFor 会死等 —— 真机不会，
 * 因为 millis() 自己会走。
 */

#include <unity.h>

#include <string>
#include <vector>

#include "Arduino.h"
#include "modem.h"

// ---------------------------------------------------------------- 假模组

/// 既当"模组的串口输出"，也记录"MCU 发出去的字节"，
/// 并且按 AT 命令自动回话，省得每个用例手写握手。
class FakeModem : public AtStream {
public:
    enum class Mode {
        OK,             // 正常
        CONNECT_ERROR,  // CIPSTART 回 ERROR
        CONNECT_ONLY,   // 只回 CONNECT OK，不回 OK
        CONNECT_FAIL,   // CIPSTART 回 CONNECT FAIL
        NO_PROMPT,      // CIPSEND 回 ERROR，没有 '>'
        SEND_FAIL,      // 数据发完后回 SEND FAIL
        SILENT,         // 什么都不回（测超时）
    };

    Mode mode = Mode::OK;

    std::vector<std::string> payloads;   // 通过 CIPSEND 发出的数据
    std::string all_tx;                  // MCU 发过的所有字节

    void inject(const std::string &s) {
        rx_.insert(rx_.end(), s.begin(), s.end());
    }
    void injectRaw(const uint8_t *p, size_t n) {
        rx_.insert(rx_.end(), p, p + n);
    }

    // ---- AtStream ----
    void write(const uint8_t *p, size_t n) override {
        all_tx.append(reinterpret_cast<const char *>(p), n);
        for (size_t i = 0; i < n; ++i) onByte(static_cast<char>(p[i]));
    }

    int available() override {
        if (rx_pos_ >= rx_.size()) {
            mock_millis_ref() += 10;     // 空闲时推进时钟，等价于真机主循环在跑
            return 0;
        }
        return static_cast<int>(rx_.size() - rx_pos_);
    }

    int read() override {
        if (rx_pos_ >= rx_.size()) return -1;
        mock_millis_ref() += 1;
        return static_cast<uint8_t>(rx_[rx_pos_++]);
    }

    void reset() {
        rx_.clear();
        rx_pos_ = 0;
        payloads.clear();
        all_tx.clear();
        cmd_.clear();
        pending_.clear();
        waiting_data_ = false;
        expect_len_ = 0;
        mode = Mode::OK;
    }

private:
    void onByte(char b) {
        if (waiting_data_) {
            pending_.push_back(b);
            if (pending_.size() >= expect_len_) {
                payloads.push_back(pending_);
                pending_.clear();
                waiting_data_ = false;
                inject(mode == Mode::SEND_FAIL ? "SEND FAIL\r\n" : "SEND OK\r\n");
            }
            return;
        }
        cmd_.push_back(b);
        const size_t n = cmd_.size();
        if (n >= 2 && cmd_[n - 2] == '\r' && cmd_[n - 1] == '\n') {
            handleCmd(cmd_);
            cmd_.clear();
        }
    }

    void handleCmd(const std::string &c) {
        if (mode == Mode::SILENT) return;

        if (c.rfind("AT+CIPSTART", 0) == 0) {
            if (mode == Mode::CONNECT_ERROR) {
                inject("ERROR\r\n");
            } else if (mode == Mode::CONNECT_FAIL) {
                inject("\r\nOK\r\n\r\nCONNECT FAIL\r\n");
            } else if (mode == Mode::CONNECT_ONLY) {
                inject("CONNECT OK\r\n");
            } else {
                inject("\r\nOK\r\n\r\nCONNECT OK\r\n");
            }
            return;
        }

        if (c.rfind("AT+CIPSEND=", 0) == 0) {
            if (mode == Mode::NO_PROMPT) {
                inject("ERROR\r\n");
                return;
            }
            expect_len_ = static_cast<size_t>(std::stoul(c.substr(11)));
            waiting_data_ = true;
            inject("\r\n> ");
            return;
        }

        if (c.rfind("AT+CIPCLOSE", 0) == 0) {
            inject("\r\nOK\r\n");
        }
    }

    std::vector<char>       rx_;
    size_t                  rx_pos_ = 0;
    std::string             cmd_;
    std::string             pending_;
    bool                    waiting_data_ = false;
    size_t                  expect_len_ = 0;
};

// ---------------------------------------------------------------- 夹具

static FakeModem      modem;
static Air780Client  *client = nullptr;

void setUp(void) {
    mock_millis_ref() = 0;
    modem.reset();
    delete client;
    client = new Air780Client(modem, 500);
}

void tearDown(void) {
    delete client;
    client = nullptr;
}

/// 连上，作为多数用例的前置。
/// FakeModem 收到 CIPSTART 会自己回 OK + CONNECT OK，connect 会把它读干净，
/// 所以这里不用再手动 inject —— 混着灌数据会让响应顺序不可控。
static void bringUp() {
    modem.mode = FakeModem::Mode::OK;
    TEST_ASSERT_EQUAL(1, client->connect("broker.local", 1883));
}

// ---------------------------------------------------------------- +IPD

void test_ipd_short_format(void) {
    modem.inject("+IPD,5:hello");
    TEST_ASSERT_EQUAL(5, client->available());

    char buf[8] = {};
    TEST_ASSERT_EQUAL(5, client->read(reinterpret_cast<uint8_t *>(buf), 5));
    TEST_ASSERT_EQUAL_STRING("hello", buf);
    TEST_ASSERT_EQUAL(0, client->available());
}

void test_ipd_with_conn_id(void) {
    modem.inject("+IPD,0,5:world");
    TEST_ASSERT_EQUAL(5, client->available());

    char buf[8] = {};
    client->read(reinterpret_cast<uint8_t *>(buf), 5);
    TEST_ASSERT_EQUAL_STRING("world", buf);
}

void test_ipd_split_across_reads(void) {
    modem.inject("+IPD,5:he");
    TEST_ASSERT_EQUAL(2, client->available());     // 只到了 he

    modem.inject("llo");
    TEST_ASSERT_EQUAL(5, client->available());

    char buf[8] = {};
    client->read(reinterpret_cast<uint8_t *>(buf), 5);
    TEST_ASSERT_EQUAL_STRING("hello", buf);
}

void test_ipd_payload_keeps_crlf(void) {
    // payload 里的 \r\n 必须原样保留，不能被当成行尾
    modem.inject("+IPD,4:\r\n\r\n");
    TEST_ASSERT_EQUAL(4, client->available());

    uint8_t buf[4] = {};
    client->read(buf, 4);
    TEST_ASSERT_EQUAL_UINT8('\r', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('\n', buf[1]);
    TEST_ASSERT_EQUAL_UINT8('\r', buf[2]);
    TEST_ASSERT_EQUAL_UINT8('\n', buf[3]);
}

void test_ipd_payload_keeps_colon(void) {
    // 数据段里出现冒号不能再次触发 +IPD 头解析
    modem.inject("+IPD,3:a:b");
    char buf[4] = {};
    client->read(reinterpret_cast<uint8_t *>(buf), 3);
    TEST_ASSERT_EQUAL_STRING("a:b", buf);
}

void test_ipd_two_packets_back_to_back(void) {
    modem.inject("+IPD,2:ab+IPD,3:cde");
    TEST_ASSERT_EQUAL(5, client->available());

    char buf[6] = {};
    client->read(reinterpret_cast<uint8_t *>(buf), 5);
    TEST_ASSERT_EQUAL_STRING("abcde", buf);
}

void test_ipd_after_crlf_prefix(void) {
    // 真实模组会在前面带 \r\n
    modem.inject("\r\n+IPD,2:ok\r\n");
    TEST_ASSERT_EQUAL(2, client->available());
}

// ---------------------------------------------------------------- 连接

void test_connect_ok(void) {
    modem.mode = FakeModem::Mode::OK;
    TEST_ASSERT_EQUAL(1, client->connect("broker.local", 1883));
    TEST_ASSERT_TRUE(client->connected());
    TEST_ASSERT_EQUAL(ModemErr::NONE, client->lastError());
    // 命令格式要能被真模组认
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
        modem.all_tx.find("AT+CIPSTART=\"TCP\",\"broker.local\",1883\r\n"));
}

void test_connect_only_connect_ok(void) {
    // 有些固件不回中间的 OK
    modem.mode = FakeModem::Mode::CONNECT_ONLY;
    TEST_ASSERT_EQUAL(1, client->connect("10.0.0.1", 1883));
    TEST_ASSERT_TRUE(client->connected());
}

void test_connect_error(void) {
    modem.mode = FakeModem::Mode::CONNECT_ERROR;
    TEST_ASSERT_EQUAL(0, client->connect("broker.local", 1883));
    TEST_ASSERT_FALSE(client->connected());
    TEST_ASSERT_EQUAL(ModemErr::CMD_ERROR, client->lastError());
}

void test_connect_fail(void) {
    modem.mode = FakeModem::Mode::CONNECT_FAIL;
    TEST_ASSERT_EQUAL(0, client->connect("broker.local", 1883));
    TEST_ASSERT_EQUAL(ModemErr::CONNECT_FAIL, client->lastError());
}

void test_connect_timeout(void) {
    // 模组不吭声：靠 FakeModem 空闲推进时钟来触发超时
    modem.mode = FakeModem::Mode::SILENT;
    TEST_ASSERT_EQUAL(0, client->connect("broker.local", 1883));
    TEST_ASSERT_EQUAL(ModemErr::TIMEOUT, client->lastError());
}

void test_connect_null_host(void) {
    TEST_ASSERT_EQUAL(0, client->connect(static_cast<const char *>(nullptr), 1883));
    TEST_ASSERT_EQUAL(ModemErr::CONNECT_FAIL, client->lastError());
}

// ---------------------------------------------------------------- 写

void test_write_handshake(void) {
    bringUp();

    const char *msg = "{\"v\":1}";
    TEST_ASSERT_EQUAL(7, client->write(reinterpret_cast<const uint8_t *>(msg), 7));
    TEST_ASSERT_EQUAL(ModemErr::NONE, client->lastError());

    TEST_ASSERT_EQUAL(1, modem.payloads.size());
    TEST_ASSERT_EQUAL_STRING(msg, modem.payloads[0].c_str());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, modem.all_tx.find("AT+CIPSEND=7\r\n"));
}

void test_write_send_fail(void) {
    bringUp();
    modem.mode = FakeModem::Mode::SEND_FAIL;

    const uint8_t b = 'x';
    TEST_ASSERT_EQUAL(0, client->write(&b, 1));
    TEST_ASSERT_EQUAL(ModemErr::SEND_FAIL, client->lastError());
}

void test_write_no_prompt(void) {
    bringUp();
    modem.mode = FakeModem::Mode::NO_PROMPT;

    const uint8_t b = 'x';
    TEST_ASSERT_EQUAL(0, client->write(&b, 1));
    TEST_ASSERT_EQUAL(ModemErr::CMD_ERROR, client->lastError());
}

void test_write_when_closed(void) {
    const uint8_t b = 'x';
    TEST_ASSERT_EQUAL(0, client->write(&b, 1));
    TEST_ASSERT_EQUAL(ModemErr::NOT_CONNECTED, client->lastError());
}

void test_write_too_long(void) {
    bringUp();
    std::vector<uint8_t> big(Air780Client::SEND_MAX + 1, 'a');
    TEST_ASSERT_EQUAL(0, client->write(big.data(), big.size()));
    TEST_ASSERT_EQUAL(ModemErr::TOO_LONG, client->lastError());
}

void test_write_empty(void) {
    bringUp();
    const uint8_t b = 0;
    TEST_ASSERT_EQUAL(0, client->write(&b, 0));
}

// ---------------------------------------------------------------- 断连

void test_closed_urc(void) {
    bringUp();
    // 模组主动上报 CLOSED（比如信号丢失）
    // 注意 bringUp 里注入的 4 字节 "OK" 也会被吃掉，所以先把这个 URC 放在后面
    modem.inject("\r\nCLOSED\r\n");
    TEST_ASSERT_FALSE(client->connected());
}

void test_stop_closes(void) {
    bringUp();
    client->stop();
    TEST_ASSERT_FALSE(client->connected());
}

// ---------------------------------------------------------------- 缓冲

void test_rx_ring_overflow_drops_oldest(void) {
    // 注入超过 RX_CAP 的数据：应保留最新的 RX_CAP 字节，不能卡死
    const size_t total = Air780Client::RX_CAP + 100;
    std::string payload(total, 'Z');
    payload[0] = 'A';
    payload[total - 1] = 'B';

    char header[32];
    snprintf(header, sizeof(header), "+IPD,%u:", static_cast<unsigned>(total));
    modem.inject(std::string(header) + payload);

    // poll() 单次最多吃 512 字节（防止模组狂刷时主循环饿死），
    // 所以这里要多推几轮才能把 600+ 字节全部消费掉
    while (modem.available() > 0) client->poll();

    TEST_ASSERT_EQUAL(static_cast<int>(Air780Client::RX_CAP), client->available());

    uint8_t buf[Air780Client::RX_CAP];
    client->read(buf, sizeof(buf));
    // 最老的 'A' 被挤掉了，最新的 'B' 还在
    TEST_ASSERT_EQUAL_UINT8('B', buf[Air780Client::RX_CAP - 1]);
    TEST_ASSERT_NOT_EQUAL('A', buf[0]);
}

void test_peek_does_not_consume(void) {
    modem.inject("+IPD,3:abc");
    TEST_ASSERT_EQUAL('a', client->peek());
    TEST_ASSERT_EQUAL('a', client->peek());
    TEST_ASSERT_EQUAL(3, client->available());

    TEST_ASSERT_EQUAL('a', client->read());
    TEST_ASSERT_EQUAL('b', client->peek());
}

void test_read_when_empty(void) {
    TEST_ASSERT_EQUAL(-1, client->read());
    uint8_t buf[4];
    TEST_ASSERT_EQUAL(0, client->read(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(-1, client->peek());
}

void test_response_lines_ignored_when_idle(void) {
    // 空闲时收到一堆与业务无关的行，不应该变成 TCP 数据
    modem.inject("\r\nRDY\r\n+CFUN: 1\r\n+CPIN: READY\r\n");
    TEST_ASSERT_EQUAL(0, client->available());
}

void test_err_name_table(void) {
    TEST_ASSERT_EQUAL_STRING("TIMEOUT", modemErrName(ModemErr::TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("SEND_FAIL", modemErrName(ModemErr::SEND_FAIL));
    TEST_ASSERT_EQUAL_STRING("NO_PROMPT", modemErrName(ModemErr::NO_PROMPT));
}

// ---------------------------------------------------------------- main

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_ipd_short_format);
    RUN_TEST(test_ipd_with_conn_id);
    RUN_TEST(test_ipd_split_across_reads);
    RUN_TEST(test_ipd_payload_keeps_crlf);
    RUN_TEST(test_ipd_payload_keeps_colon);
    RUN_TEST(test_ipd_two_packets_back_to_back);
    RUN_TEST(test_ipd_after_crlf_prefix);

    RUN_TEST(test_connect_ok);
    RUN_TEST(test_connect_only_connect_ok);
    RUN_TEST(test_connect_error);
    RUN_TEST(test_connect_fail);
    RUN_TEST(test_connect_timeout);
    RUN_TEST(test_connect_null_host);

    RUN_TEST(test_write_handshake);
    RUN_TEST(test_write_send_fail);
    RUN_TEST(test_write_no_prompt);
    RUN_TEST(test_write_when_closed);
    RUN_TEST(test_write_too_long);
    RUN_TEST(test_write_empty);

    RUN_TEST(test_closed_urc);
    RUN_TEST(test_stop_closes);

    RUN_TEST(test_rx_ring_overflow_drops_oldest);
    RUN_TEST(test_peek_does_not_consume);
    RUN_TEST(test_read_when_empty);
    RUN_TEST(test_response_lines_ignored_when_idle);
    RUN_TEST(test_err_name_table);

    return UNITY_END();
}
