#pragma once

/**
 * 网络状态机
 *
 * 只做决策，不碰硬件、不碰 MQTT。输入是模组状态，输出是"现在该不该连"。
 * 动作由上层执行（modem 层发 AT、mqtt 层发布）。
 *
 * 状态：
 *   IDLE        模组未就绪
 *   WAIT_REG    等注网
 *   CONNECTING  TCP 连接中
 *   ONLINE      已连上，可收发
 *   RETRY       失败退避中
 *
 * 退避：2s → 4s → 8s → 16s → 30s 封顶，连接成功后清零。
 *
 * 纯逻辑，时间由外部注入（in.now_ms），所以能在电脑上测。
 */

#include <stdint.h>

enum class NetState : uint8_t {
    IDLE = 0,
    WAIT_REG,
    CONNECTING,
    ONLINE,
    RETRY
};

struct NetInput {
    bool     modem_ready;    // 模组上电并响应 AT
    bool     registered;     // 已注网（+CGATT: 1 且 +CREG 正常）
    bool     tcp_connected;  // 当前 TCP 会话是否建立
    uint32_t now_ms;
};

class Net {
public:
    void begin(uint32_t now_ms);
    void tick(const NetInput &in);

    NetState state() const { return state_; }
    bool     online() const { return state_ == NetState::ONLINE; }

    uint32_t backoffMs() const { return backoff_ms_; }
    uint32_t failCount() const { return fail_count_; }
    uint32_t nextAttemptMs() const { return next_attempt_ms_; }
    /// 距离下次可以尝试连接还有多久（RETRY 之外返回 0）
    uint32_t waitMs(uint32_t now_ms) const;

    static const char *name(NetState s);

private:
    void enter(NetState s, uint32_t now_ms);
    void fail(uint32_t now_ms);

    NetState state_        = NetState::IDLE;
    uint32_t fail_count_   = 0;
    uint32_t backoff_ms_   = 0;
    uint32_t next_attempt_ms_ = 0;
    uint32_t connecting_since_ms_ = 0;
};
