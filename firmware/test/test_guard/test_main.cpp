/**
 * guard 状态机单元测试（native，不需要硬件）
 *
 * 跑法：
 *   cd firmware && pio test -e native
 *
 * 每个用例自己推进虚拟时钟，验证状态序列是否符合预期。
 * 判定阈值来自 include/config.h，改阈值这里会跟着变。
 */

#include <unity.h>

#include "config.h"
#include "guard.h"

// ---------------------------------------------------------------- 辅助

static Guard g;

// 把时间往前推 ms 毫秒
static void advance(uint32_t ms) { mock_millis_ref() += ms; }

// 喂一次输入并推进时间
static void feed(LockState lock, bool tampered, float rms, uint32_t dt_ms) {
    advance(dt_ms);
    GuardInput in{ lock, tampered, rms };
    g.tick(in);
}

// 从干净状态起步
static void reset_to(LockState lock) {
    mock_millis_ref() = 0;
    g.begin();
    g.setActionHandler(nullptr);
    feed(lock, false, 0.0f, 0);
}

void setUp(void) { mock_millis_ref() = 0; }
void tearDown(void) {}

// ---------------------------------------------------------------- 用例

void test_unlocked_goes_disarmed(void) {
    reset_to(LockState::UNLOCKED);
    TEST_ASSERT_EQUAL(GuardState::DISARMED, g.state());
}

void test_locked_goes_armed(void) {
    reset_to(LockState::LOCKED);
    TEST_ASSERT_EQUAL(GuardState::ARMED, g.state());
}

void test_unknown_stays_init(void) {
    reset_to(LockState::UNKNOWN);
    TEST_ASSERT_EQUAL(GuardState::INIT, g.state());
}

void test_unlock_disarms(void) {
    reset_to(LockState::LOCKED);
    TEST_ASSERT_EQUAL(GuardState::ARMED, g.state());

    feed(LockState::UNLOCKED, false, 0.0f, 100);
    TEST_ASSERT_EQUAL(GuardState::DISARMED, g.state());
}

void test_short_motion_does_not_alarm(void) {
    reset_to(LockState::LOCKED);

    // 振动强度超过阈值，但持续时间不足 MOTION_HOLD_MS
    const uint32_t step = 100;
    const uint32_t total = MOTION_HOLD_MS - 200;
    for (uint32_t t = 0; t < total; t += step) {
        feed(LockState::LOCKED, false, MOTION_RMS_G * 2.0f, step);
    }
    TEST_ASSERT_EQUAL(GuardState::ARMED, g.state());
}

void test_sustained_motion_alarms(void) {
    reset_to(LockState::LOCKED);

    const uint32_t step = 100;
    for (uint32_t t = 0; t <= MOTION_HOLD_MS + 200; t += step) {
        feed(LockState::LOCKED, false, MOTION_RMS_G * 2.0f, step);
    }
    TEST_ASSERT_EQUAL(GuardState::ALARM, g.state());
}

void test_motion_below_threshold_never_alarms(void) {
    reset_to(LockState::LOCKED);

    for (uint32_t t = 0; t < 5000; t += 100) {
        feed(LockState::LOCKED, false, MOTION_RMS_G * 0.5f, 100);
    }
    TEST_ASSERT_EQUAL(GuardState::ARMED, g.state());
}

void test_alarm_cools_down_to_armed(void) {
    reset_to(LockState::LOCKED);

    // 触发告警
    for (uint32_t t = 0; t <= MOTION_HOLD_MS + 200; t += 100) {
        feed(LockState::LOCKED, false, MOTION_RMS_G * 2.0f, 100);
    }
    TEST_ASSERT_EQUAL(GuardState::ALARM, g.state());

    // 停止振动，等冷却结束
    feed(LockState::LOCKED, false, 0.0f, ALARM_COOLDOWN_MS + 200);
    TEST_ASSERT_EQUAL(GuardState::ARMED, g.state());
}

void test_unlock_during_alarm_disarms(void) {
    reset_to(LockState::LOCKED);
    for (uint32_t t = 0; t <= MOTION_HOLD_MS + 200; t += 100) {
        feed(LockState::LOCKED, false, MOTION_RMS_G * 2.0f, 100);
    }
    TEST_ASSERT_EQUAL(GuardState::ALARM, g.state());

    feed(LockState::UNLOCKED, false, 0.0f, 100);
    TEST_ASSERT_EQUAL(GuardState::DISARMED, g.state());
}

void test_tamper_jumps_to_alarm_from_disarmed(void) {
    reset_to(LockState::UNLOCKED);
    TEST_ASSERT_EQUAL(GuardState::DISARMED, g.state());

    feed(LockState::UNLOCKED, true, 0.0f, 100);
    TEST_ASSERT_EQUAL(GuardState::ALARM, g.state());
}

void test_tamper_wins_over_motion(void) {
    reset_to(LockState::LOCKED);
    feed(LockState::LOCKED, true, 0.0f, 100);
    TEST_ASSERT_EQUAL(GuardState::ALARM, g.state());
}

void test_action_handler_fires_on_transition(void) {
    static GuardState seen[8];
    static int count = 0;

    reset_to(LockState::UNLOCKED);
    count = 0;
    g.setActionHandler([](GuardState s) {
        if (count < 8) seen[count++] = s;
    });

    feed(LockState::LOCKED, false, 0.0f, 100);     // DISARMED -> ARMED
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(GuardState::ARMED, seen[0]);

    feed(LockState::LOCKED, false, 0.0f, 100);     // 状态没变，不应再触发
    TEST_ASSERT_EQUAL(1, count);
}

// ---------------------------------------------------------------- 入口

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_unlocked_goes_disarmed);
    RUN_TEST(test_locked_goes_armed);
    RUN_TEST(test_unknown_stays_init);
    RUN_TEST(test_unlock_disarms);
    RUN_TEST(test_short_motion_does_not_alarm);
    RUN_TEST(test_sustained_motion_alarms);
    RUN_TEST(test_motion_below_threshold_never_alarms);
    RUN_TEST(test_alarm_cools_down_to_armed);
    RUN_TEST(test_unlock_during_alarm_disarms);
    RUN_TEST(test_tamper_jumps_to_alarm_from_disarmed);
    RUN_TEST(test_tamper_wins_over_motion);
    RUN_TEST(test_action_handler_fires_on_transition);
    return UNITY_END();
}
