#pragma once

/**
 * 板级配置：引脚、采样参数、Mock 开关
 *
 * 一体机阶段硬件还没定型，所以锁 / 灯 / 电源都带 Mock 开关。
 * 机械做好后把对应的开关改成 0，只改这一处。
 */

// ---------------------------------------------------------------- 编译开关
// 1 = 用串口命令模拟，0 = 走真实硬件
// 用 #ifndef 包裹，方便命令行 -DMOCK_XXX=0 做编译验证
#ifndef MOCK_LOCK
#define MOCK_LOCK   1
#endif
#ifndef MOCK_LIGHT
#define MOCK_LIGHT  1
#endif
#ifndef MOCK_POWER
#define MOCK_POWER  1
#endif
#ifndef MOCK_ALARM
#define MOCK_ALARM  1
#endif

// ---------------------------------------------------------------- I2C（LIS3DH）
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9
#define I2C_CLOCK_HZ    400000

// ---------------------------------------------------------------- 锁
#define PIN_LOCK_SENSE   4    // 锁舌霍尔：闭合 = LOW
#define PIN_LOCK_MOTOR_A 5    // 电机 H 桥 A
#define PIN_LOCK_MOTOR_B 6    // 电机 H 桥 B
#define PIN_TAMPER       7    // 开盖检测：常闭回路，断开 = 被拆
#define LOCK_MOTOR_MS    400  // 电机通电时长

// ---------------------------------------------------------------- 车灯
#define PIN_LIGHT_PWM    10
#define PIN_LIGHT_ID     1    // ADC，读 ID 电阻判断灯是否在位
#define LIGHT_ID_R_STD   10000
#define LIGHT_ID_R_TAIL  22000

// ---------------------------------------------------------------- 电源
#define PIN_BATT_ADC     2
#define BATT_DIVIDER     2.0f   // 100k / 100k 分压

// ---------------------------------------------------------------- 调度周期
#define IMU_SAMPLE_HZ    50
#define IMU_SAMPLE_MS    (1000 / IMU_SAMPLE_HZ)
#define GUARD_TICK_MS    100
#define LOCK_TICK_MS     20
#define LIGHT_TICK_MS    100
#define POWER_TICK_MS    1000
#define HEARTBEAT_MS     5000

// ---------------------------------------------------------------- 判定阈值（待实测校准）
#define MOTION_RMS_G          0.05f   // 超过算"在动"
#define MOTION_HOLD_MS        1500    // 持续这么久才算事件
#define ALARM_COOLDOWN_MS     10000   // 告警后静默，避免刷屏

// ---------------------------------------------------------------- 喇叭（MAX98357A）
#define PIN_I2S_BCLK     15
#define PIN_I2S_LRC      16
#define PIN_I2S_DIN      17
#define PIN_AMP_SD       18    // 功放使能，低 = 关断
#define AUDIO_SAMPLE_HZ  16000
#define ALARM_TICK_MS    10

// ---------------------------------------------------------------- 预留：唤醒
// #define PIN_IMU_INT1    3     // LIS3DH INT1，低功耗中断唤醒

// ---------------------------------------------------------------- 预留：码表 / 轮速
//
// 码表功能暂搁置（需求未定），但硬件和代码结构都留了接入点。
// 打开方式见 firmware/README.md「未来扩展点」。
//
// #define PIN_HALL_WHEEL   11    // 车轮霍尔，必须在 GPIO0–21 内（RTC，可深睡唤醒）
// #define WHEEL_CIRC_MM    2146  // 轮周长：700×25c ≈ 2146，700×28c ≈ 2155
// #define WHEEL_DEBOUNCE_MS 30   // 脉冲去抖窗口（30 ms 约合 250 km/h，正常骑行不可能更快）
// #define WHEEL_STOP_MS    2000  // 超过这么久没脉冲判定为停止
