#include "TransitApi.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <sdkconfig.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#include <cstdio>

namespace {

constexpr size_t MAX_RESPONSE = 96 * 1024;
constexpr size_t MAX_STOPS = 25;

std::string encode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    return encoded;
}

const char* text(const cJSON* object, const char* key, const char* fallback = "?") {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0]
        ? item->valuestring : fallback;
}

void optional(std::string& output, const cJSON* object, const char* key, const char* prefix) {
    const char* value = text(object, key, "");
    if (*value) {
        output += prefix;
        output += value;
    }
}

struct Response {
    std::string body;
    bool tooLarge = false;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    auto* response = static_cast<Response*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t length = static_cast<size_t>(event->data_len);
        if (length > MAX_RESPONSE - response->body.size()) {
            response->tooLarge = true;
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char*>(event->data), length);
    }
    return ESP_OK;
}

} // namespace

std::string transitUrl(const TransitRequest& request) {
    std::string url = "https://cpskapi.lib.tuke.sk/";
    if (request.kind == RequestKind::Connections) {
        url += "mhd?mode=trainbus&from=" + encode(request.from) + "&to=" + encode(request.to);
        if (!request.date.empty()) url += "&date=" + encode(request.date);
        if (!request.time.empty()) url += "&time=" + encode(request.time);
    } else {
        url += "mhd-stops?mode=trainbus&q=" +
            encode(request.kind == RequestKind::FromStops ? request.from : request.to);
    }
    return url;
}

TransitResult transitParse(const TransitRequest& request, const char* json) {
    TransitResult result;
    cJSON* root = cJSON_Parse(json);
    if (!root) {
        result.message = "Invalid JSON response from the service.";
        return result;
    }

    if (request.kind != RequestKind::Connections) {
        if (!cJSON_IsArray(root)) {
            result.message = "Unexpected stop search response.";
        } else {
            const cJSON* item;
            cJSON_ArrayForEach(item, root) {
                if (!cJSON_IsString(item) || !item->valuestring) {
                    result.stops.clear();
                    result.message = "Unexpected stop search response.";
                    break;
                }
                if (result.stops.size() < MAX_STOPS) result.stops.emplace_back(item->valuestring);
            }
            if (result.message.empty()) {
                result.message = result.stops.empty() ? "No stops found." : "Select a stop below.";
                if (cJSON_GetArraySize(root) > static_cast<int>(MAX_STOPS)) {
                    result.message += " Showing the first 25; refine your search for more.";
                }
            }
        }
        cJSON_Delete(root);
        return result;
    }

    if (!cJSON_IsObject(root)) {
        result.message = "Unexpected connection response.";
    } else {
        const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* connections = cJSON_GetObjectItemCaseSensitive(root, "connections");
        if (cJSON_IsString(error) && error->valuestring && error->valuestring[0]) {
            result.message = "Service error: ";
            result.message += error->valuestring;
        } else if (!cJSON_IsArray(connections)) {
            result.message = "Unexpected connection response.";
        } else if (cJSON_GetArraySize(connections) == 0) {
            result.message = "No connections found.";
        } else {
            result.message = "Verify times and transfers on CP.sk; this unofficial service can make mistakes.\n";
            const cJSON* connection;
            int number = 0;
            cJSON_ArrayForEach(connection, connections) {
                if (!cJSON_IsObject(connection)) {
                    result.message = "Unexpected connection response.";
                    break;
                }
                char heading[32];
                snprintf(heading, sizeof(heading), "\nConnection %d: ", ++number);
                result.message += heading;
                result.message += text(connection, "date");
                result.message += "\n";
                result.message += text(connection, "departure");
                result.message += " - ";
                result.message += text(connection, "arrival");
                optional(result.message, connection, "duration", "  ");
                result.message += "\n";

                const cJSON* legs = cJSON_GetObjectItemCaseSensitive(connection, "legs");
                if (!cJSON_IsArray(legs) || cJSON_GetArraySize(legs) == 0) {
                    result.message += "Leg details unavailable.\n";
                    continue;
                }
                const cJSON* leg;
                cJSON_ArrayForEach(leg, legs) {
                    if (!cJSON_IsObject(leg)) {
                        result.message += "Leg details unavailable.\n";
                        continue;
                    }
                    result.message += "\n";
                    result.message += text(leg, "line");
                    optional(result.message, leg, "type", " (");
                    if (*text(leg, "type", "")) result.message += ")";
                    optional(result.message, leg, "direction", " toward ");
                    result.message += "\n";
                    result.message += text(leg, "dep");
                    result.message += "  ";
                    result.message += text(leg, "from_stop");
                    optional(result.message, leg, "from_platform", "  platform ");
                    result.message += "\n";
                    result.message += text(leg, "arr");
                    result.message += "  ";
                    result.message += text(leg, "to_stop");
                    optional(result.message, leg, "to_platform", "  platform ");
                    result.message += "\n";
                }
                const cJSON* transfers = cJSON_GetObjectItemCaseSensitive(connection, "transfers");
                if (cJSON_IsArray(transfers)) {
                    const cJSON* transfer;
                    cJSON_ArrayForEach(transfer, transfers) {
                        if (!cJSON_IsObject(transfer)) continue;
                        result.message += "\nTransfer: ";
                        result.message += text(transfer, "stop");
                        optional(result.message, transfer, "board_stop", " -> ");
                        optional(result.message, transfer, "walk", "  walk: ");
                        optional(result.message, transfer, "wait", "  wait: ");
                        result.message += "\n";
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    return result;
}

TransitResult transitFetch(const TransitRequest& request) {
    Response response;
    std::string url = transitUrl(request);
    esp_http_client_config_t config {};
    config.url = url.c_str();
    config.timeout_ms = 20000;
    config.event_handler = onHttpEvent;
    config.user_data = &response;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#else
    return {"This firmware has no HTTPS certificate bundle.", {}};
#endif
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return {"Could not create HTTPS client.", {}};
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (response.tooLarge) return {"Service response exceeds 96 KB.", {}};
    if (error != ESP_OK) return {"Request failed. Check Wi-Fi, device time and HTTPS certificates.", {}};
    if (status < 200 || status >= 300) {
        char message[100];
        snprintf(message, sizeof(message), "Service returned HTTP %d. Check your search and try again.", status);
        return {message, {}};
    }
    return transitParse(request, response.body.c_str());
}
