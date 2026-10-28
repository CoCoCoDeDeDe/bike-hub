#pragma once

/**
 * 锁模块
 *
 * 一体机里锁是信任根：只有锁舌的状态变化才能改变布防状态。
 *
 * 两种工作模式：
 *   MOCK_LOCK=1  串口命令注入状态（机械未就绪时用）
 *   MOCK_LOCK=0  读锁舌霍尔 + 驱动电机的 H 桥
 *
 * 防拆检测（PIN_TAMPER）两种模式下都生效：常闭回路断开即 TAMPERED。
 */

#include <Arduino.h>

enum class LockState : uint8_t {
    UNKNOWN = 0,   // 刚上电还没读到
    LOCKED,        // 锁舌闭合
    UNLOCKED,      // 锁舌退出
    MOVING,        // 电机动作中（过渡态）
    TAMPERED       // 回路断开 / 异常
};

class Lock {
public:
    bool begin();
    void tick();

    LockState state() const { return state_; }
    bool tampered() const { return tampered_; }

    // 请求动作。真实模式下驱动电机，Mock 模式下直接改状态。
    bool requestLock();
    bool requestUnlock();

    // 仅 MOCK_LOCK=1 时有效
    void mockSet(LockState s);
    void mockTamper(bool on);

    static const char *name(LockState s);

private:
    void enter(LockState s);

    LockState state_ = LockState::UNKNOWN;
    LockState pending_ = LockState::UNKNOWN;
    bool tampered_ = false;
    uint32_t action_deadline_ = 0;
};
