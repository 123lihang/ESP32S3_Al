#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define TAG "audio_player"

// 严格匹配手册的I2S引脚
#define I2S_NUM         I2S_NUM_0
#define I2S_BCK_PIN     36
#define I2S_WS_PIN      35
#define I2S_DATA_PIN    37
#define SAMPLE_RATE     16000

// 小块播放缓冲区大小（单次仅4KB内存，绝对不会分配失败）
#define BUF_SAMPLES     1024

// I2S TX通道句柄
static i2s_chan_handle_t tx_chan = NULL;
static bool s_i2s_enabled = false;

void audio_i2s_init(void)
{
    // 清理已有通道，避免重复初始化
    if (tx_chan != NULL) {
        if (s_i2s_enabled) {
            i2s_channel_disable(tx_chan);
            s_i2s_enabled = false;
        }
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }

    /************************** 1. I2S通道基础配置 **************************/
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.intr_priority = 3;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = BUF_SAMPLES;
    chan_cfg.auto_clear_before_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));
    ESP_LOGI(TAG, "I2S channel created");

    /************************** 2. 精准匹配板载功放的I2S时序 **************************/
    i2s_std_config_t std_cfg = {
        // 时钟配置：16KHz采样率，精准匹配功放要求
        .clk_cfg = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = SAMPLE_RATE,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        // 槽位配置：标准飞利浦I2S格式，16位双声道，国产功放通用兼容
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        // 引脚配置：严格匹配手册
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DATA_PIN,
            .din  = GPIO_NUM_NC,
        },
    };
    // 双声道输出，左右声道相同数据，确保功放稳定锁存
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = 16;

    /************************** 3. 初始化I2S通道 **************************/
    esp_err_t ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "I2S init done, matched SC01 PLUS amplifier");
}

void audio_play_test_tone(void)
{
    if (tx_chan == NULL) {
        ESP_LOGE(TAG, "I2S channel not initialized!");
        return;
    }

    // 使能I2S通道
    if (!s_i2s_enabled) {
        esp_err_t ret = i2s_channel_enable(tx_chan);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(ret));
            return;
        }
        s_i2s_enabled = true;
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(TAG, "I2S channel enabled");
    }

    // 测试音参数：2kHz正弦波，3秒时长，大音量
    ESP_LOGI(TAG, "===== Start playing 2kHz test tone, 3 seconds =====");
    const int16_t amplitude = 30000;  // 最大音量
    const int tone_freq = 2000;        // 人耳敏感频段
    const int duration_ms = 3000;      // 播放3秒
    const int total_loop = (SAMPLE_RATE * duration_ms) / 1000 / BUF_SAMPLES;

    /************************** 核心：小块循环播放，仅4KB内存 **************************/
    // 双声道缓冲区，单次仅 1024 * 2 * 2 = 4096字节 = 4KB
    int16_t *audio_buf = heap_caps_malloc(BUF_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    // 兜底：内部内存不够就用PSRAM
    if (audio_buf == NULL) {
        ESP_LOGW(TAG, "Internal memory not enough, try PSRAM");
        audio_buf = heap_caps_malloc(BUF_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (audio_buf == NULL) {
        ESP_LOGE(TAG, "Audio buffer malloc failed!");
        return;
    }
    ESP_LOGI(TAG, "Audio buffer malloc success, size: %d bytes", BUF_SAMPLES * 2 * sizeof(int16_t));

    // 循环生成并播放音频
    size_t written_bytes = 0;
    int sample_offset = 0;
    for (int loop = 0; loop < total_loop; loop++) {
        // 生成当前块的正弦波数据
        for (int i = 0; i < BUF_SAMPLES; i++) {
            float rad = 2 * 3.1415926f * tone_freq * (sample_offset + i) / SAMPLE_RATE;
            int16_t sample = (int16_t)(amplitude * sinf(rad));
            audio_buf[i*2] = sample;    // 左声道
            audio_buf[i*2 + 1] = sample;// 右声道
        }
        sample_offset += BUF_SAMPLES;

        // 写入I2S播放
        esp_err_t ret = i2s_channel_write(tx_chan, audio_buf, BUF_SAMPLES * 2 * sizeof(int16_t), &written_bytes, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed at loop %d: %s", loop, esp_err_to_name(ret));
            break;
        }
    }

    // 等待最后一帧播放完成
    vTaskDelay(pdMS_TO_TICKS(100));

    // 释放资源
    free(audio_buf);
    ESP_LOGI(TAG, "Play completed");

    // 播放完成后释放I2S资源，不影响LVGL
    i2s_channel_disable(tx_chan);
    s_i2s_enabled = false;
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
    ESP_LOGI(TAG, "I2S channel released");
}

static void audio_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(3000)); // 开机3秒后播放
    audio_play_test_tone();
    vTaskDelete(NULL);
}

void audio_start_playback(void) {
    xTaskCreate(audio_task, "audio_task", 4096, NULL, 1, NULL);
}