/**
 * LIS3DH 驱动实现
 *
 * 寄存器读写的两个约定：
 *   1. I2C 读操作要把地址最高位置 1（0x80）
 *   2. LIS3DH 支持地址自动递增，连续读 OUT_X_L 起始的 6 字节即可拿到 XYZ
 */

#include "lis3dh.h"

namespace {

// ---- 寄存器 ----
constexpr uint8_t REG_STATUS      = 0x27;
constexpr uint8_t REG_OUT_X_L     = 0x28;
constexpr uint8_t REG_WHO_AM_I    = 0x0F;
constexpr uint8_t REG_CTRL_REG1   = 0x20;
constexpr uint8_t REG_CTRL_REG2   = 0x21;
constexpr uint8_t REG_CTRL_REG4   = 0x23;
constexpr uint8_t REG_CTRL_REG5   = 0x24;
constexpr uint8_t REG_CTRL_REG6   = 0x25;
constexpr uint8_t REG_FIFO_CTRL   = 0x2E;
constexpr uint8_t REG_FIFO_SRC    = 0x2F;

constexpr uint8_t WHO_AM_I_VALUE  = 0x33;

// CTRL_REG1 位
constexpr uint8_t CR1_XEN         = 0x01;
constexpr uint8_t CR1_YEN         = 0x02;
constexpr uint8_t CR1_ZEN         = 0x04;
constexpr uint8_t CR1_LPEN        = 0x08;

// CTRL_REG4 位
constexpr uint8_t CR4_BDU         = 0x80;  // 读期间不更新，保证 XYZ 一致
constexpr uint8_t CR4_HR          = 0x08;  // 12 bit 高分辨率
constexpr uint8_t CR4_FS_SHIFT    = 4;

// STATUS_REG 位
constexpr uint8_t STATUS_ZYXDA    = 0x08;

// FIFO_SRC 位
constexpr uint8_t FIFO_SRC_WTM    = 0x80;
constexpr uint8_t FIFO_SRC_OVR    = 0x40;
constexpr uint8_t FIFO_SRC_FSS    = 0x1F;

constexpr uint8_t I2C_READ_FLAG   = 0x80;

}  // namespace

// ---------------------------------------------------------------- 底层读写

bool Lis3dh::writeReg(uint8_t reg, uint8_t val) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(val);
    return wire_->endTransmission() == 0;
}

bool Lis3dh::readRegs(uint8_t reg, uint8_t *buf, size_t len) {
    wire_->beginTransmission(addr_);
    wire_->write((uint8_t)(reg | I2C_READ_FLAG));
    if (wire_->endTransmission(false) != 0) return false;

    size_t got = wire_->requestFrom((int)addr_, (int)len);
    if (got != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)wire_->read();
    return true;
}

bool Lis3dh::readReg(uint8_t reg, uint8_t &val) {
    return readRegs(reg, &val, 1);
}

void Lis3dh::fail(const char *msg) {
    err_ = msg;
    ready_ = false;
}

// ---------------------------------------------------------------- 初始化

bool Lis3dh::begin(TwoWire &wire, uint8_t addr) {
    wire_ = &wire;
    addr_ = addr;
    ready_ = false;
    err_ = "";

    if (!readReg(REG_WHO_AM_I, who_)) {
        fail("I2C 无响应（检查接线、上拉、供电）");
        return false;
    }
    if (who_ != WHO_AM_I_VALUE) {
        fail("WHO_AM_I 不匹配（可能是 ADXL345 等其他芯片）");
        return false;
    }

    // 先关掉输出，配置期间数据无意义
    if (!writeReg(REG_CTRL_REG1, 0x00)) { fail("CTRL_REG1 写入失败"); return false; }

    if (!setRange(Range::G4)) return false;
    if (!setOdr(Odr::HZ50))   return false;

    // 三轴使能
    uint8_t cr1 = 0;
    if (!readReg(REG_CTRL_REG1, cr1)) { fail("CTRL_REG1 读取失败"); return false; }
    cr1 |= (CR1_XEN | CR1_YEN | CR1_ZEN);
    if (!writeReg(REG_CTRL_REG1, cr1)) { fail("CTRL_REG1 写入失败"); return false; }

    ready_ = true;
    return true;
}

bool Lis3dh::beginAuto(TwoWire &wire) {
    // SA0 接地是 0x18，接 VCC 是 0x19
    if (begin(wire, 0x18)) return true;
    if (begin(wire, 0x19)) return true;
    return false;
}

// ---------------------------------------------------------------- 配置

bool Lis3dh::setRange(Range r) {
    uint8_t cr4 = 0;
    if (!readReg(REG_CTRL_REG4, cr4)) { fail("CTRL_REG4 读取失败"); return false; }

    cr4 &= (uint8_t)~0x30;                       // 清 FS[5:4]
    cr4 |= (uint8_t)((uint8_t)r << CR4_FS_SHIFT);
    cr4 |= CR4_BDU | CR4_HR;                     // 12 bit + 一致读

    if (!writeReg(REG_CTRL_REG4, cr4)) { fail("CTRL_REG4 写入失败"); return false; }
    range_ = r;
    return true;
}

bool Lis3dh::setOdr(Odr odr) {
    uint8_t cr1 = 0;
    if (!readReg(REG_CTRL_REG1, cr1)) { fail("CTRL_REG1 读取失败"); return false; }

    cr1 &= 0x0F;                                 // 清 ODR[7:4]，保留低 4 位
    cr1 |= (uint8_t)((uint8_t)odr << 4);

    if (!writeReg(REG_CTRL_REG1, cr1)) { fail("CTRL_REG1 写入失败"); return false; }
    return true;
}

bool Lis3dh::setLowPower(bool on) {
    uint8_t cr1 = 0;
    if (!readReg(REG_CTRL_REG1, cr1)) return false;
    if (on) cr1 |= CR1_LPEN;
    else    cr1 &= (uint8_t)~CR1_LPEN;
    return writeReg(REG_CTRL_REG1, cr1);
}

bool Lis3dh::setInt1ActiveHigh(bool high) {
    uint8_t cr6 = 0;
    if (!readReg(REG_CTRL_REG6, cr6)) return false;
    // H_LACTIVE = 1 表示低电平有效，所以 high 时清位
    if (high) cr6 &= (uint8_t)~0x02;
    else      cr6 |= 0x02;
    return writeReg(REG_CTRL_REG6, cr6);
}

// ---------------------------------------------------------------- 采样

bool Lis3dh::dataReady() {
    uint8_t st = 0;
    if (!readReg(REG_STATUS, st)) return false;
    return (st & STATUS_ZYXDA) != 0;
}

bool Lis3dh::readRaw(int16_t &x, int16_t &y, int16_t &z) {
    uint8_t buf[6];
    if (!readRegs(REG_OUT_X_L, buf, sizeof(buf))) return false;

    // 高分辨率模式下数据左对齐，右移 4 位得到 12 bit 有符号值
    x = (int16_t)((int16_t)((buf[1] << 8) | buf[0]) >> 4);
    y = (int16_t)((int16_t)((buf[3] << 8) | buf[2]) >> 4);
    z = (int16_t)((int16_t)((buf[5] << 8) | buf[4]) >> 4);
    return true;
}

bool Lis3dh::readG(float &x, float &y, float &z) {
    int16_t rx = 0, ry = 0, rz = 0;
    if (!readRaw(rx, ry, rz)) return false;
    const float s = mgPerLsb() / 1000.0f;
    x = rx * s;
    y = ry * s;
    z = rz * s;
    return true;
}

float Lis3dh::mgPerLsb() const {
    switch (range_) {
        case Range::G2:  return 1.0f;
        case Range::G4:  return 2.0f;
        case Range::G8:  return 4.0f;
        case Range::G16: return 12.0f;
    }
    return 2.0f;
}

// ---------------------------------------------------------------- FIFO

bool Lis3dh::fifoEnable(uint8_t watermark) {
    // CTRL_REG5.FIFO_EN = bit 6
    uint8_t cr5 = 0;
    if (!readReg(REG_CTRL_REG5, cr5)) return false;
    if (!writeReg(REG_CTRL_REG5, (uint8_t)(cr5 | 0x40))) return false;

    // FM = 01（FIFO 模式），FTH = 水位
    return writeReg(REG_FIFO_CTRL, (uint8_t)(0x40 | (watermark & 0x1F)));
}

bool Lis3dh::fifoDisable() {
    uint8_t cr5 = 0;
    if (!readReg(REG_CTRL_REG5, cr5)) return false;
    writeReg(REG_FIFO_CTRL, 0x00);  // Bypass 模式
    return writeReg(REG_CTRL_REG5, (uint8_t)(cr5 & (uint8_t)~0x40));
}

uint8_t Lis3dh::fifoCount() {
    uint8_t src = 0;
    if (!readReg(REG_FIFO_SRC, src)) return 0;
    if (src & FIFO_SRC_OVR) return FIFO_SRC_FSS;  // 溢出时按满处理
    return (uint8_t)(src & FIFO_SRC_FSS);
}

bool Lis3dh::readFifoRaw(int16_t *out, uint8_t maxSamples, uint8_t &got) {
    got = 0;
    uint8_t n = fifoCount();
    if (n == 0) return true;
    if (n > maxSamples) n = maxSamples;

    for (uint8_t i = 0; i < n; i++) {
        int16_t x = 0, y = 0, z = 0;
        if (!readRaw(x, y, z)) return false;
        out[got * 3 + 0] = x;
        out[got * 3 + 1] = y;
        out[got * 3 + 2] = z;
        got++;
    }
    return true;
}
