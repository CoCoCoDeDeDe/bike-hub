#pragma once

/**
 * 电源模块
 *
 * 一体机的电池是封装的、不可拆，只能通过磁吸口充电。
 * 所以这里只做三件事：读电压、估电量、报低电。
 */

#include <Arduino.h>

class Power {
public:
    bool begin();
    void tick();

    float voltage() const { return voltage_; }
    uint8_t percent() const { return percent_; }
    bool low() const { return percent_ <= 15; }
    bool critical() const { return percent_ <= 5; }

    static const char *levelName(uint8_t percent);

private:
    float voltage_ = 0.0f;
    uint8_t percent_ = 0;
    uint32_t last_ = 0;
};
