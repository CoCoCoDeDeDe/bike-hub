/**
 * protocol 编解码单元测试（native，不需要硬件）
 *
 * 跑法：cd firmware && pio test -e native
 *
 * 断言方式：把生成结果再用 ArduinoJson 解回来，检查字段——
 * 比字符串比对更耐受字段顺序和空格差异。
 */

#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>

#include "protocol.h"

using namespace proto;

static char buf[MSG_MAX];

void setUp(void) { memset(buf, 0, sizeof(buf)); }
void tearDown(void) {}

// ---------------------------------------------------------------- state

void test_state_contains_all_fields(void) {
    StateView s;
    s.lock = "LOCKED";
    s.guard = "ARMED";
    s.light = "OFF";
    s.alarm = "IDLE";
    s.batt = 86;
    s.volt = 3.95f;
    s.rssi = 18;
    s.uptime = 3600;
    s.fw = "0.4.0";

    const size_t n = build_state(buf, sizeof(buf), 1698765432u, s);
    TEST_ASSERT_GREATER_THAN(0, n);

    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, buf));

    TEST_ASSERT_EQUAL(1, doc["v"].as<int>());
    TEST_ASSERT_EQUAL(1698765432u, doc["ts"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("LOCKED", doc["lock"]);
    TEST_ASSERT_EQUAL_STRING("ARMED", doc["guard"]);
    TEST_ASSERT_EQUAL(86, doc["batt"].as<int>());
    TEST_ASSERT_EQUAL_STRING("0.4.0", doc["fw"]);
}

void test_state_rejects_small_buffer(void) {
    StateView s;
    char tiny[8];
    TEST_ASSERT_EQUAL(0, build_state(tiny, sizeof(tiny), 0, s));
}

// ---------------------------------------------------------------- event

void test_event_omits_optional_fields(void) {
    EventView e;
    e.seq = 42;
    e.type = "tamper";
    // rms 默认 -1，lock 默认 nullptr，都不应出现

    const size_t n = build_event(buf, sizeof(buf), 123, e);
    TEST_ASSERT_GREATER_THAN(0, n);

    StaticJsonDocument<384> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, buf));
    TEST_ASSERT_EQUAL(42, doc["id"].as<int>());
    TEST_ASSERT_EQUAL_STRING("tamper", doc["t"]);
    TEST_ASSERT_FALSE(doc.containsKey("rms"));
    TEST_ASSERT_FALSE(doc.containsKey("lock"));
}

void test_event_includes_optional_fields(void) {
    EventView e;
    e.seq = 7;
    e.type = "alarm";
    e.rms = 0.42f;
    e.lock = "LOCKED";

    TEST_ASSERT_GREATER_THAN(0, build_event(buf, sizeof(buf), 0, e));

    StaticJsonDocument<384> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, buf));
    TEST_ASSERT_TRUE(doc.containsKey("rms"));
    TEST_ASSERT_EQUAL_STRING("LOCKED", doc["lock"]);
}

// ---------------------------------------------------------------- loc / ack

void test_loc_roundtrip(void) {
    LocView l;
    l.lat = 31.2304;
    l.lon = 121.4737;
    l.acc = 8.5f;

    TEST_ASSERT_GREATER_THAN(0, build_loc(buf, sizeof(buf), 0, l));

    StaticJsonDocument<384> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, buf));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 31.2304, doc["lat"].as<double>());
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 121.4737, doc["lon"].as<double>());
    TEST_ASSERT_FALSE(doc.containsKey("spd"));   // 速度为负时不带
}

void test_ack_ok_and_error(void) {
    TEST_ASSERT_GREATER_THAN(0, build_ack(buf, sizeof(buf), 7, true));
    {
        StaticJsonDocument<256> doc;
        TEST_ASSERT_FALSE(deserializeJson(doc, buf));
        TEST_ASSERT_TRUE(doc["ok"].as<bool>());
        TEST_ASSERT_EQUAL(7, doc["id"].as<int>());
        TEST_ASSERT_FALSE(doc.containsKey("err"));
    }

    TEST_ASSERT_GREATER_THAN(0, build_ack(buf, sizeof(buf), 8, false, err::NO_SOUND));
    {
        StaticJsonDocument<256> doc;
        TEST_ASSERT_FALSE(deserializeJson(doc, buf));
        TEST_ASSERT_FALSE(doc["ok"].as<bool>());
        TEST_ASSERT_EQUAL_STRING("no_such_sound", doc["err"]);
    }
}

// ---------------------------------------------------------------- 命令解析

void test_parse_play(void) {
    Command c;
    TEST_ASSERT_TRUE(parse_command(R"({"v":1,"id":9,"cmd":"play","arg":3})", c));
    TEST_ASSERT_EQUAL(CommandType::PLAY, c.type);
    TEST_ASSERT_EQUAL(9, c.id);
    TEST_ASSERT_EQUAL(3, c.arg);
}

void test_parse_play_out_of_range(void) {
    Command c;
    const char *e = nullptr;
    TEST_ASSERT_FALSE(parse_command(R"({"v":1,"id":1,"cmd":"play","arg":0})", c, &e));
    TEST_ASSERT_EQUAL_STRING(err::NO_SOUND, e);

    TEST_ASSERT_FALSE(parse_command(R"({"v":1,"id":1,"cmd":"play","arg":10})", c, &e));
    TEST_ASSERT_EQUAL_STRING(err::NO_SOUND, e);
}

void test_parse_simple_commands(void) {
    Command c;
    TEST_ASSERT_TRUE(parse_command(R"({"cmd":"stop"})", c));
    TEST_ASSERT_EQUAL(CommandType::STOP, c.type);

    TEST_ASSERT_TRUE(parse_command(R"({"cmd":"status"})", c));
    TEST_ASSERT_EQUAL(CommandType::STATUS, c.type);

    TEST_ASSERT_TRUE(parse_command(R"({"cmd":"lock"})", c));
    TEST_ASSERT_EQUAL(CommandType::LOCK, c.type);

    TEST_ASSERT_TRUE(parse_command(R"({"cmd":"unlock"})", c));
    TEST_ASSERT_EQUAL(CommandType::UNLOCK, c.type);
}

void test_parse_unknown_command(void) {
    Command c;
    const char *e = nullptr;
    TEST_ASSERT_FALSE(parse_command(R"({"cmd":"disarm"})", c, &e));
    TEST_ASSERT_EQUAL_STRING(err::BAD_CMD, e);
}

void test_parse_bad_json(void) {
    Command c;
    const char *e = nullptr;
    TEST_ASSERT_FALSE(parse_command("{not json", c, &e));
    TEST_ASSERT_EQUAL_STRING(err::BAD_JSON, e);

    TEST_ASSERT_FALSE(parse_command(nullptr, c, &e));
    TEST_ASSERT_EQUAL_STRING(err::BAD_JSON, e);
}

void test_parse_without_cmd(void) {
    Command c;
    const char *e = nullptr;
    TEST_ASSERT_FALSE(parse_command(R"({"id":1,"arg":2})", c, &e));
    TEST_ASSERT_EQUAL_STRING(err::BAD_CMD, e);
}

void test_parse_rejects_future_version(void) {
    Command c;
    const char *e = nullptr;
    TEST_ASSERT_FALSE(parse_command(R"({"v":2,"cmd":"stop"})", c, &e));
    TEST_ASSERT_EQUAL_STRING(err::BAD_CMD, e);
}

// ---------------------------------------------------------------- 入口

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_state_contains_all_fields);
    RUN_TEST(test_state_rejects_small_buffer);
    RUN_TEST(test_event_omits_optional_fields);
    RUN_TEST(test_event_includes_optional_fields);
    RUN_TEST(test_loc_roundtrip);
    RUN_TEST(test_ack_ok_and_error);
    RUN_TEST(test_parse_play);
    RUN_TEST(test_parse_play_out_of_range);
    RUN_TEST(test_parse_simple_commands);
    RUN_TEST(test_parse_unknown_command);
    RUN_TEST(test_parse_bad_json);
    RUN_TEST(test_parse_without_cmd);
    RUN_TEST(test_parse_rejects_future_version);
    return UNITY_END();
}
