/**
 * net 状态机单元测试（native，不需要硬件）
 *
 * 跑法：cd firmware && pio test -e native
 *
 * 时间完全由输入注入，所以退避等待不用真的等 30 秒。
 */

#include <unity.h>

#include "net.h"

static Net n;
static uint32_t now_ms = 0;

static NetInput make(bool modem, bool reg, bool tcp) {
    return NetInput{ modem, reg, tcp, now_ms };
}

static void feed(bool modem, bool reg, bool tcp) { n.tick(make(modem, reg, tcp)); }

static void advance(uint32_t ms) { now_ms += ms; }

static void reset_to_online() {
    now_ms = 0;
    n.begin(0);
    feed(true, true, false);      // → CONNECTING
    feed(true, true, true);       // → ONLINE
}

void setUp(void) {
    now_ms = 0;
    n.begin(0);
}
void tearDown(void) {}

// ---------------------------------------------------------------- 基本迁移

void test_starts_idle(void) {
    TEST_ASSERT_EQUAL(NetState::IDLE, n.state());
}

void test_modem_ready_without_reg_waits(void) {
    feed(true, false, false);
    TEST_ASSERT_EQUAL(NetState::WAIT_REG, n.state());
}

void test_register_then_connecting(void) {
    feed(true, false, false);
    feed(true, true, false);
    TEST_ASSERT_EQUAL(NetState::CONNECTING, n.state());
}

void test_tcp_connected_goes_online(void) {
    reset_to_online();
    TEST_ASSERT_EQUAL(NetState::ONLINE, n.state());
    TEST_ASSERT_TRUE(n.online());
}

void test_modem_lost_returns_idle(void) {
    reset_to_online();
    feed(false, false, false);
    TEST_ASSERT_EQUAL(NetState::IDLE, n.state());
}

// ---------------------------------------------------------------- 失败与退避

void test_connect_timeout_enters_retry(void) {
    feed(true, true, false);          // CONNECTING，记下起点
    TEST_ASSERT_EQUAL(NetState::CONNECTING, n.state());

    advance(9000);                    // 未超时
    feed(true, true, false);
    TEST_ASSERT_EQUAL(NetState::CONNECTING, n.state());

    advance(1500);                    // 累计 10.5 s，超时
    feed(true, true, false);
    TEST_ASSERT_EQUAL(NetState::RETRY, n.state());
    TEST_ASSERT_EQUAL(1, n.failCount());
    TEST_ASSERT_EQUAL(2000, n.backoffMs());
}

void test_backoff_grows_and_caps(void) {
    // 第一次失败
    feed(true, true, false);
    advance(11000);
    feed(true, true, false);
    TEST_ASSERT_EQUAL(2000, n.backoffMs());

    // 等退避结束，再次进入 CONNECTING 并失败
    advance(2000);
    feed(true, true, false);
    TEST_ASSERT_EQUAL(NetState::CONNECTING, n.state());
    advance(11000);
    feed(true, true, false);
    TEST_ASSERT_EQUAL(4000, n.backoffMs());

    // 继续推到封顶
    for (int i = 0; i < 5; i++) {
        advance(n.backoffMs() + 100);
        feed(true, true, false);
        advance(11000);
        feed(true, true, false);
    }
    TEST_ASSERT_EQUAL(30000, n.backoffMs());
}

void test_retry_waits_before_reconnect(void) {
    feed(true, true, false);
    advance(11000);
    feed(true, true, false);          // → RETRY, backoff 2000
    TEST_ASSERT_EQUAL(NetState::RETRY, n.state());

    advance(1000);                    // 还没到
    feed(true, true, false);
    TEST_ASSERT_EQUAL(NetState::RETRY, n.state());

    advance(1200);                    // 累计 2.2 s，可以重试
    feed(true, true, false);
    TEST_ASSERT_EQUAL(NetState::CONNECTING, n.state());
}

void test_success_resets_backoff(void) {
    // 先失败一次
    feed(true, true, false);
    advance(11000);
    feed(true, true, false);
    TEST_ASSERT_EQUAL(1, n.failCount());

    // 重试并成功
    advance(2100);
    feed(true, true, false);          // CONNECTING
    feed(true, true, true);           // ONLINE
    TEST_ASSERT_EQUAL(NetState::ONLINE, n.state());
    TEST_ASSERT_EQUAL(0, n.failCount());
    TEST_ASSERT_EQUAL(0, n.backoffMs());
}

void test_drop_from_online_enters_retry(void) {
    reset_to_online();
    feed(true, true, false);          // TCP 掉了
    TEST_ASSERT_EQUAL(NetState::RETRY, n.state());
    TEST_ASSERT_EQUAL(1, n.failCount());
}

// ---------------------------------------------------------------- 等待时间

void test_wait_ms_reports_remaining(void) {
    feed(true, true, false);
    advance(11000);
    feed(true, true, false);          // RETRY，退避 2000
    advance(500);
    TEST_ASSERT_EQUAL(1500, n.waitMs(now_ms));

    advance(2000);
    TEST_ASSERT_EQUAL(0, n.waitMs(now_ms));
}

void test_wait_ms_zero_when_not_retry(void) {
    reset_to_online();
    TEST_ASSERT_EQUAL(0, n.waitMs(now_ms));
}

// ---------------------------------------------------------------- 入口

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_idle);
    RUN_TEST(test_modem_ready_without_reg_waits);
    RUN_TEST(test_register_then_connecting);
    RUN_TEST(test_tcp_connected_goes_online);
    RUN_TEST(test_modem_lost_returns_idle);
    RUN_TEST(test_connect_timeout_enters_retry);
    RUN_TEST(test_backoff_grows_and_caps);
    RUN_TEST(test_retry_waits_before_reconnect);
    RUN_TEST(test_success_resets_backoff);
    RUN_TEST(test_drop_from_online_enters_retry);
    RUN_TEST(test_wait_ms_reports_remaining);
    RUN_TEST(test_wait_ms_zero_when_not_retry);
    return UNITY_END();
}
