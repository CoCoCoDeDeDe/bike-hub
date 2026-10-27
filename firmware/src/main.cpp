/**
 * bike-hub 固件骨架
 *
 * 当前阶段：验证「编译 → 烧录 → 串口」链路 + LIS3DH 连续采样。
 * 还没有判定逻辑、没有 4G、没有音频。
 *
 * 串口输出格式（方便直接喂给 pandas）：
 *   启动信息    普通文本
 *   CSV 流      t_ms,ax_mg,ay_mg,az_mg
 *   心跳        # [beat] ...            以 # 开头，读取时会被当注释跳过
 *
 * Python 侧：
 *   pd.read_csv(StringIO(text), comment='#', names=['t','ax','ay','az'])
 */

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "lis3dh.h"

#define FW_VERSION "0.2.0"

static Lis3dh g_imu;
static uint32_t g_boot_ms = 0;
static uint32_t g_next_sample = 0;
static uint32_t g_last_beat = 0;
static uint32_t g_dropped = 0;

// ---------------------------------------------------------------- 工具

static const char *reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "上电";
        case ESP_RST_EXT:       return "外部复位";
        case ESP_RST_SW:        return "软件复位";
        case ESP_RST_PANIC:     return "异常崩溃";
        case ESP_RST_INT_WDT:   return "中断看门狗";
        case ESP_RST_TASK_WDT:  return "任务看门狗";
        case ESP_RST_WDT:       return "看门狗";
        case ESP_RST_DEEPSLEEP: return "深睡唤醒";
        case ESP_RST_BROWNOUT:  return "欠压复位";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "未知";
    }
}

static void print_banner() {
    Serial.println();
    Serial.println("========================================");
    Serial.printf("  bike-hub  firmware %s\n", FW_VERSION);
    Serial.printf("  built %s %s\n", __DATE__, __TIME__);
    Serial.println("========================================");
    Serial.println();
}

static void print_chip_info() {
    Serial.println("[chip]");
    Serial.printf("  型号      : %s rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("  核心      : %d @ %lu MHz\n", ESP.getChipCores(), (unsigned long)getCpuFrequencyMhz());
    Serial.printf("  Flash     : %lu MB\n", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
    if (psramFound()) {
        Serial.printf("  PSRAM     : %lu MB (空闲 %lu KB)\n",
                      (unsigned long)(ESP.getPsramSize() / (1024 * 1024)),
                      (unsigned long)(ESP.getFreePsram() / 1024));
    } else {
        Serial.println("  PSRAM     : 无（N8 版本，音频缓冲会受限制）");
    }
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("  MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
                  (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
    Serial.printf("  复位原因  : %s\n", reset_reason_str());
    Serial.println();
}

static const char *range_str(Lis3dh::Range r) {
    switch (r) {
        case Lis3dh::Range::G2:  return "±2g";
        case Lis3dh::Range::G4:  return "±4g";
        case Lis3dh::Range::G8:  return "±8g";
        case Lis3dh::Range::G16: return "±16g";
    }
    return "?";
}

static void init_imu() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);

    Serial.println("[imu] 扫描 I2C 总线…");
    uint8_t found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("       发现设备 0x%02X\n", a);
            found++;
        }
    }
    if (found == 0) {
        Serial.println("       总线上没有任何设备（检查 SDA/SCL 接线与供电）");
    }

    if (!g_imu.beginAuto(Wire)) {
        Serial.printf("[imu] 初始化失败：%s\n", g_imu.error());
        Serial.println("[imu] 先只跑心跳，接好传感器后重新上电");
        return;
    }

    Serial.printf("[imu] 就绪  地址 0x%02X  WHO_AM_I 0x%02X\n", g_imu.address(), g_imu.whoAmI());
    Serial.printf("[imu] 量程 %s  灵敏度 %.1f mg/LSB  采样 %d Hz\n",
                  range_str(g_imu.range()), g_imu.mgPerLsb(), IMU_SAMPLE_HZ);
}

// ---------------------------------------------------------------- 生命周期

void setup() {
    Serial.begin(115200);
    delay(500);  // 等 USB 枚举/串口稳定

    g_boot_ms = millis();
    g_last_beat = 0;
    g_next_sample = g_boot_ms;

    print_banner();
    print_chip_info();
    init_imu();

    Serial.println();
    Serial.println("# bike-hub imu stream — 断电结束");
    Serial.println("# t_ms,ax_mg,ay_mg,az_mg");
}

void loop() {
    const uint32_t now = millis();

    // ---- 50 Hz 采样 ----
    if (g_imu.ready() && (int32_t)(now - g_next_sample) >= 0) {
        g_next_sample = now + IMU_SAMPLE_MS;

        if (g_imu.dataReady()) {
            int16_t x = 0, y = 0, z = 0;
            if (g_imu.readRaw(x, y, z)) {
                const float s = g_imu.mgPerLsb();
                Serial.printf("%lu,%d,%d,%d\n", now,
                              (int)(x * s), (int)(y * s), (int)(z * s));
            }
        } else {
            g_dropped++;  // 采样点没跟上，通常是 I2C 太慢或被日志阻塞
        }
    }

    // ---- 心跳（以 # 开头，不破坏 CSV）----
    if (now - g_last_beat >= HEARTBEAT_MS) {
        g_last_beat = now;
        Serial.printf("# [beat] up=%lus heap=%lu  min=%lu  drop=%lu\n",
                      (unsigned long)((now - g_boot_ms) / 1000),
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMinFreeHeap(),
                      (unsigned long)g_dropped);
    }

    delay(1);  // 让出 CPU
}
