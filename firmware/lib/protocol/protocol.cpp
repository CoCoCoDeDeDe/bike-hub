#include "protocol.h"

#include <ArduinoJson.h>
#include <string.h>

namespace proto {

namespace {

// 容量按最长消息估算，留一点余量，避免 measureJson 才发现溢出
constexpr size_t DOC_SMALL = 192;
constexpr size_t DOC_STATE = 384;
constexpr size_t DOC_EVENT = 192;

template <size_t N>
size_t emit(StaticJsonDocument<N> &doc, char *out, size_t cap) {
    const size_t need = measureJson(doc) + 1;   // +1 为结尾 '\0'
    if (need > cap) return 0;
    return serializeJson(doc, out, cap);
}

}  // namespace

// ---------------------------------------------------------------- 构造

size_t build_state(char *out, size_t cap, uint32_t ts, const StateView &s) {
    StaticJsonDocument<DOC_STATE> doc;
    doc["v"]     = VERSION;
    doc["ts"]    = ts;
    doc["lock"]  = s.lock;
    doc["guard"] = s.guard;
    doc["light"] = s.light;
    doc["alarm"] = s.alarm;
    doc["batt"]  = s.batt;
    doc["volt"]  = s.volt;
    doc["rssi"]  = s.rssi;
    doc["up"]    = s.uptime;
    doc["fw"]    = s.fw;
    return emit(doc, out, cap);
}

size_t build_event(char *out, size_t cap, uint32_t ts, const EventView &e) {
    StaticJsonDocument<DOC_EVENT> doc;
    doc["v"]  = VERSION;
    doc["id"] = e.seq;
    doc["ts"] = ts;
    doc["t"]  = e.type;

    if (e.rms >= 0.0f) doc["rms"] = e.rms;
    if (e.lock)        doc["lock"] = e.lock;

    return emit(doc, out, cap);
}

size_t build_loc(char *out, size_t cap, uint32_t ts, const LocView &l) {
    StaticJsonDocument<DOC_SMALL> doc;
    doc["v"]  = VERSION;
    doc["ts"] = ts;
    doc["lat"] = l.lat;
    doc["lon"] = l.lon;

    if (l.acc >= 0.0f)   doc["acc"] = l.acc;
    if (l.speed >= 0.0f) doc["spd"] = l.speed;

    return emit(doc, out, cap);
}

size_t build_ack(char *out, size_t cap, uint32_t cmd_id, bool ok, const char *err) {
    StaticJsonDocument<DOC_SMALL> doc;
    doc["v"]  = VERSION;
    doc["id"] = cmd_id;
    doc["ok"] = ok;
    if (!ok && err) doc["err"] = err;
    return emit(doc, out, cap);
}

size_t build_offline(char *out, size_t cap) {
    StaticJsonDocument<DOC_SMALL> doc;
    doc["v"]      = VERSION;
    doc["online"] = false;
    return emit(doc, out, cap);
}

// ---------------------------------------------------------------- 解析

const char *command_name(CommandType t) {
    switch (t) {
        case CommandType::PLAY:   return "play";
        case CommandType::STOP:   return "stop";
        case CommandType::STATUS: return "status";
        case CommandType::LOCK:   return "lock";
        case CommandType::UNLOCK: return "unlock";
        case CommandType::NONE:   return "none";
    }
    return "none";
}

bool parse_command(const char *json, Command &out, const char **error) {
    out = Command{};
    const char *e = nullptr;

    if (!json) {
        e = err::BAD_JSON;
        if (error) *error = e;
        return false;
    }

    StaticJsonDocument<DOC_SMALL> doc;
    const DeserializationError de = deserializeJson(doc, json);
    if (de) {
        if (error) *error = err::BAD_JSON;
        return false;
    }

    // 版本检查：只认 1；将来做兼容时在这里放宽
    if (doc.containsKey("v") && (int)doc["v"] != VERSION) {
        if (error) *error = err::BAD_CMD;
        return false;
    }

    const char *cmd = doc["cmd"];
    if (!cmd) {
        if (error) *error = err::BAD_CMD;
        return false;
    }

    out.id = doc["id"] | 0;

    if (strcmp(cmd, "play") == 0) {
        const int id = doc["arg"] | 0;
        if (id < 1 || id > 9) {
            if (error) *error = err::NO_SOUND;
            return false;
        }
        out.type = CommandType::PLAY;
        out.arg  = (uint8_t)id;
        return true;
    }
    if (strcmp(cmd, "stop") == 0)   { out.type = CommandType::STOP;   return true; }
    if (strcmp(cmd, "status") == 0) { out.type = CommandType::STATUS; return true; }
    if (strcmp(cmd, "lock") == 0)   { out.type = CommandType::LOCK;   return true; }
    if (strcmp(cmd, "unlock") == 0) { out.type = CommandType::UNLOCK; return true; }

    if (error) *error = err::BAD_CMD;
    return false;
}

}  // namespace proto
