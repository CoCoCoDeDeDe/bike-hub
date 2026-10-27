#pragma once

/**
 * LIS3DH 三轴加速度计驱动
 *
 * 设计目标：
 *   - 50 Hz 连续采样，输出原始值（mg），给上层做特征分析
 *   - 配置简单、可读，不过度抽象
 *   - 出错时给明确原因，方便对波形排查
 *
 * 默认配置：±4 g、50 Hz、12 bit 高分辨率、三轴使能
 * 为什么是 ±4 g：撬锁冲击经常超过 2 g，±2 g 会削顶，特征就毁了
 *
 * 数据手册：ST LIS3DH DS
 */

#include <Arduino.h>
#include <Wire.h>

class Lis3dh {
public:
    enum class Range : uint8_t { G2 = 0, G4 = 1, G8 = 2, G16 = 3 };

    enum class Odr : uint8_t {
        PWR_DOWN = 0, HZ1 = 1, HZ10 = 2, HZ25 = 3, HZ50 = 4,
        HZ100 = 5, HZ200 = 6, HZ400 = 7, HZ1620 = 8, HZ5376 = 9
    };

    // ---- 初始化 ----
    // 指定地址初始化；addr 传 0x18 或 0x19
    bool begin(TwoWire &wire, uint8_t addr = 0x18);
    // 自动探测 0x18 / 0x19，成功返回 true
    bool beginAuto(TwoWire &wire);

    bool ready() const { return ready_; }
    uint8_t address() const { return addr_; }
    uint8_t whoAmI() const { return who_; }
    const char *error() const { return err_; }

    // ---- 配置 ----
    bool setRange(Range r);
    bool setOdr(Odr odr);
    bool setLowPower(bool on);        // 低功耗模式（8 bit），参数与 12 bit 不同
    bool setInt1ActiveHigh(bool high); // INT1 极性，默认高电平有效
    Range range() const { return range_; }

    // ---- 采样 ----
    bool dataReady();
    bool readRaw(int16_t &x, int16_t &y, int16_t &z);   // 12 bit 有符号原始值
    bool readG(float &x, float &y, float &z);           // 换算成 g
    float mgPerLsb() const;

    // ---- FIFO（预留：低功耗批量唤醒用）----
    bool fifoEnable(uint8_t watermark);
    bool fifoDisable();
    uint8_t fifoCount();
    bool readFifoRaw(int16_t *out, uint8_t maxSamples, uint8_t &got);

private:
    bool writeReg(uint8_t reg, uint8_t val);
    bool readRegs(uint8_t reg, uint8_t *buf, size_t len);
    bool readReg(uint8_t reg, uint8_t &val);
    void fail(const char *msg);

    TwoWire *wire_ = nullptr;
    uint8_t addr_ = 0x18;
    uint8_t who_ = 0;
    Range range_ = Range::G4;
    bool ready_ = false;
    const char *err_ = "";
};
