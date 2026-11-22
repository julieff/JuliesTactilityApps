#pragma once

// The subset of ESP-IDF's HTTP client interface used by TransitApi.cpp.
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;

enum esp_http_client_event_id_t { HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA };

struct esp_http_client_event_t {
    esp_http_client_event_id_t event_id;
    void* user_data;
    void* data;
    int data_len;
};

struct esp_http_client_config_t {
    const char* url;
    int timeout_ms;
    esp_err_t (*event_handler)(esp_http_client_event_t*);
    void* user_data;
    esp_err_t (*crt_bundle_attach)(void*);
};

struct esp_http_client;
using esp_http_client_handle_t = esp_http_client*;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
