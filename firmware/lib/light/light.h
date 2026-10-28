#pragma once

/**
 * 车灯模块
 *
 * 车灯自带电池、可随时拆下充电，所以这里只输出控制信号，不给它供电。
 *
 * 三种模式：
 *   OFF    关
 *   ON     常亮，亮度由 setBrightness 决定
 *   ALERT  警报闪烁（由灯自己执行，hub 只给一个"拉满"的信号）
 *
 * ID 针用来判断灯在不在、是哪种灯——拆下充电属于正常，读到短路才报故障。
 */

#include <Arduino.h>

enum class LightMode : uint8_t { OFF = 0, ON, ALERT };
enum class LightStatus : uint8_t { ABSENT = 0, PRESENT, FAULT };

class Light {
public:
    bool begin();
    void tick();

    LightStatus status() const { return status_; }
    LightMode mode() const { return mode_; }
    uint8_t brightness() const { return brightness_; }

    void setBrightness(uint8_t pct);   // 0–100
    void setMode(LightMode m);

    static const char *name(LightMode m);
    static const char *name(LightStatus s);

private:
    void apply();

    LightStatus status_ = LightStatus::ABSENT;
    LightMode mode_ = LightMode::OFF;
    uint8_t brightness_ = 100;
    uint32_t last_id_check_ = 0;
};
