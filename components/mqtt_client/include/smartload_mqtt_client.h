#pragma once

#include "esp_err.h"

#include <stdbool.h>

#define SMARTLOAD_MQTT_URI_LEN 192
#define SMARTLOAD_MQTT_CLIENT_ID_LEN 128
#define SMARTLOAD_MQTT_USERNAME_LEN 320
#define SMARTLOAD_MQTT_PASSWORD_LEN 256
#define SMARTLOAD_MQTT_TOPIC_LEN 192

typedef struct {
    bool enabled;
    char broker_uri[SMARTLOAD_MQTT_URI_LEN];
    char client_id[SMARTLOAD_MQTT_CLIENT_ID_LEN];
    char username[SMARTLOAD_MQTT_USERNAME_LEN];
    char password[SMARTLOAD_MQTT_PASSWORD_LEN];
    char publish_topic[SMARTLOAD_MQTT_TOPIC_LEN];
    char subscribe_topic[SMARTLOAD_MQTT_TOPIC_LEN];
} smartload_mqtt_client_config_t;

typedef void (*smartload_mqtt_message_cb_t)(const char *topic, int topic_len, const char *data, int data_len);

esp_err_t smartload_mqtt_client_load_config(smartload_mqtt_client_config_t *out_cfg);
esp_err_t smartload_mqtt_client_save_config(const smartload_mqtt_client_config_t *cfg);
esp_err_t smartload_mqtt_client_init(const smartload_mqtt_client_config_t *cfg, smartload_mqtt_message_cb_t cb);
esp_err_t smartload_mqtt_client_start(void);
esp_err_t smartload_mqtt_client_stop(void);
bool smartload_mqtt_client_is_connected(void);
esp_err_t smartload_mqtt_client_publish_json(const char *payload_json);
