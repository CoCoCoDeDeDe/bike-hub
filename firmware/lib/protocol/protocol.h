#pragma once

/**
 * MQTT 消息编解码
 *
 * 这一层不碰网络、不碰硬件，只做 JSON ↔ 结构体。
 * 协议定义见 docs/protocol.md。
 *
 * 设计约束：
 *   - 不用动态分配（StaticJsonDocument），避免堆碎片
 *   - 输出缓冲由调用方提供，写不下就返回 0，不截断
 *   - 解析失败要给出原因，方便面板侧排查
 */

#include <stdint.h>
#include <stddef.h>

namespace proto {

constexpr int    VERSION = 1;
constexpr size_t MSG_MAX = 256;   // 单条消息的建议上限

// ---------------------------------------------------------------- 下行命令

enum class CommandType : uint8_t {
    NONE = 0,
    PLAY,
    STOP,
    STATUS,
    LOCK,
    UNLOCK
};

struct Command {
    uint32_t    id   = 0;
    CommandType type = CommandType::NONE;
    uint8_t     arg  = 0;
};

/// 错误码，与 docs/protocol.md §3.4 一致
namespace err {
constexpr const char *BAD_JSON = "bad_json";
constexpr const char *BAD_CMD  = "bad_cmd";
constexpr const char *NO_SOUND = "no_such_sound";
}  // namespace err

// ---------------------------------------------------------------- 上行视图

struct StateView {
    const char *lock   = "UNKNOWN";
    const char *guard  = "INIT";
    const char *light  = "OFF";
    const char *alarm  = "IDLE";
    uint8_t     batt   = 0;
    float       volt   = 0.0f;
    int         rssi   = -1;
    uint32_t    uptime = 0;
    const char *fw     = "0.0.0";
};

struct EventView {
    uint32_t    seq  = 0;
    const char *type = "boot";
    float       rms  = -1.0f;    // < 0 表示不带这个字段
    const char *lock = nullptr;  // nullptr 表示不带
};

struct LocView {
    double lat   = 0.0;
    double lon   = 0.0;
    float  acc   = -1.0f;    // < 0 不带
    float  speed = -1.0f;    // < 0 不带
};

// ---------------------------------------------------------------- 构造

/// 返回写入长度；缓冲不足返回 0
size_t build_state(char *out, size_t cap, uint32_t ts, const StateView &s);
size_t build_event(char *out, size_t cap, uint32_t ts, const EventView &e);
size_t build_loc(char *out, size_t cap, uint32_t ts, const LocView &l);
size_t build_ack(char *out, size_t cap, uint32_t cmd_id, bool ok,
                 const char *err = nullptr);
/// 遗嘱消息（broker 代发），retained
size_t build_offline(char *out, size_t cap);

// ---------------------------------------------------------------- 解析

bool parse_command(const char *json, Command &out, const char **error = nullptr);

// ---------------------------------------------------------------- 工具

const char *command_name(CommandType t);

}  // namespace proto
