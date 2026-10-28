/**
 * bike-hub 固件入口
 *
 * 这个文件只做三件事：初始化、调度、事件播报。
 * 任何业务判断都在 lib/ 的模块里，loop() 里不出现"该不该报警"这种逻辑。
 *
 * 模块边界：
 *   lock   锁舌状态（唯一撤防依据）+ 防拆
 *   light  车灯（独立供电，只收 PWM 信号）
 *   guard  防盗状态机（消费 lock + 运动强度）
 *   power  电池
 *
 * 串口命令（回车结束）：
 *   lock / unlock / tamper / clear    模拟锁态与防拆
 *   light off|on|alert                控制车灯
 *   stream on|off                     开关 50 Hz CSV 流
 *   status                            打印当前状态
 *   help                              命令列表
 */

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "guard.h"
#include "light.h"
#include "lis3dh.h"
#include "lock.h"
#include "power.h"

#define FW_VERSION "0.3.0"

static Lis3dh g_imu;
static Lock   g_lock;
static Light  g_light;
static Guard  g_guard;
static Power  g_power;

// ---------------------------------------------------------------- 调度状态

static uint32_t g_boot_ms = 0;
static uint32_t t_imu = 0, t_lock = 0, t_light = 0, t_power = 0, t_beat = 0, t_guard = 0;

// ---------------------------------------------------------------- 运动强度

static const uint8_t MOTION_WIN = 16;   // 50 Hz 下约 0.32 s
static float   g_mag[MOTION_WIN];
static uint8_t g_mag_idx = 0;
static uint8_t g_mag_cnt = 0;
static float   g_rms = 0.0f;

static void push_mag(float g) {
    g_mag[g_mag_idx] = g;
    g_mag_idx = (uint8_t)((g_mag_idx + 1) % MOTION_WIN);
    if (g_mag_cnt < MOTION_WIN) g_mag_cnt++;
}

static float compute_rms() {
    if (g_mag_cnt < 4) return 0.0f;
    float mean = 0.0f;
    for (uint8_t i = 0; i < g_mag_cnt; i++) mean += g_mag[i];
    mean /= g_mag_cnt;
    float var = 0.0f;
    for (uint8_t i = 0; i < g_mag_cnt; i++) {
        const float d = g_mag[i] - mean;
        var += d * d;
    }
    return sqrtf(var / g_mag_cnt);
}

// ---------------------------------------------------------------- 状态缓存

static LockState  g_last_lock  = LockState::UNKNOWN;
static GuardState g_last_guard = GuardState::INIT;
static bool       g_stream = false;

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
        Serial.printf("  PSRAM     : %lu MB\n", (unsigned long)(ESP.getPsramSize() / (1024 * 1024)));
    } else {
        Serial.println("  PSRAM     : 无");
    }
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("  MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
                  (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
    Serial.printf("  复位原因  : %s\n", reset_reason_str());
    Serial.println();
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
    if (found == 0) Serial.println("       总线上没有设备");

    if (!g_imu.beginAuto(Wire)) {
        Serial.printf("[imu] 初始化失败：%s\n", g_imu.error());
        return;
    }
    Serial.printf("[imu] 就绪 0x%02X  WHO_AM_I 0x%02X  ±4g 50Hz\n",
                  g_imu.address(), g_imu.whoAmI());
}

// ---------------------------------------------------------------- 串口命令

static void print_status() {
    Serial.printf("[status] lock=%s guard=%s rms=%.3fg batt=%.2fV/%u%% light=%s/%s\n",
                  Lock::name(g_lock.state()),
                  Guard::name(g_guard.state()),
                  g_rms,
                  g_power.voltage(), g_power.percent(),
                  Light::name(g_light.mode()), Light::name(g_light.status()));
}

static void print_help() {
    Serial.println("[cmd] lock | unlock | tamper | clear");
    Serial.println("[cmd] light off|on|alert");
    Serial.println("[cmd] stream on|off");
    Serial.println("[cmd] status | help");
}

static void handle_cmd(char *line) {
    while (*line == ' ') line++;

    if (strcmp(line, "lock") == 0) {
        g_lock.requestLock();
    } else if (strcmp(line, "unlock") == 0) {
        g_lock.requestUnlock();
    } else if (strcmp(line, "tamper") == 0) {
        g_lock.mockTamper(true);
    } else if (strcmp(line, "clear") == 0) {
        g_lock.mockTamper(false);
    } else if (strcmp(line, "light off") == 0) {
        g_light.setMode(LightMode::OFF);
    } else if (strcmp(line, "light on") == 0) {
        g_light.setBrightness(80);
        g_light.setMode(LightMode::ON);
    } else if (strcmp(line, "light alert") == 0) {
        g_light.setMode(LightMode::ALERT);
    } else if (strcmp(line, "stream on") == 0) {
        g_stream = true;
        Serial.println("# t_ms,ax_mg,ay_mg,az_mg");
    } else if (strcmp(line, "stream off") == 0) {
        g_stream = false;
        Serial.println("[cmd] CSV 流已关闭");
    } else if (strcmp(line, "status") == 0) {
        print_status();
    } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        print_help();
    } else if (line[0] != '\0') {
        Serial.printf("[cmd] 未知命令：%s\n", line);
    }
}

static void poll_serial() {
    static char buf[40];
    static uint8_t len = 0;

    while (Serial.available() > 0) {
        const char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len] = '\0';
            handle_cmd(buf);
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ---------------------------------------------------------------- 调度

static void tick_imu() {
    const uint32_t now = millis();
    if ((int32_t)(now - t_imu) < 0) return;
    t_imu = now + IMU_SAMPLE_MS;

    if (!g_imu.ready() || !g_imu.dataReady()) return;

    int16_t x = 0, y = 0, z = 0;
    if (!g_imu.readRaw(x, y, z)) return;

    const float s = g_imu.mgPerLsb();
    const float ax = x * s, ay = y * s, az = z * s;
    const float mag_g = sqrtf(ax * ax + ay * ay + az * az) / 1000.0f;
    push_mag(mag_g);
    g_rms = compute_rms();

    if (g_stream) {
        Serial.printf("%lu,%d,%d,%d\n", now, (int)ax, (int)ay, (int)az);
    }
}

static void report_changes() {
    if (g_lock.state() != g_last_lock) {
        Serial.printf("[lock] %s -> %s\n",
                      Lock::name(g_last_lock), Lock::name(g_lock.state()));
        g_last_lock = g_lock.state();
    }

    if (g_guard.state() != g_last_guard) {
        Serial.printf("[guard] %s -> %s  (lock=%s rms=%.3fg)\n",
                      Guard::name(g_last_guard), Guard::name(g_guard.state()),
                      Lock::name(g_lock.state()), g_rms);

        switch (g_guard.state()) {
            case GuardState::ALARM: g_light.setMode(LightMode::ALERT); break;
            default:                g_light.setMode(LightMode::OFF);   break;
        }
        g_last_guard = g_guard.state();
    }
}

// ---------------------------------------------------------------- 生命周期

void setup() {
    Serial.begin(115200);
    delay(500);

    g_boot_ms = millis();
    t_imu = t_lock = t_light = t_power = t_beat = t_guard = g_boot_ms;

    print_banner();
    print_chip_info();

    g_power.begin();
    init_imu();
    g_lock.begin();
    g_light.begin();
    g_guard.begin();

    // 用当前锁态决定初始布防状态
    g_last_lock = g_lock.state();
    g_guard.tick(g_lock.state(), g_lock.tampered(), 0.0f);
    g_last_guard = g_guard.state();
    Serial.printf("[guard] 初始状态 %s\n", Guard::name(g_guard.state()));

    Serial.println();
    print_help();
    Serial.println();
}

void loop() {
    const uint32_t now = millis();

    poll_serial();
    tick_imu();

    if ((int32_t)(now - t_lock) >= 0) {
        t_lock = now + LOCK_TICK_MS;
        g_lock.tick();
    }

    if ((int32_t)(now - t_light) >= 0) {
        t_light = now + LIGHT_TICK_MS;
        g_light.tick();
    }

    if ((int32_t)(now - t_power) >= 0) {
        t_power = now + POWER_TICK_MS;
        g_power.tick();
    }

    if ((int32_t)(now - t_guard) >= 0) {
        t_guard = now + GUARD_TICK_MS;
        g_guard.tick(g_lock.state(), g_lock.tampered(), g_rms);
    }

    report_changes();

    if ((int32_t)(now - t_beat) >= 0) {
        t_beat = now + HEARTBEAT_MS;
        Serial.printf("# [beat] up=%lus heap=%lu guard=%s batt=%u%%\n",
                      (unsigned long)((now - g_boot_ms) / 1000),
                      (unsigned long)ESP.getFreeHeap(),
                      Guard::name(g_guard.state()),
                      g_power.percent());
    }

    delay(1);
}
