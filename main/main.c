
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "driver/gpio.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

#define CAN_TX_GPIO GPIO_NUM_21
#define CAN_RX_GPIO GPIO_NUM_22

#define ENABLE_CAN_TRANSMIT 0
#define RX_QUEUE_DEPTH 64

static const char *TAG = "PSA207_CAN";
static twai_node_handle_t s_twai_node = NULL;
static QueueHandle_t s_rx_queue = NULL;

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    uint64_t timestamp;
    bool is_ext;
    bool is_rtr;
} app_can_frame_t;

typedef struct {
    uint32_t id;
    const char *name;
    const char *meaning;
    bool log_every_frame;
} watched_id_t;

static const watched_id_t WATCHED_IDS[] = {
    {0x21F, "RADIO_STALK_DOC",       "steering-column radio remote candidate; dump: DLC3 04 10 60 static", false},
    {0x3E5, "RD4_BUTTONS_DOC",       "RD4/front-panel/menu/navigation possibility; dump: DLC6 static", false},
    {0x165, "OBS_CHANGED_165",       "Observed changing near interaction window; candidate vehicle/input state", false},
    {0x1A5, "OBS_CHANGED_1A5",       "Noted E0->E9 near interaction window; candidate interaction/state", false},
    {0x1E0, "OBS_CHANGED_1E0",       "Noted 3 payloads;local interaction", false},
    {0x1E5, "OBS_CHANGED_1E5",       "downstream change after 0x1E0", false},
    {0x3F6, "OBS_DYNAMIC_3F6",       "rolling/dynamic payload; likely counter/status/text/state", false},
    {0x220, "DISPLAY_RADIO_220",     "static: DLC2 41 41", false},
    {0x221, "DISPLAY_RADIO_221",     "static: DLC7", false},
    {0x225, "RADIO_STATUS_225",      "Radio/status ID", false},
    {0x227, "RADIO_DISPLAY_227",     "Radio/display ID", false},
    {0x2A0, "BSI_STATUS_2A0",        "BSI/status ID", false},
    {0x2A1, "BSI_TRIP_2A1",          "BSI/trip/status ID", false},
    {0x2A5, "RADIO_TEXT_2A5",        "Radio/display text/status ID", false},
    {0x261, "BSI_TRIP_261",          "BSI/trip/time-adjacent ID", false},
    {0x276, "CLOCK_BSI_TO_DISPLAY",  "Likely BSI date/time broadcast candidate", true},
    {0x39B, "CLOCK_DISPLAY_TO_BSI",  "Likely display/clock-setting control related", true},
};

#define WATCHED_COUNT (sizeof(WATCHED_IDS) / sizeof(WATCHED_IDS[0]))

typedef struct {
    bool seen;
    uint8_t dlc;
    uint8_t data[8];
    int64_t last_us;
    uint32_t count;
} id_state_t;

static id_state_t states[WATCHED_COUNT];

static const watched_id_t *find_watched(uint32_t id, size_t *idx_out)
{
    for (size_t i = 0; i < WATCHED_COUNT; i++) {
        if (WATCHED_IDS[i].id == id) {
            if (idx_out) *idx_out = i;
            return &WATCHED_IDS[i];
        }
    }
    return NULL;
}

static void format_data(char *out, size_t out_len, const uint8_t *data, uint8_t dlc)
{
    size_t pos = 0;
    for (uint8_t i = 0; i < dlc && i < 8; i++) {
        int n = snprintf(out + pos, out_len - pos, "%02X%s", data[i], (i + 1 < dlc) ? " " : "");
        if (n < 0 || (size_t)n >= out_len - pos) break;
        pos += (size_t)n;
    }
}

static bool payload_changed(const id_state_t *s, const app_can_frame_t *frame)
{
    if (!s->seen) return true;
    if (s->dlc != frame->dlc) return true;
    return memcmp(s->data, frame->data, frame->dlc) != 0;
}

static void log_frame(const watched_id_t *w, id_state_t *s, const app_can_frame_t *frame, bool changed)
{
    char data_str[3 * 8] = {0};
    format_data(data_str, sizeof(data_str), frame->data, frame->dlc);

    int64_t now = esp_timer_get_time();
    int64_t delta_ms = s->seen ? (now - s->last_us) / 1000 : -1;

    if (changed) {
        ESP_LOGI(TAG,
                 "CHANGE t=%" PRId64 "ms id=0x%03" PRIX32 " %-20s dlc=%u data=%s prev_delta=%" PRId64 "ms count=%" PRIu32 " :: %s",
                 (int64_t)(now / 1000), frame->id, w->name, frame->dlc, data_str,
                 delta_ms, s->count + 1, w->meaning);
    } else if (w->log_every_frame) {
        ESP_LOGI(TAG,
                 "SEEN   t=%" PRId64 "ms id=0x%03" PRIX32 " %-20s dlc=%u data=%s delta=%" PRId64 "ms count=%" PRIu32 " :: %s",
                 (int64_t)(now / 1000), frame->id, w->name, frame->dlc, data_str,
                 delta_ms, s->count + 1, w->meaning);
    }

    s->seen = true;
    s->dlc = frame->dlc;
    memcpy(s->data, frame->data, frame->dlc);
    s->last_us = now;
    s->count++;
}

static bool twai_rx_done_cb(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    uint8_t rx_buf[8] = {0};
    twai_frame_t rx_frame = {
        .buffer = rx_buf,
        .buffer_len = sizeof(rx_buf),
    };

    bool need_yield = false;

    while (twai_node_receive_from_isr(handle, &rx_frame) == ESP_OK) {
        app_can_frame_t app_frame = {
            .id = rx_frame.header.id,
            .dlc = (uint8_t)rx_frame.buffer_len,
            .timestamp = rx_frame.header.timestamp,
            .is_ext = rx_frame.header.ide,
            .is_rtr = rx_frame.header.rtr,
        };

        if (app_frame.dlc > 8) app_frame.dlc = 8;
        memcpy(app_frame.data, rx_buf, app_frame.dlc);

        BaseType_t hp_task_woken = pdFALSE;
        if (s_rx_queue != NULL) {
            (void)xQueueSendFromISR(s_rx_queue, &app_frame, &hp_task_woken);
            if (hp_task_woken == pdTRUE) need_yield = true;
        }

        memset(rx_buf, 0, sizeof(rx_buf));
        rx_frame.buffer = rx_buf;
        rx_frame.buffer_len = sizeof(rx_buf);
    }

    return need_yield;
}

static void can_log_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "CAN log task started. Waiting for watched IDs.");

    while (true) {
        app_can_frame_t frame = {0};
        if (xQueueReceive(s_rx_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        if (frame.is_ext || frame.is_rtr) continue;

        size_t idx = 0;
        const watched_id_t *w = find_watched(frame.id, &idx);
        if (!w) continue;

        id_state_t *s = &states[idx];
        bool changed = payload_changed(s, &frame);
        log_frame(w, s, &frame, changed);
    }
}

static void install_twai(void)
{
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(app_can_frame_t));
    ESP_ERROR_CHECK(s_rx_queue ? ESP_OK : ESP_ERR_NO_MEM);

    twai_onchip_node_config_t node_config = {
        .io_cfg.tx = CAN_TX_GPIO,
        .io_cfg.rx = CAN_RX_GPIO,
        .bit_timing.bitrate = 125000,
        .tx_queue_depth = 5,
        .timestamp_resolution_hz = 1000000,
        .flags.enable_listen_only =
#if ENABLE_CAN_TRANSMIT
            false,
#else
            true,
#endif
    };

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &s_twai_node));

    twai_event_callbacks_t cbs = {
        .on_rx_done = twai_rx_done_cb,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_twai_node, &cbs, NULL));

    twai_mask_filter_config_t accept_all = {
        .id = 0,
        .mask = 0,
        .is_ext = false,
    };
    ESP_ERROR_CHECK(twai_node_config_mask_filter(s_twai_node, 0, &accept_all));
    ESP_ERROR_CHECK(twai_node_enable(s_twai_node));

#if ENABLE_CAN_TRANSMIT
    ESP_LOGW(TAG, "TWAI node started in NORMAL mode: transmission ENABLED.");
#else
    ESP_LOGI(TAG, "TWAI node started in LISTEN_ONLY mode: transmission disabled.");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "PSA207 CAN monitor booting.");
    ESP_LOGI(TAG, "ESP-IDF 6 TWAI node API: esp_twai.h + esp_twai_onchip.h");
    ESP_LOGI(TAG, "TX GPIO=%d RX GPIO=%d bitrate=125000 mode=%s",
             CAN_TX_GPIO, CAN_RX_GPIO,
#if ENABLE_CAN_TRANSMIT
             "NORMAL"
#else
             "LISTEN_ONLY"
#endif
    );

    install_twai();
    xTaskCreate(can_log_task, "can_log_task", 8192, NULL, 10, NULL);
}
