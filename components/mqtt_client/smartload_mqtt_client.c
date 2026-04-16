#include "smartload_mqtt_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "nvs_manager.h"
#include "storage_manager.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "mqtt_client";

#define MQTT_CFG_FILE "/mqtt_client.json"
#define MQTT_CFG_NS "smartload"

static struct {
    smartload_mqtt_client_config_t cfg;
    esp_mqtt_client_handle_t client;
    smartload_mqtt_message_cb_t msg_cb;
    bool connected;
} s;

static bool mqtt_json_copy(cJSON *root, const char *key, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
        snprintf(out, out_size, "%s", item->valuestring);
        return true;
    }
    return false;
}

static esp_err_t mqtt_load_from_file(smartload_mqtt_client_config_t *out_cfg)
{
    char buf[1024];
    size_t len = 0;
    if (storage_manager_read_text(MQTT_CFG_FILE, buf, sizeof(buf), &len) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(item)) {
        out_cfg->enabled = cJSON_IsTrue(item);
    }
    mqtt_json_copy(root, "broker_uri", out_cfg->broker_uri, sizeof(out_cfg->broker_uri));
    mqtt_json_copy(root, "client_id", out_cfg->client_id, sizeof(out_cfg->client_id));
    mqtt_json_copy(root, "username", out_cfg->username, sizeof(out_cfg->username));
    mqtt_json_copy(root, "password", out_cfg->password, sizeof(out_cfg->password));
    mqtt_json_copy(root, "publish_topic", out_cfg->publish_topic, sizeof(out_cfg->publish_topic));
    mqtt_json_copy(root, "subscribe_topic", out_cfg->subscribe_topic, sizeof(out_cfg->subscribe_topic));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t smartload_mqtt_client_load_config(smartload_mqtt_client_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    int32_t enabled = 0;
    if (nvs_manager_get_i32(MQTT_CFG_NS, "mq_en", &enabled) == ESP_OK) {
        out_cfg->enabled = enabled != 0;
    }
    nvs_manager_get_str(MQTT_CFG_NS, "mq_uri", out_cfg->broker_uri, sizeof(out_cfg->broker_uri));
    nvs_manager_get_str(MQTT_CFG_NS, "mq_cid", out_cfg->client_id, sizeof(out_cfg->client_id));
    nvs_manager_get_str(MQTT_CFG_NS, "mq_usr", out_cfg->username, sizeof(out_cfg->username));
    nvs_manager_get_str(MQTT_CFG_NS, "mq_pwd", out_cfg->password, sizeof(out_cfg->password));
    nvs_manager_get_str(MQTT_CFG_NS, "mq_pub", out_cfg->publish_topic, sizeof(out_cfg->publish_topic));
    nvs_manager_get_str(MQTT_CFG_NS, "mq_sub", out_cfg->subscribe_topic, sizeof(out_cfg->subscribe_topic));

    if (out_cfg->broker_uri[0] != '\0') {
        return ESP_OK;
    }

    return mqtt_load_from_file(out_cfg);
}

esp_err_t smartload_mqtt_client_save_config(const smartload_mqtt_client_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_manager_set_i32(MQTT_CFG_NS, "mq_en", cfg->enabled ? 1 : 0);
    if (err != ESP_OK) return err;
    err = nvs_manager_set_str(MQTT_CFG_NS, "mq_uri", cfg->broker_uri);
    if (err != ESP_OK) return err;
    err = nvs_manager_set_str(MQTT_CFG_NS, "mq_cid", cfg->client_id);
    if (err != ESP_OK) return err;
    err = nvs_manager_set_str(MQTT_CFG_NS, "mq_usr", cfg->username);
    if (err != ESP_OK) return err;
    err = nvs_manager_set_str(MQTT_CFG_NS, "mq_pwd", cfg->password);
    if (err != ESP_OK) return err;
    err = nvs_manager_set_str(MQTT_CFG_NS, "mq_pub", cfg->publish_topic);
    if (err != ESP_OK) return err;
    err = nvs_manager_set_str(MQTT_CFG_NS, "mq_sub", cfg->subscribe_topic);
    if (err != ESP_OK) return err;

    char json[1400];
    snprintf(json, sizeof(json),
             "{\"enabled\":%s,\"broker_uri\":\"%s\",\"client_id\":\"%s\",\"username\":\"%s\","
             "\"password\":\"%s\",\"publish_topic\":\"%s\",\"subscribe_topic\":\"%s\"}",
             cfg->enabled ? "true" : "false",
             cfg->broker_uri,
             cfg->client_id,
             cfg->username,
             cfg->password,
             cfg->publish_topic,
             cfg->subscribe_topic);
    return storage_manager_write_text(MQTT_CFG_FILE, json);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s.connected = true;
        ESP_LOGI(TAG, "Connected broker=%s client_id=%s", s.cfg.broker_uri, s.cfg.client_id);
        if (s.cfg.subscribe_topic[0] != '\0') {
            esp_mqtt_client_subscribe(s.client, s.cfg.subscribe_topic, 1);
            ESP_LOGI(TAG, "Subscribed topic=%s", s.cfg.subscribe_topic);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s.connected = false;
        ESP_LOGW(TAG, "Disconnected broker=%s", s.cfg.broker_uri);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "RX topic=%.*s payload=%.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);
        if (s.msg_cb) {
            s.msg_cb(event->topic, event->topic_len, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error event");
        break;
    default:
        break;
    }
}

esp_err_t smartload_mqtt_client_init(const smartload_mqtt_client_config_t *cfg, smartload_mqtt_message_cb_t cb)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    s.msg_cb = cb;

    if (!s.cfg.enabled || s.cfg.broker_uri[0] == '\0') {
        ESP_LOGI(TAG, "Optional MQTT client disabled");
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s.cfg.broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s.cfg.client_id[0] ? s.cfg.client_id : NULL,
        .credentials.username = s.cfg.username[0] ? s.cfg.username : NULL,
        .credentials.authentication.password = s.cfg.password[0] ? s.cfg.password : NULL,
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
    ESP_LOGI(TAG, "MQTT client ready broker=%s pub=%s sub=%s",
             s.cfg.broker_uri,
             s.cfg.publish_topic,
             s.cfg.subscribe_topic);
    return ESP_OK;
}

esp_err_t smartload_mqtt_client_start(void)
{
    if (!s.cfg.enabled || !s.client) {
        return ESP_OK;
    }
    return esp_mqtt_client_start(s.client);
}

esp_err_t smartload_mqtt_client_stop(void)
{
    s.connected = false;
    if (!s.client) {
        return ESP_OK;
    }
    return esp_mqtt_client_stop(s.client);
}

bool smartload_mqtt_client_is_connected(void)
{
    return s.connected;
}

esp_err_t smartload_mqtt_client_publish_json(const char *payload_json)
{
    if (!payload_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.client || !s.connected || s.cfg.publish_topic[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "TX topic=%s payload=%s", s.cfg.publish_topic, payload_json);
    int mid = esp_mqtt_client_publish(s.client, s.cfg.publish_topic, payload_json, 0, 1, 0);
    return mid >= 0 ? ESP_OK : ESP_FAIL;
}
