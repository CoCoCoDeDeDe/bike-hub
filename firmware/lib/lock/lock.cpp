#include "lock.h"
#include "config.h"

// ---------------------------------------------------------------- 名称

const char *Lock::name(LockState s) {
    switch (s) {
        case LockState::UNKNOWN:  return "UNKNOWN";
        case LockState::LOCKED:   return "LOCKED";
        case LockState::UNLOCKED: return "UNLOCKED";
        case LockState::MOVING:   return "MOVING";
        case LockState::TAMPERED: return "TAMPERED";
    }
    return "?";
}

// ---------------------------------------------------------------- 初始化

bool Lock::begin() {
#if MOCK_LOCK
    // Mock：默认未上锁，等串口命令驱动
    enter(LockState::UNLOCKED);
    Serial.println("[lock] Mock 模式（串口命令：lock / unlock / tamper）");
#else
    pinMode(PIN_LOCK_SENSE, INPUT_PULLUP);
    pinMode(PIN_TAMPER, INPUT_PULLUP);
    pinMode(PIN_LOCK_MOTOR_A, OUTPUT);
    pinMode(PIN_LOCK_MOTOR_B, OUTPUT);
    digitalWrite(PIN_LOCK_MOTOR_A, LOW);
    digitalWrite(PIN_LOCK_MOTOR_B, LOW);

    // 上电先读一次锁舌
    const bool closed = (digitalRead(PIN_LOCK_SENSE) == LOW);
    enter(closed ? LockState::LOCKED : LockState::UNLOCKED);
    Serial.printf("[lock] 就绪，锁舌当前 %s\n", closed ? "闭合" : "退出");

    // 开盖检测：常闭回路，读到高电平说明回路断了
    tampered_ = (digitalRead(PIN_TAMPER) != LOW);
    if (tampered_) {
        Serial.println("[lock] 警告：防拆回路断开");
        enter(LockState::TAMPERED);
    }
#endif
    return true;
}

// ---------------------------------------------------------------- 状态

void Lock::enter(LockState s) {
    if (state_ == s) return;
    state_ = s;
    // 状态变化由上层轮询后打印，这里不打印，避免重复刷屏
}

// ---------------------------------------------------------------- 命令

bool Lock::requestLock() {
#if MOCK_LOCK
    enter(LockState::LOCKED);
    return true;
#else
    if (state_ == LockState::LOCKED || state_ == LockState::MOVING) return false;
    digitalWrite(PIN_LOCK_MOTOR_A, HIGH);
    digitalWrite(PIN_LOCK_MOTOR_B, LOW);
    pending_ = LockState::LOCKED;
    action_deadline_ = millis() + LOCK_MOTOR_MS;
    enter(LockState::MOVING);
    return true;
#endif
}

bool Lock::requestUnlock() {
#if MOCK_LOCK
    enter(LockState::UNLOCKED);
    return true;
#else
    if (state_ == LockState::UNLOCKED || state_ == LockState::MOVING) return false;
    digitalWrite(PIN_LOCK_MOTOR_A, LOW);
    digitalWrite(PIN_LOCK_MOTOR_B, HIGH);
    pending_ = LockState::UNLOCKED;
    action_deadline_ = millis() + LOCK_MOTOR_MS;
    enter(LockState::MOVING);
    return true;
#endif
}

// ---------------------------------------------------------------- 周期

void Lock::tick() {
#if MOCK_LOCK
    // Mock 下状态完全由命令控制，不读硬件
#else
    // 防拆优先：回路断开立刻置位
    const bool broken = (digitalRead(PIN_TAMPER) != LOW);
    if (broken && !tampered_) {
        tampered_ = true;
        enter(LockState::TAMPERED);
        return;
    }
    if (tampered_) return;

    if (state_ == LockState::MOVING) {
        if ((int32_t)(millis() - action_deadline_) >= 0) {
            // 电机停转
            digitalWrite(PIN_LOCK_MOTOR_A, LOW);
            digitalWrite(PIN_LOCK_MOTOR_B, LOW);

            // 以传感器为准，而不是以"命令成功"为准
            const bool closed = (digitalRead(PIN_LOCK_SENSE) == LOW);
            const LockState actual = closed ? LockState::LOCKED : LockState::UNLOCKED;
            if (actual != pending_) {
                Serial.printf("[lock] 动作未到位：期望 %s，实际 %s\n",
                              name(pending_), name(actual));
            }
            enter(actual);
        }
        return;
    }

    // 静止时也持续跟随传感器，机械抖动、外力都算
    const bool closed = (digitalRead(PIN_LOCK_SENSE) == LOW);
    const LockState actual = closed ? LockState::LOCKED : LockState::UNLOCKED;
    if (actual != state_) enter(actual);
#endif
}

// ---------------------------------------------------------------- Mock

void Lock::mockSet(LockState s) {
    if (s == LockState::TAMPERED) {
        mockTamper(true);
        return;
    }
    tampered_ = false;
    enter(s);
}

void Lock::mockTamper(bool on) {
    tampered_ = on;
    if (on) enter(LockState::TAMPERED);
}
