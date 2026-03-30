/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <math.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef PI
#define PI  (3.14159f)
#endif

// ==================== 核心配置：480x320 屏幕 ====================
#define EXAMPLE_LCD_H_RES   480
#define EXAMPLE_LCD_V_RES   320

// ==================== 触摸配置（FT6336U I2C）====================
#define TOUCH_I2C_PORT       I2C_NUM_0
#define TOUCH_SDA_GPIO        6
#define TOUCH_SCL_GPIO        5
#define FT6336U_ADDR          0x38

// ==================== PWM 背光配置 ====================
#define BACKLIGHT_GPIO      45      
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define BACKLIGHT_FREQ      5000

#if CONFIG_EXAMPLE_LCD_IMAGE_FROM_EMBEDDED_BINARY
LV_IMG_DECLARE(esp_logo);
LV_IMG_DECLARE(esp_text);
#endif


typedef struct {
    lv_obj_t *scr;
    int count_val;
} my_timer_context_t;

static my_timer_context_t my_tim_ctx;
static lv_obj_t *arc[3];
static lv_obj_t *img_logo = NULL;
static lv_obj_t *img_text = NULL;

static const lv_color_t arc_color[] = {
    LV_COLOR_MAKE(232, 87, 116),
    LV_COLOR_MAKE(126, 87, 162),
    LV_COLOR_MAKE(90, 202, 228),
};

// ======================================================================================
// ===================== 右边滑动条：控制图片透明度=====================
// ======================================================================================
static lv_obj_t *vertical_slider = NULL;
// 拉高 GPIO10 点灯
void gpio10_high_init(void)
{
    // 配置 GPIO10 为推挽输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 10),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // 输出高电平（点灯）
    gpio_set_level(GPIO_NUM_10, 1);
}

static void slider_value_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);

    if(img_logo != NULL)
    {
        lv_obj_set_style_opa(img_logo, value, LV_PART_MAIN);
    }
}


static void create_vertical_slider(lv_obj_t *scr)
{
    vertical_slider = lv_slider_create(scr);
    lv_obj_set_size(vertical_slider, 25, 180);
    lv_obj_align(vertical_slider, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_slider_set_range(vertical_slider, 10, 255);
    lv_slider_set_value(vertical_slider, 255, LV_ANIM_ON);

    lv_obj_set_style_line_width(vertical_slider, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vertical_slider, lv_color_hex(0x5ACAE4), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vertical_slider, lv_color_hex(0x7E57A2), LV_PART_KNOB);
    lv_obj_set_style_radius(vertical_slider, 15, LV_PART_KNOB);

    lv_obj_add_event_cb(vertical_slider, slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// ======================================================================================
// ===================== 左边滑动条：控制 PWM 背光 =====================
// ======================================================================================
static lv_obj_t *slider_pwm_left;

static void slider_pwm_left_cb(lv_event_t *e)
{
    int32_t val = lv_slider_get_value(lv_event_get_target(e));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, val);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

static void create_slider_pwm_left(lv_obj_t *scr)
{
    slider_pwm_left = lv_slider_create(scr);
    lv_obj_set_size(slider_pwm_left, 25, 180);
    lv_obj_align(slider_pwm_left, LV_ALIGN_LEFT_MID, 20, 0); 

    lv_slider_set_range(slider_pwm_left, 10, 255);
    lv_slider_set_value(slider_pwm_left, 255, LV_ANIM_OFF);

    lv_obj_set_style_line_width(slider_pwm_left, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_pwm_left, lv_color_hex(0xFFAA00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_pwm_left, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(slider_pwm_left, 15, LV_PART_KNOB);

    lv_obj_add_event_cb(slider_pwm_left, slider_pwm_left_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// ======================================================================================
// ==================== PWM 背光初始化 ====================
// ======================================================================================
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = BACKLIGHT_FREQ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num       = BACKLIGHT_GPIO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .duty           = 255,
        .hpoint         = 0
    };
    ledc_channel_config(&channel);
}

// ======================================================================================
// ==================== 触摸部分====================
// ======================================================================================
static bool ft6336u_read(uint16_t *x, uint16_t *y)
{
    uint8_t data[5];
    esp_err_t ret = i2c_master_write_read_device(
        TOUCH_I2C_PORT,
        FT6336U_ADDR,
        (uint8_t[]){0x02}, 1,
        data, 5,
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK || (data[0] & 0x0F) == 0) {
        return false;
    }

    *x = ((data[1] & 0x0F) << 8) | data[2];
    *y = ((data[3] & 0x0F) << 8) | data[4];

    int32_t tx = *x;
    int32_t ty = *y;

    *x = EXAMPLE_LCD_H_RES - ty;
    *y = tx;

    return true;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (ft6336u_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void touch_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    i2c_param_config(TOUCH_I2C_PORT, &i2c_conf);
    i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
}

// ======================================================================================
// ==================== 动画部分 ====================
// ======================================================================================
static void anim_timer_cb(lv_timer_t *timer)
{
    my_timer_context_t *ctx = (my_timer_context_t *)lv_timer_get_user_data(timer);
    int c = ctx->count_val;
    lv_obj_t *scr = ctx->scr;

    if (c < 90) {
        lv_coord_t sa = c > 0 ? (1 - cosf(c/180.0f*PI)) * 270 : 0;
        lv_coord_t sl = (sinf(c/180.0f*PI) + 1) * 135;
        for (int i=0;i<3;i++) {
            lv_arc_set_bg_angles(arc[i], sa, sl);
            lv_arc_set_rotation(arc[i], (c + 120*(i+1)) % 360);
        }
    }

    if (c == 90) {
        for (int i=0;i<3;i++) lv_obj_delete(arc[i]);
        img_text = lv_img_create(scr);
#if CONFIG_EXAMPLE_LCD_IMAGE_FROM_FILE_SYSTEM
        lv_img_set_src(img_text, "S:/spiffs/esp_text.png");
#else
        lv_img_set_src(img_text, &esp_text);
#endif
        lv_obj_set_style_opa(img_text, 0, LV_PART_MAIN);
    }

    if (c >= 100 && c <= 180) {
        lv_coord_t ofs = (sinf((c-140)*2.25f/90.0f)+1)*20.0f;
        lv_obj_align(img_logo, LV_ALIGN_CENTER, 0, -ofs);
        lv_obj_align(img_text, LV_ALIGN_CENTER, 0, 2*ofs);
        lv_obj_set_style_opa(img_text, ofs/40.0f*255, LV_PART_MAIN);
    }

    if ((c +=5) == 220) lv_timer_delete(timer);
    else ctx->count_val = c;
}

static void start_animation(lv_obj_t *scr)
{
    lv_obj_center(img_logo);
    for (int i=0;i<3;i++) {
        arc[i] = lv_arc_create(scr);
        lv_obj_set_size(arc[i], 220-30*i, 220-30*i);
        lv_arc_set_bg_angles(arc[i], 120*i, 10+120*i);
        lv_arc_set_value(arc[i], 0);
        lv_obj_remove_style(arc[i], NULL, LV_PART_KNOB);
        lv_obj_set_style_line_width(arc[i], 10, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(arc[i], arc_color[i], LV_PART_INDICATOR);
        lv_obj_center(arc[i]);
    }
    if (img_text) { lv_obj_delete(img_text); img_text=NULL; }
    my_tim_ctx.count_val = -90;
    my_tim_ctx.scr = scr;
    lv_timer_create(anim_timer_cb, 20, &my_tim_ctx);
}

// ======================================================================================
// ==================== 主入口（只加了两行） ====================
// ======================================================================================
void example_lvgl_demo_ui(lv_disp_t *disp)

{
    
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    img_logo = lv_img_create(scr);
#if CONFIG_EXAMPLE_LCD_IMAGE_FROM_FILE_SYSTEM
    lv_img_set_src(img_logo, "S:/spiffs/esp_logo.png");
#else
    lv_img_set_src(img_logo, &esp_logo);
#endif
    lv_obj_align(img_logo, LV_ALIGN_CENTER, 0, 0); // 居中显示

    // // ===================== 加一个固定文字 =====================
    // lv_obj_t *label = lv_label_create(scr);
    // lv_label_set_text(label, "SCREEN IS WORKING");
    // lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -20);
    backlight_init();    
    gpio10_high_init();  //点灯
    touch_init();
    create_vertical_slider(scr);   
    create_slider_pwm_left(scr);   
    start_animation(scr);
}