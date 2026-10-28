#include "alarm.h"
#include "config.h"

#if !MOCK_ALARM
#include <LittleFS.h>
#include <ESP_I2S.h>
#endif

namespace {
constexpr uint32_t WAV_HEADER_BYTES = 44;   // 标准 PCM WAV 头
constexpr size_t   CHUNK_BYTES = 512;       // 每次 tick 喂给 I2S 的量
}

#if !MOCK_ALARM
static I2SClass   s_i2s;
static File       s_file;
static uint8_t    s_buf[CHUNK_BYTES];
static bool       s_i2s_ready = false;
#endif

// ---------------------------------------------------------------- 名称

const char *Alarm::name(AlarmState s) {
    switch (s) {
        case AlarmState::IDLE:    return "IDLE";
        case AlarmState::PLAYING: return "PLAYING";
    }
    return "?";
}

// ---------------------------------------------------------------- 初始化

bool Alarm::begin() {
    state_ = AlarmState::IDLE;
    id_ = 0;
    loop_ = false;

#if MOCK_ALARM
    Serial.println("[alarm] Mock 模式（串口命令：play N / stop）");
#else
    pinMode(PIN_AMP_SD, OUTPUT);
    digitalWrite(PIN_AMP_SD, LOW);   // 默认关断，避免上电爆音

    if (!LittleFS.begin(true)) {
        Serial.println("[alarm] LittleFS 挂载失败");
        return false;
    }
    Serial.printf("[alarm] LittleFS 就绪，总 %lu KB 已用 %lu KB\n",
                  (unsigned long)(LittleFS.totalBytes() / 1024),
                  (unsigned long)(LittleFS.usedBytes() / 1024));

    s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
    if (!s_i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_HZ,
                     I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.printf("[alarm] I2S 初始化失败 (%d)\n", s_i2s.lastError());
        return false;
    }
    s_i2s_ready = true;
#endif
    return true;
}

// ---------------------------------------------------------------- 控制

bool Alarm::play(uint8_t id, bool loop) {
    if (id == 0) return false;

#if MOCK_ALARM
    id_ = id;
    loop_ = loop;
    // 每段音频按 3 秒模拟，方便观察状态
    mock_end_ms_ = millis() + (loop ? 0xFFFFFFFFu : 3000u);
    state_ = AlarmState::PLAYING;
    Serial.printf("[alarm] PLAY id=%u loop=%d\n", id_, (int)loop_);
    return true;
#else
    stop();

    char path[12];
    snprintf(path, sizeof(path), "/a%u.wav", id);
    s_file = LittleFS.open(path, "r");
    if (!s_file) {
        Serial.printf("[alarm] 打不开 %s（用 uploadfs 烧过音频吗？）\n", path);
        return false;
    }
    if (s_file.size() <= WAV_HEADER_BYTES) {
        Serial.printf("[alarm] %s 太小，不是有效 WAV\n", path);
        s_file.close();
        return false;
    }
    s_file.seek(WAV_HEADER_BYTES);

    digitalWrite(PIN_AMP_SD, HIGH);
    id_ = id;
    loop_ = loop;
    state_ = AlarmState::PLAYING;
    Serial.printf("[alarm] PLAY %s loop=%d\n", path, (int)loop_);
    return true;
#endif
}

void Alarm::stop() {
    if (state_ == AlarmState::IDLE) return;
    state_ = AlarmState::IDLE;

#if MOCK_ALARM
    Serial.printf("[alarm] STOP id=%u\n", id_);
#else
    if (s_file) s_file.close();
    if (s_i2s_ready) {
        // 补一段静音，避免关断瞬间的爆音
        memset(s_buf, 0, sizeof(s_buf));
        s_i2s.write(s_buf, sizeof(s_buf));
    }
    digitalWrite(PIN_AMP_SD, LOW);
    Serial.printf("[alarm] STOP id=%u\n", id_);
#endif
    id_ = 0;
    loop_ = false;
}

// ---------------------------------------------------------------- 周期

void Alarm::tick() {
    if (state_ != AlarmState::PLAYING) return;
#if MOCK_ALARM
    runMock();
#else
    runHardware();
#endif
}

void Alarm::runMock() {
    if (loop_) return;   // 循环模式下一直播放，等 stop()
    if ((int32_t)(millis() - mock_end_ms_) >= 0) {
        stop();
    }
}

#if !MOCK_ALARM
void Alarm::runHardware() {
    if (!s_file.available()) {
        if (loop_) {
            s_file.seek(WAV_HEADER_BYTES);   // 循环：回到数据起点
        } else {
            stop();
            return;
        }
    }

    const size_t n = s_file.read(s_buf, CHUNK_BYTES);
    if (n == 0) return;

    // write 在半满时会阻塞很短时间（DMA 吸收），单次约几毫秒
    s_i2s.write(s_buf, n);
}
#else
void Alarm::runHardware() {}
#endif
