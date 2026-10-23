#include "esp_openclaw_room_node.h"
#include "tailscale_manager.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define TAG "waveshare_room"
#define WAVESHARE_DRAW_ROWS 16

static void (*toggle_room_view)(void);

static void boot_button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    for (;;) {
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !was_pressed && toggle_room_view != NULL) {
            toggle_room_view();
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static esp_err_t waveshare_setup_local_input(void *ctx, void (*toggle_view)(void))
{
    (void)ctx;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "BOOT button GPIO");
    toggle_room_view = toggle_view;
    return xTaskCreateWithCaps(
        boot_button_task,
        "boot_btn",
        2560,
        NULL,
        4,
        NULL,
        MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void align_sh8601_flush(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    if (area == NULL) {
        return;
    }
    area->x1 = 0;
    area->x2 = BSP_LCD_H_RES - 1;
    area->y1 &= ~1;
    area->y2 |= 1;
}

static lv_display_t *waveshare_display_start(void *ctx)
{
    (void)ctx;
    /* FIELD 2026-10-23 (two failed rungs taught the ladder): prio 13
     * does not just slow the lane — it STALLS BRING-UP. Evidence: the
     * tailscale lane dropped the moment this rung flashed (board offline,
     * 100% ping loss, every captured boot frozen in audio init without
     * reaching 'room node ready'): taskLVGL outranks the operator/node
     * clients (11) that must run during boot, so init starves. prio 4
     * starved the display instead. 7 is the field-proven compromise:
     * lane boots and lives, display beats housekeeping (5/6/4); the
     * residual post-call churn (ml_wg_mgr 24-27%) is being fixed at its
     * source (DISCO registry hygiene + retry backoff), after which even
     * the default 4 would be safe. The HTTPD (CONFIG_ML_ENABLE_CONFIG_HTTPD)
     * gives /api/peers/allowed as a manual trim in the meantime. */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        }};
    cfg.lvgl_port_cfg.task_priority = 7;
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    if (display == NULL || !bsp_display_lock(0)) {
        return NULL;
    }

    /* SH8601 QSPI cannot DMA from PSRAM. Allocate the two partial buffers once
     * from internal DMA memory, and keep invalidations on even full-width rows. */
    size_t draw_bytes = (size_t)BSP_LCD_H_RES * WAVESHARE_DRAW_ROWS * sizeof(uint16_t);
    void *draw_a = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN, draw_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *draw_b = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN, draw_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (draw_a == NULL || draw_b == NULL) {
        heap_caps_free(draw_a);
        heap_caps_free(draw_b);
        bsp_display_unlock();
        ESP_LOGE(TAG, "both internal DMA draw buffers are required for SH8601");
        return NULL;
    }
    lv_display_set_buffers(
        display, draw_a, draw_b, (uint32_t)draw_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display, align_sh8601_flush, LV_EVENT_INVALIDATE_AREA, NULL);
    /* FIELD 2026-10-23: the touch-poll DIAGNOSTIC (lv_indev_set_mode TIMER)
     * was REVERTED after one run: it proved the FT5x06 fails every I2C read
     * ('FT5x06: I2C read error!' x22 at t=1730ms), but esp_lvgl_port's
     * lvgl_port_touchpad_read wraps the read in ESP_ERROR_CHECK
     * (esp_lvgl_port_touch.c:127), so in poll mode EVERY failed read
     * abort()ed the board -> bootloop. In EVENT mode the read only runs on
     * INT events, so a never-firing INT line + failing I2C reads are
     * silently hidden (why touch looked 'silent' all along). FINDING:
     * the touch controller (or its bus power/wiring/pull-ups) does not
     * answer on I2C. Next step is hardware-side: check touch FPC/connector
     * + 3.3V on the touch rail + I2C pull-ups on the FT5x06 bus. */
    bsp_display_unlock();
    return display;
}

static bool waveshare_display_lock(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    return bsp_display_lock(timeout_ms);
}

static void waveshare_display_unlock(void *ctx)
{
    (void)ctx;
    bsp_display_unlock();
}

static esp_err_t waveshare_set_brightness(void *ctx, int percent)
{
    (void)ctx;
    return bsp_display_brightness_set(percent);
}

static esp_err_t waveshare_audio_open(
    void *ctx,
    esp_openclaw_room_audio_handles_t *handles)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(handles != NULL, ESP_ERR_INVALID_ARG, TAG, "audio handles required");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "audio I2C bus init");
    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(i2c != NULL, ESP_ERR_INVALID_STATE, TAG, "audio I2C bus handle");

    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t channel_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &tx, &rx), TAG, "I2S channel create");

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &i2s_cfg), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &i2s_cfg), TAG, "I2S RX init");
    i2s_chan_info_t tx_info = {0};
    i2s_chan_info_t rx_info = {0};
    ESP_RETURN_ON_ERROR(i2s_channel_get_info(tx, &tx_info), TAG, "I2S TX channel info");
    ESP_RETURN_ON_ERROR(i2s_channel_get_info(rx, &rx_info), TAG, "I2S RX channel info");
    ESP_RETURN_ON_FALSE(
        tx_info.pair_chan == rx && rx_info.pair_chan == tx,
        ESP_ERR_INVALID_STATE,
        TAG,
        "I2S TX/RX failed to establish a reciprocal pair");
    ESP_LOGI(TAG, "I2S paired at 24 kHz with a symmetric stereo STD frame contract");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, TAG, "I2S codec data interface");

    audio_codec_i2c_cfg_t speaker_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *speaker_ctrl = audio_codec_new_i2c_ctrl(&speaker_ctrl_cfg);
    ESP_RETURN_ON_FALSE(
        speaker_ctrl != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 control interface");
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 GPIO interface");
    esp_codec_dev_hw_gain_t gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3};
    es8311_codec_cfg_t speaker_cfg = {
        .ctrl_if = speaker_ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .use_mclk = true,
        .hw_gain = gain,
    };
    const audio_codec_if_t *speaker = es8311_codec_new(&speaker_cfg);
    ESP_RETURN_ON_FALSE(speaker != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 codec interface");
    esp_codec_dev_cfg_t speaker_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker,
        .data_if = data,
    };
    handles->playback = esp_codec_dev_new(&speaker_dev);
    ESP_RETURN_ON_FALSE(
        handles->playback != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 playback device");

    audio_codec_i2c_cfg_t microphone_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *microphone_ctrl =
        audio_codec_new_i2c_ctrl(&microphone_ctrl_cfg);
    ESP_RETURN_ON_FALSE(
        microphone_ctrl != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 control interface");
    es7210_codec_cfg_t microphone_cfg = {
        .ctrl_if = microphone_ctrl,
        /* Tested board geometry: near-end MIC1 plus speaker reference on MIC3. */
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3,
    };
    const audio_codec_if_t *microphone = es7210_codec_new(&microphone_cfg);
    ESP_RETURN_ON_FALSE(
        microphone != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 codec interface");
    esp_codec_dev_cfg_t microphone_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone,
        .data_if = data,
    };
    handles->record = esp_codec_dev_new(&microphone_dev);
    ESP_RETURN_ON_FALSE(
        handles->record != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 capture device");
    return ESP_OK;
}

/* FIELD 2026-10-23 (touch A/B verdict): Waveshare's factory firmware drives
 * the SAME FT3168 with the SAME esp_lcd_touch_ft5x06 driver, same pins, same
 * 400kHz - and touch works there, while every build since the 2026-10-22
 * 19:58 sdkconfig regeneration reads nothing (the indev poll diagnostic
 * showed FT5x06 I2C read error x22). Hardware is PROVEN good. This probe
 * instruments our side of that regression: a second device handle on the
 * same bus polling the FT5x06 registers directly every 5s, plus the
 * INT(38)/RST(9) GPIO levels + the BSP io handle's own state. It separates:
 *   - chip ABSENT from bus (NACK at 0x38)   -> power/reset-state issue
 *   - chip PRESENT but regs read garbage    -> config/driver-state issue
 * Runs from an esp_timer (reads only), WARNING line per probe so it is
 * visible under the serial-discipline log gates. */
#define TOUCH_PROBE_I2C_ADDR 0x38   /* FT5x06/FT3168 default */
static esp_lcd_touch_handle_t s_tp_probe_tp;   /* factory-path touch handle on the BSP bus */
static esp_timer_handle_t s_tp_probe_timer;
static esp_lcd_touch_handle_t s_tp_probe_tp;   /* factory-path touch handle on the BSP bus */

static void touch_probe_tick_cb(void *arg)
{
    (void)arg;
    int int_lvl = gpio_get_level((gpio_num_t)BSP_LCD_TOUCH_INT);
    int rst_lvl = gpio_get_level((gpio_num_t)BSP_LCD_TOUCH_RST);
    if (s_tp_probe_tp == NULL) {
        /* One-shot every 30s until it succeeds: the factory path itself —
         * bsp_touch_new creates a second touch device on the BSP's OWN I2C
         * bus (same pins, same driver, same 0x38 init sequence as the
         * factory firmware). If this fails, the chip NACKs on our bus; if
         * the reset line is held low, that is the reason. */
        esp_err_t err = bsp_touch_new(NULL, &s_tp_probe_tp);
        ESP_LOGW(TAG, "[tp_probe] bsp_touch_new -> %s (int=%d rst=%d)",
                 esp_err_to_name(err), int_lvl, rst_lvl);
        if (err != ESP_OK) { s_tp_probe_tp = NULL; return; }
    }
    esp_err_t err = esp_lcd_touch_read_data(s_tp_probe_tp);
    ESP_LOGW(TAG, "[tp_probe] read -> %s (int=%d rst=%d)",
             esp_err_to_name(err), int_lvl, rst_lvl);
}

static void touch_probe_start(void)
{
    if (s_tp_probe_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = touch_probe_tick_cb,
            .name = "tp_probe",
        };
        if (esp_timer_create(&args, &s_tp_probe_timer) != ESP_OK) return;
    }
    esp_timer_start_periodic(s_tp_probe_timer, 5000000);
    ESP_LOGW(TAG, "[tp_probe] armed: factory-path bsp_touch_new + read on the BSP bus, 5s period");
}

void app_main(void)
{
    /* FIELD 2026-10-21 (Fredrik): install the cached gateway name -> IP
     * answer BEFORE the room node primes its node/operator clients — those
     * start connecting within ~100ms of esp_openclaw_room_node_start(),
     * while the tailnet lane (and the peer table it feeds) only comes up
     * seconds later. Without this the first connects die on
     * CANNOT_RESOLVE_HOSTNAME. */
    esp_err_t gw_prep = tailscale_manager_prepare_gateway();
    ESP_LOGI(TAG, "gateway resolver prepared: %s", esp_err_to_name(gw_prep));

    const esp_openclaw_room_node_config_t config = {
        .display_name = "Waveshare AMOLED Room Node",
        .model_identifier = "waveshare-esp32-s3-touch-amoled-2.06",
        .display = {
            .start = waveshare_display_start,
            .setup_local_input = waveshare_setup_local_input,
            .lock = waveshare_display_lock,
            .unlock = waveshare_display_unlock,
            .set_brightness = waveshare_set_brightness,
            .native_width = BSP_LCD_H_RES,
            .native_height = BSP_LCD_V_RES,
            .safe_inset = 32,
            .animated_face = true,
            .animation_frame_ms = 16,
            /* FIELD 2026-10-21: the board left idle_brightness unset, so
             * room_ui_target_brightness() computed 0 for ROOM_UI_IDLE and the
             * panel was driven to brightness 0 — the screen went BLACK when a
             * call ended (the face is hidden in IDLE anyway). Not a panel
             * fault, and not the cause of the call failure: a state-driven
             * brightness. 18%% matches the other text states. */
            .idle_brightness = 18,
        },
        .audio = {
            .open = waveshare_audio_open,
            .afe_layout = "MR",
            .record_channels = 2,
            .channel_mask = 0x3,
            .playback_volume = 100,
            .playback_gain_db = 6.0f,
            .configure_input_gain = true,
            .input_gain_db = 30.0f,
            /* FIELD 2026-10-21 (Fredrik): the ES7210 is a 2-mic ADC — the 'R'
             * channel is MIC3, NOT a loopback of the speaker. So there is no
             * real AEC reference: the speaker's own voice reached the
             * uplink, the gateway interrupted itself, and the voice session
             * was torn down mid-reply (dual ws abort ~20s after every call
             * opened — errno 113 'Software caused connection abort' = the
             * peer's RST, not a network fault). Force half-duplex: the mic
             * is muted while the speaker plays (+300 ms tail). */
            .force_half_duplex_capture = true,
        },
    };
    ESP_ERROR_CHECK(esp_openclaw_room_node_start(&config));

    /* FIELD 2026-10-23 (touch A/B): instrument OUR side of the touch
     * regression (factory firmware = touch works; ours since the 19:58
     * regen = NACK). Arms AFTER the room node start so the BSP's own bus
     * claim and the display pipeline are settled first. */
    touch_probe_start();

    /* Tailnet lane: connectivity layer only — the gateway URI and its
     * credentials stay provisioning items (NVS, USB setup flow). The
     * manager is flag-gated (CONFIG_TS_ENABLE) and no-ops when disabled;
     * it waits internally for the room node's station to hold an address. */
    ESP_ERROR_CHECK(tailscale_manager_start());
}

/* FIELD 2026-10-21 (Fredrik): the room node calls this when a gateway
 * connect fails — drop the cached gateway name -> IP answer so the next
 * resolve re-reads the peer table. */
void esp_openclaw_room_note_gateway_unreachable(void)
{
    tailscale_manager_note_gateway_unreachable();
}
