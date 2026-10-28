#pragma once

/**
 * 喇叭模块
 *
 * 播放 LittleFS 里的预制录音（/a1.wav … /a9.wav），支持循环。
 *
 * 音频格式必须统一：16 kHz / 16 bit / 单声道 PCM WAV。
 * 转换命令：
 *   ffmpeg -i in.mp3 -ar 16000 -ac 1 -sample_fmt s16 -t 8 a1.wav
 *
 * 播放是流式的：每 tick 从文件读一块喂给 I2S，不会一口气阻塞几秒。
 * 这样报警时 IMU 采样和状态机照常运行。
 *
 * MOCK_ALARM=1 时不碰硬件，用虚拟时间模拟播放时长。
 */

#include <Arduino.h>

enum class AlarmState : uint8_t {
    IDLE = 0,
    PLAYING
};

class Alarm {
public:
    bool begin();
    void tick();

    /// 播放编号 1–9 的音频；loop=true 时循环直到 stop()
    bool play(uint8_t id, bool loop = false);
    void stop();

    bool busy() const { return state_ == AlarmState::PLAYING; }
    AlarmState state() const { return state_; }
    uint8_t currentId() const { return id_; }

    static const char *name(AlarmState s);

private:
    void runMock();
    void runHardware();

    AlarmState state_ = AlarmState::IDLE;
    uint8_t id_ = 0;
    bool loop_ = false;
    uint32_t mock_end_ms_ = 0;
};
