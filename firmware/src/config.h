#pragma once

/**
 * 板级配置：引脚与采样参数
 *
 * 接线是按原型阶段的实际连线写的，换板子时只改这里。
 */

// ---- I2C（LIS3DH）----
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9
#define I2C_CLOCK_HZ    400000

// ---- 采样 ----
#define IMU_SAMPLE_HZ   50
#define IMU_SAMPLE_MS   (1000 / IMU_SAMPLE_HZ)
#define HEARTBEAT_MS    5000

// ---- 预留给后续外设 ----
// #define PIN_IMU_INT1    7    // LIS3DH INT1，硬件唤醒用
// #define PIN_HALL_WHEEL  5    // 车轮霍尔
// #define PIN_HALL_LID    6    // 开盖霍尔
// #define PIN_AMP_SD      4    // MAX98357A 使能
// #define PIN_BATT_ADC    1    // 电池分压
