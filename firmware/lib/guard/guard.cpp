#include "guard.h"
#include "config.h"

const char *Guard::name(GuardState s) {
    switch (s) {
        case GuardState::INIT:     return "INIT";
        case GuardState::DISARMED: return "DISARMED";
        case GuardState::ARMED:    return "ARMED";
        case GuardState::ALARM:    return "ALARM";
    }
    return "?";
}

void Guard::begin() {
    state_ = GuardState::INIT;
    motion_since_ = 0;
    alarm_until_ = 0;
}

// ---------------------------------------------------------------- 迁移

void Guard::onEnter(GuardState s) {
    // 只做本状态自己的事，不碰别的模块
    switch (s) {
        case GuardState::ARMED:
            motion_since_ = 0;
            break;
        case GuardState::ALARM:
            alarm_until_ = millis() + ALARM_COOLDOWN_MS;
            break;
        default:
            break;
    }
}

void Guard::onExit(GuardState s) {
    (void)s;   // 目前没有需要清理的东西
}

void Guard::enter(GuardState s) {
    if (state_ == s) return;

    const GuardState prev = state_;
    onExit(prev);
    state_ = s;
    onEnter(s);

    // 先通知外部，再打印——保证日志里的因果顺序是"动作 → 记录"
    if (action_) action_(s);

    Serial.printf("[guard] %-9s -> %-9s (lock=%s tamper=%d rms=%.3f)\n",
                  name(prev), name(s),
                  Lock::name(last_input_.lock),
                  (int)last_input_.tampered,
                  last_input_.motion_rms_g);
}

// ---------------------------------------------------------------- 周期

void Guard::tick(const GuardInput &in) {
    last_input_ = in;
    const uint32_t now = millis();

    // 防拆优先级最高，任何状态都直接升级
    if (in.tampered) {
        enter(GuardState::ALARM);
        return;
    }

    switch (state_) {
        case GuardState::INIT:
            if (in.lock == LockState::LOCKED)        enter(GuardState::ARMED);
            else if (in.lock == LockState::UNLOCKED) enter(GuardState::DISARMED);
            // UNKNOWN / MOVING：继续等
            break;

        case GuardState::DISARMED:
            // 只有落锁才是布防入口
            if (in.lock == LockState::LOCKED) enter(GuardState::ARMED);
            break;

        case GuardState::ARMED:
            // 开锁 = 明确撤防，优先级高于一切
            if (in.lock == LockState::UNLOCKED) {
                enter(GuardState::DISARMED);
                break;
            }
            // 持续运动才升级，单次冲击不算
            if (in.motion_rms_g > MOTION_RMS_G) {
                if (motion_since_ == 0) motion_since_ = now;
                if (now - motion_since_ >= MOTION_HOLD_MS) {
                    enter(GuardState::ALARM);
                }
            } else {
                motion_since_ = 0;
            }
            break;

        case GuardState::ALARM:
            if (in.lock == LockState::UNLOCKED) {
                enter(GuardState::DISARMED);
                break;
            }
            if ((int32_t)(now - alarm_until_) >= 0) {
                enter(GuardState::ARMED);   // 冷却结束回到布防
            }
            break;
    }
}
