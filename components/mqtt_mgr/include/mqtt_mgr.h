#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    MQTT_MGR_EVENT_CONNECTED = 0,
    MQTT_MGR_EVENT_DISCONNECTED,
    MQTT_MGR_EVENT_DATA,
    MQTT_MGR_EVENT_ERROR,
} mqtt_mgr_event_type_t;

typedef struct {
    mqtt_mgr_event_type_t type;
    const char *topic;
    int topic_len;
    const char *data;
    int data_len;
} mqtt_mgr_event_t;

typedef void (*mqtt_mgr_event_cb_t)(const mqtt_mgr_event_t *event, void *user_ctx);

typedef struct {
    char broker_uri[192];
    char client_id[128];
    char username[320];
    char password[256];
} mqtt_mgr_config_t;

esp_err_t mqtt_mgr_init(const mqtt_mgr_config_t *cfg, mqtt_mgr_event_cb_t callback, void *user_ctx);
esp_err_t mqtt_mgr_connect(void);
esp_err_t mqtt_mgr_disconnect(void);
bool mqtt_mgr_is_connected(void);
int mqtt_mgr_publish(const char *topic, const char *payload, int qos, int retain);
esp_err_t mqtt_mgr_subscribe(const char *topic, int qos);
