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

void Guard::enter(GuardState s) {
    if (state_ == s) return;
    state_ = s;
    if (s == GuardState::ARMED) motion_since_ = 0;
}

void Guard::tick(LockState lock, bool tampered, float motion_rms_g) {
    const uint32_t now = millis();

    // 防拆优先级最高，任何状态都直接升级
    if (tampered) {
        if (state_ != GuardState::ALARM) {
            enter(GuardState::ALARM);
            alarm_until_ = now + ALARM_COOLDOWN_MS;
        }
        return;
    }

    switch (state_) {
        case GuardState::INIT:
            if (lock == LockState::LOCKED)        enter(GuardState::ARMED);
            else if (lock == LockState::UNLOCKED) enter(GuardState::DISARMED);
            // UNKNOWN / MOVING：继续等
            break;

        case GuardState::DISARMED:
            // 只有落锁才是布防入口
            if (lock == LockState::LOCKED) enter(GuardState::ARMED);
            break;

        case GuardState::ARMED:
            // 开锁 = 明确撤防，优先级高于一切
            if (lock == LockState::UNLOCKED) {
                enter(GuardState::DISARMED);
                break;
            }
            // 持续运动才升级，单次冲击不算
            if (motion_rms_g > MOTION_RMS_G) {
                if (motion_since_ == 0) motion_since_ = now;
                if (now - motion_since_ >= MOTION_HOLD_MS) {
                    enter(GuardState::ALARM);
                    alarm_until_ = now + ALARM_COOLDOWN_MS;
                }
            } else {
                motion_since_ = 0;
            }
            break;

        case GuardState::ALARM:
            if (lock == LockState::UNLOCKED) {
                enter(GuardState::DISARMED);
                break;
            }
            if ((int32_t)(now - alarm_until_) >= 0) {
                enter(GuardState::ARMED);   // 冷却结束回到布防
            }
            break;
    }
}
