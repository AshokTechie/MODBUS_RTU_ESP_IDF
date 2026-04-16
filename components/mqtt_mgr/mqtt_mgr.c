#include "mqtt_mgr.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "mqtt_mgr";

#define MQTT_MGR_CONNECTED_BIT BIT0
#define MQTT_MGR_FAILED_BIT    BIT1

static struct {
    mqtt_mgr_config_t cfg;
    esp_mqtt_client_handle_t client;
    mqtt_mgr_event_cb_t callback;
    void *user_ctx;
    bool connected;
    EventGroupHandle_t events;
} s;

static void dispatch_event(mqtt_mgr_event_type_t type, const char *topic, int topic_len, const char *data, int data_len)
{
    if (!s.callback) {
        return;
    }
    mqtt_mgr_event_t evt = {
        .type = type,
        .topic = topic,
        .topic_len = topic_len,
        .data = data,
        .data_len = data_len,
    };
    s.callback(&evt, s.user_ctx);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s.connected = true;
        if (s.events) {
            xEventGroupClearBits(s.events, MQTT_MGR_FAILED_BIT);
            xEventGroupSetBits(s.events, MQTT_MGR_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "Connected broker=%s client_id=%s", s.cfg.broker_uri, s.cfg.client_id);
        dispatch_event(MQTT_MGR_EVENT_CONNECTED, NULL, 0, NULL, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s.connected = false;
        if (s.events) {
            xEventGroupClearBits(s.events, MQTT_MGR_CONNECTED_BIT);
            xEventGroupSetBits(s.events, MQTT_MGR_FAILED_BIT);
        }
        ESP_LOGW(TAG, "Disconnected broker=%s", s.cfg.broker_uri);
        dispatch_event(MQTT_MGR_EVENT_DISCONNECTED, NULL, 0, NULL, 0);
        break;
    case MQTT_EVENT_DATA:
        dispatch_event(MQTT_MGR_EVENT_DATA, event->topic, event->topic_len, event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        s.connected = false;
        if (s.events) {
            xEventGroupClearBits(s.events, MQTT_MGR_CONNECTED_BIT);
            xEventGroupSetBits(s.events, MQTT_MGR_FAILED_BIT);
        }
        if (event && event->error_handle) {
            ESP_LOGE(TAG,
                     "MQTT error broker=%s type=%d sock_errno=%d esp_tls=0x%x tls_stack=0x%x",
                     s.cfg.broker_uri,
                     event->error_handle->error_type,
                     event->error_handle->esp_transport_sock_errno,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err);
        } else {
            ESP_LOGE(TAG, "MQTT error broker=%s", s.cfg.broker_uri);
        }
        dispatch_event(MQTT_MGR_EVENT_ERROR, NULL, 0, NULL, 0);
        break;
    default:
        break;
    }
}

esp_err_t mqtt_mgr_init(const mqtt_mgr_config_t *cfg, mqtt_mgr_event_cb_t callback, void *user_ctx)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s.events) {
        s.events = xEventGroupCreate();
        if (!s.events) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s.client) {
        mqtt_mgr_destroy();
    }

    memset(&s.cfg, 0, sizeof(s.cfg));
    s.cfg = *cfg;
    s.callback = callback;
    s.user_ctx = user_ctx;
    s.connected = false;
    xEventGroupClearBits(s.events, MQTT_MGR_CONNECTED_BIT | MQTT_MGR_FAILED_BIT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s.cfg.broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s.cfg.client_id,
        .credentials.username = s.cfg.username,
        .credentials.authentication.password = s.cfg.password,
        .session.keepalive = 60,
        .buffer.size = 4096,
        .buffer.out_size = 2048,
        .network.timeout_ms = 10000,
        .task.stack_size = 8192,
    };

    s.client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s.client) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "MQTT initialized for %s", s.cfg.broker_uri);
    return ESP_OK;
}

esp_err_t mqtt_mgr_connect(void)
{
    if (!s.client) {
        return ESP_ERR_INVALID_STATE;
    }
    s.connected = false;
    if (s.events) {
        xEventGroupClearBits(s.events, MQTT_MGR_CONNECTED_BIT | MQTT_MGR_FAILED_BIT);
    }
    return esp_mqtt_client_start(s.client);
}

esp_err_t mqtt_mgr_disconnect(void)
{
    if (!s.client) {
        return ESP_ERR_INVALID_STATE;
    }
    s.connected = false;
    if (s.events) {
        xEventGroupClearBits(s.events, MQTT_MGR_CONNECTED_BIT);
    }
    return esp_mqtt_client_stop(s.client);
}

esp_err_t mqtt_mgr_wait_until_connected(uint32_t timeout_ms)
{
    if (!s.events || !s.client) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s.events,
        MQTT_MGR_CONNECTED_BIT | MQTT_MGR_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & MQTT_MGR_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & MQTT_MGR_FAILED_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t mqtt_mgr_destroy(void)
{
    esp_err_t err = ESP_OK;
    if (s.client) {
        s.connected = false;
        esp_mqtt_client_stop(s.client);
        err = esp_mqtt_client_destroy(s.client);
        s.client = NULL;
    }
    if (s.events) {
        xEventGroupClearBits(s.events, MQTT_MGR_CONNECTED_BIT | MQTT_MGR_FAILED_BIT);
    }
    return err;
}

bool mqtt_mgr_is_connected(void)
{
    return s.connected;
}

int mqtt_mgr_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!s.client) {
        return -1;
    }
    return esp_mqtt_client_publish(s.client, topic, payload, 0, qos, retain);
}

esp_err_t mqtt_mgr_subscribe(const char *topic, int qos)
{
    if (!s.client) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_subscribe(s.client, topic, qos) >= 0 ? ESP_OK : ESP_FAIL;
}
