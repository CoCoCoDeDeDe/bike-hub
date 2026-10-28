#pragma once

/**
 * 防盗状态机
 *
 * 【状态机统一模式】每个 FSM 模块都提供这五样：
 *   1. enum class XxxState
 *   2. static const char *name(XxxState)
 *   3. void enter(XxxState)     判重 → onExit → 切换 → onEnter → 通知 → 打印
 *   4. void onEnter / onExit    只放本状态自己的副作用，不跨模块
 *   5. struct XxxInput + tick(const XxxInput &)   输入结构体化，加字段不破接口
 *
 * 输入只有三类：锁态（唯一布防依据）、防拆、运动强度。
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

/// 状态机的一次输入。以后加轮速、GPS 位移，往这里加字段即可。
struct GuardInput {
    LockState lock;
    bool tampered;
    float motion_rms_g;
};

/// 状态变化时通知外部（灯、喇叭等）。guard 不直接依赖那些模块。
using GuardAction = void (*)(GuardState);

class Guard {
public:
    void begin();
    void tick(const GuardInput &in);

    GuardState state() const { return state_; }
    static const char *name(GuardState s);

    void setActionHandler(GuardAction h) { action_ = h; }

private:
    void enter(GuardState s);
    void onEnter(GuardState s);
    void onExit(GuardState s);

    GuardState state_ = GuardState::INIT;
    GuardInput last_input_{ LockState::UNKNOWN, false, 0.0f };
    GuardAction action_ = nullptr;

    uint32_t motion_since_ = 0;
    uint32_t alarm_until_ = 0;
};
