#include "light.h"
#include "config.h"

// ---------------------------------------------------------------- 名称

const char *Light::name(LightMode m) {
    switch (m) {
        case LightMode::OFF:   return "OFF";
        case LightMode::ON:    return "ON";
        case LightMode::ALERT: return "ALERT";
    }
    return "?";
}

const char *Light::name(LightStatus s) {
    switch (s) {
        case LightStatus::ABSENT:  return "ABSENT";
        case LightStatus::PRESENT: return "PRESENT";
        case LightStatus::FAULT:   return "FAULT";
    }
    return "?";
}

// ---------------------------------------------------------------- 初始化

bool Light::begin() {
#if MOCK_LIGHT
    status_ = LightStatus::PRESENT;
    mode_ = LightMode::OFF;
    Serial.println("[light] Mock 模式（串口命令：light off|on|alert）");
#else
    pinMode(PIN_LIGHT_PWM, OUTPUT);
    ledcAttach(PIN_LIGHT_PWM, 5000, 10);   // 5 kHz, 10 bit
    ledcWrite(PIN_LIGHT_PWM, 0);
    status_ = LightStatus::ABSENT;
    Serial.println("[light] 就绪，等待识别车灯");
#endif
    return true;
}

// ---------------------------------------------------------------- 识别

void Light::tick() {
    const uint32_t now = millis();
    if (now - last_id_check_ < LIGHT_TICK_MS) return;
    last_id_check_ = now;

#if MOCK_LIGHT
    return;   // Mock 下状态由命令控制
#else
    // ID 针：灯内部有固定电阻分压，hub 读电压推算阻值
    const int raw = analogRead(PIN_LIGHT_ID);
    const float v = raw * 3.3f / 4095.0f;

    LightStatus s;
    if (raw <= 8) {
        s = LightStatus::FAULT;        // 对地短路，多半是触点进水
    } else if (raw >= 4087) {
        // 理论上是开路：按没装灯处理
        s = LightStatus::ABSENT;
    } else {
        // 3.3V 经 R_id 与 10k 上拉分压：R = 10k * (3.3 - v) / v
        const float r = 10000.0f * (3.3f - v) / (v > 0.01f ? v : 0.01f);
        if (r < 2000.0f)        s = LightStatus::FAULT;
        else if (r < 16000.0f)  s = LightStatus::PRESENT;   // 标准灯 10k
        else if (r < 30000.0f)  s = LightStatus::PRESENT;   // 尾灯 22k
        else                    s = LightStatus::ABSENT;
    }
    if (s != status_) {
        status_ = s;
        apply();
    }
#endif
}

// ---------------------------------------------------------------- 控制

void Light::setBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    brightness_ = pct;
    apply();
}

void Light::setMode(LightMode m) {
    mode_ = m;
    apply();
}

void Light::apply() {
#if MOCK_LIGHT
    Serial.printf("[light] mode=%s brightness=%u%% status=%s\n",
                  name(mode_), brightness_, name(status_));
#else
    uint16_t duty = 0;
    if (status_ == LightStatus::PRESENT) {
        switch (mode_) {
            case LightMode::OFF:   duty = 0; break;
            case LightMode::ON:    duty = (uint16_t)((brightness_ * 1023) / 100); break;
            case LightMode::ALERT: duty = 1023; break;   // 拉满，闪烁由灯自己执行
        }
    }
    ledcWrite(PIN_LIGHT_PWM, duty);
#endif
}
