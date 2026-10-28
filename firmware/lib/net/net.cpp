#include "net.h"

namespace {
// 连接尝试的超时：超过这个时间还没连上就判定失败
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;

// 退避序列，最后一项封顶
constexpr uint32_t BACKOFF_MS[] = { 2000, 4000, 8000, 16000, 30000 };
constexpr uint32_t BACKOFF_MAX_IDX = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]) - 1;
}  // namespace

const char *Net::name(NetState s) {
    switch (s) {
        case NetState::IDLE:       return "IDLE";
        case NetState::WAIT_REG:   return "WAIT_REG";
        case NetState::CONNECTING: return "CONNECTING";
        case NetState::ONLINE:     return "ONLINE";
        case NetState::RETRY:      return "RETRY";
    }
    return "?";
}

void Net::begin(uint32_t now_ms) {
    state_ = NetState::IDLE;
    fail_count_ = 0;
    backoff_ms_ = 0;
    next_attempt_ms_ = now_ms;
    connecting_since_ms_ = 0;
}

void Net::enter(NetState s, uint32_t now_ms) {
    if (state_ == s) return;
    state_ = s;

    if (s == NetState::CONNECTING) {
        connecting_since_ms_ = now_ms;
    } else if (s == NetState::ONLINE) {
        // 连上就清零退避，下次断线重新从 2 s 开始
        fail_count_ = 0;
        backoff_ms_ = 0;
        next_attempt_ms_ = now_ms;
    }
}

void Net::fail(uint32_t now_ms) {
    if (fail_count_ < 0xFFFFFFFFu) fail_count_++;

    const uint32_t idx = (fail_count_ - 1) < BACKOFF_MAX_IDX
                             ? (fail_count_ - 1)
                             : BACKOFF_MAX_IDX;
    backoff_ms_ = BACKOFF_MS[idx];
    next_attempt_ms_ = now_ms + backoff_ms_;
    enter(NetState::RETRY, now_ms);
}

uint32_t Net::waitMs(uint32_t now_ms) const {
    if (state_ != NetState::RETRY) return 0;
    if ((int32_t)(next_attempt_ms_ - now_ms) <= 0) return 0;
    return next_attempt_ms_ - now_ms;
}

void Net::tick(const NetInput &in) {
    switch (state_) {
        case NetState::IDLE:
            if (in.modem_ready) {
                enter(in.registered ? NetState::CONNECTING : NetState::WAIT_REG, in.now_ms);
            }
            break;

        case NetState::WAIT_REG:
            if (!in.modem_ready) {
                enter(NetState::IDLE, in.now_ms);
            } else if (in.registered) {
                enter(NetState::CONNECTING, in.now_ms);
            }
            break;

        case NetState::CONNECTING:
            if (!in.modem_ready) {
                enter(NetState::IDLE, in.now_ms);
            } else if (in.tcp_connected) {
                enter(NetState::ONLINE, in.now_ms);
            } else if (!in.registered) {
                enter(NetState::WAIT_REG, in.now_ms);
            } else if ((int32_t)(in.now_ms - connecting_since_ms_) >= (int32_t)CONNECT_TIMEOUT_MS) {
                fail(in.now_ms);
            }
            break;

        case NetState::ONLINE:
            if (!in.modem_ready) {
                enter(NetState::IDLE, in.now_ms);
            } else if (!in.registered) {
                enter(NetState::WAIT_REG, in.now_ms);
            } else if (!in.tcp_connected) {
                fail(in.now_ms);   // 掉线，走退避
            }
            break;

        case NetState::RETRY:
            if (!in.modem_ready) {
                enter(NetState::IDLE, in.now_ms);
            } else if ((int32_t)(in.now_ms - next_attempt_ms_) >= 0) {
                enter(in.registered ? NetState::CONNECTING : NetState::WAIT_REG, in.now_ms);
            }
            break;
    }
}
