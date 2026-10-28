#pragma once

/**
 * 防盗状态机
 *
 * 输入只有三类：
 *   - 锁态（唯一能改变布防/撤防的依据）
 *   - 防拆信号
 *   - 运动强度（加速度标准差）
 *
 * 输出：状态变化，由 main 负责打印 / 触发喇叭 / 点亮车灯。
 *
 *   INIT ──锁上──▶ ARMED ──持续运动──▶ ALARM
 *     │             ▲                    │
 *     └──未锁──▶ DISARMED ◀──锁开────────┘
 */

#include <Arduino.h>
#include "lock.h"

enum class GuardState : uint8_t {
    INIT = 0,
    DISARMED,
    ARMED,
    ALARM
};

class Guard {
public:
    void begin();
    void tick(LockState lock, bool tampered, float motion_rms_g);

    GuardState state() const { return state_; }
    static const char *name(GuardState s);

private:
    void enter(GuardState s);

    GuardState state_ = GuardState::INIT;
    uint32_t motion_since_ = 0;
    uint32_t alarm_until_ = 0;
};
