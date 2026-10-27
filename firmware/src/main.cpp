/**
 * bike-hub 固件骨架
 *
 * 当前阶段的目标只有一个：验证「编译 → 烧录 → 串口回显」这条链路。
 * 还没有传感器、没有 4G、没有音频，只有启动信息和心跳。
 *
 * 后面每接一个外设，就在 lib/ 下加一个模块，在这里注册初始化，
 * 保持 loop() 里只做调度、不写业务逻辑。
 */

#include <Arduino.h>

#define FW_VERSION "0.1.0"
#define HEARTBEAT_MS 2000

static uint32_t g_boot_ms = 0;
static uint32_t g_last_beat = 0;

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
    Serial.printf("  SDK       : %s\n", ESP.getSdkVersion());
    Serial.printf("  MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
                  (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
    Serial.printf("  复位原因  : %s\n", reset_reason_str());
    Serial.println();
}

// ---------------------------------------------------------------- 生命周期

void setup() {
    Serial.begin(115200);
    delay(500);  // 等 USB 枚举/串口稳定

    g_boot_ms = millis();
    g_last_beat = 0;

    print_banner();
    print_chip_info();

    Serial.println("[boot] 骨架就绪，等待接入外设");
    Serial.printf("[boot] 心跳间隔 %d ms\n\n", HEARTBEAT_MS);
}

void loop() {
    if (millis() - g_last_beat >= HEARTBEAT_MS) {
        g_last_beat = millis();

        uint32_t up_s = (millis() - g_boot_ms) / 1000;
        Serial.printf("[beat] up=%lus  heap=%lu  psram=%lu  min_heap=%lu\n",
                      (unsigned long)up_s,
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)(psramFound() ? ESP.getFreePsram() : 0),
                      (unsigned long)ESP.getMinFreeHeap());
    }

    delay(10);  // 让出 CPU，避免空转占满一个核
}
