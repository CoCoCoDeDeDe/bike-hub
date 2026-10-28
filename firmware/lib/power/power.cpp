#include "power.h"
#include "config.h"

// 单节锂电的粗略映射：3.3 V ≈ 0%，4.2 V ≈ 100%
static uint8_t voltage_to_percent(float v) {
    if (v >= 4.15f) return 100;
    if (v <= 3.30f) return 0;
    return (uint8_t)((v - 3.30f) / (4.15f - 3.30f) * 100.0f);
}

const char *Power::levelName(uint8_t percent) {
    if (percent <= 5)  return "CRITICAL";
    if (percent <= 15) return "LOW";
    if (percent <= 40) return "OK";
    return "GOOD";
}

bool Power::begin() {
#if MOCK_POWER
    voltage_ = 3.95f;
    percent_ = voltage_to_percent(voltage_);
    Serial.println("[power] Mock 模式（固定 3.95 V）");
#else
    analogReadResolution(12);
    Serial.println("[power] 电池 ADC 就绪");
#endif
    return true;
}

void Power::tick() {
    const uint32_t now = millis();
    if (now - last_ < POWER_TICK_MS) return;
    last_ = now;

#if MOCK_POWER
    return;
#else
    // 多次采样取平均，抑制 ADC 噪声
    uint32_t acc = 0;
    for (uint8_t i = 0; i < 8; i++) acc += analogRead(PIN_BATT_ADC);
    const float raw = acc / 8.0f;
    voltage_ = raw * 3.3f / 4095.0f * BATT_DIVIDER;
    percent_ = voltage_to_percent(voltage_);
#endif
}
