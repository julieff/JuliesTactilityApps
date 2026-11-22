// Host tests. With a C++ compiler and libcjson development package installed,
// build and run from Apps/SlovakiaTransit/tests:
// c++ -std=c++17 -Ifakes -I../main/Source $(pkg-config --cflags libcjson) TransitApiTest.cpp ../main/Source/TransitApi.cpp $(pkg-config --libs libcjson) -o /tmp/slovakia-transit-test
// /tmp/slovakia-transit-test
// Repeat the build with -DCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=0 to test firmware
// without a certificate bundle. Neither build opens a network connection.

#include "TransitApi.h"
#include "esp_http_client.h"
#include "sdkconfig.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

void expect(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(EXIT_FAILURE);
    }
}

const TransitRequest connections {RequestKind::Connections, "Bratislava", "Košice", "", ""};
const TransitRequest fromStops {RequestKind::FromStops, "Žilina +", "Nitra&", "2026-11-22", "08:30"};
const TransitRequest toStops {RequestKind::ToStops, fromStops.from, fromStops.to,
                             fromStops.date, fromStops.time};

void expectMessage(const TransitResult& result, const char* expected) {
    if (result.message != expected) {
        std::fprintf(stderr, "Expected message: %s\nActual message: %s\n",
                     expected, result.message.c_str());
        std::exit(EXIT_FAILURE);
    }
    expect(result.stops.empty(), "message-only result has no selectable stops");
}

void testUrls() {
    const std::string connectionUrl =
        "https://cpskapi.lib.tuke.sk/mhd?mode=trainbus&from=Bratislava&to=Ko%C5%A1ice";
    expect(transitUrl(connections) == connectionUrl, "omit both empty date and time");
    TransitRequest dated = connections;
    dated.date = "2026-11-22";
    expect(transitUrl(dated) == connectionUrl + "&date=2026-11-22", "date works without time");
    dated.time = "08:30";
    expect(transitUrl(dated) == connectionUrl + "&date=2026-11-22&time=08%3A30",
           "include both date and time");
    dated.date.clear();
    expect(transitUrl(dated) == connectionUrl + "&time=08%3A30", "time works without date");

    expect(transitUrl(fromStops) == "https://cpskapi.lib.tuke.sk/mhd-stops?mode=trainbus&q=%C5%BDilina%20%2B",
           "origin lookup encodes UTF-8, space and plus, and ignores date and time");
    expect(transitUrl(toStops) == "https://cpskapi.lib.tuke.sk/mhd-stops?mode=trainbus&q=Nitra%26",
           "destination lookup encodes ampersand and uses only the destination");

    TransitRequest reserved {RequestKind::Connections, "A-_.~", "B&to=X/#?%=+", "", ""};
    expect(transitUrl(reserved) ==
               "https://cpskapi.lib.tuke.sk/mhd?mode=trainbus&from=A-_.~&to=B%26to%3DX%2F%23%3F%25%3D%2B",
           "preserve unreserved characters and encode delimiters inside a place name");
}

std::string stopArray(int count, const char* extraItem = nullptr) {
    std::string json = "[";
    for (int i = 1; i <= count; ++i) {
        if (i != 1) json += ',';
        json += "\"Stop " + std::to_string(i) + "\"";
    }
    if (extraItem) {
        if (count != 0) json += ',';
        json += extraItem;
    }
    return json + ']';
}

void testStops() {
    const auto stops = transitParse(toStops, R"(["Žilina, stanica","Košice"])");
    expect(stops.stops == std::vector<std::string>{"Žilina, stanica", "Košice"},
           "keep stop names intact and in service order");
    expect(stops.message == "Select a stop below.", "prompt on successful lookup");
    expectMessage(transitParse(fromStops, "[]"), "No stops found.");
    expectMessage(transitParse(fromStops, "{}"), "Unexpected stop search response.");
    expectMessage(transitParse(fromStops, R"(["Valid",null])"), "Unexpected stop search response.");
    expectMessage(transitParse(toStops, "["), "Invalid JSON response from the service.");

    const auto atLimit = transitParse(fromStops, stopArray(25).c_str());
    expect(atLimit.stops.size() == 25 && atLimit.stops.back() == "Stop 25",
           "keep all 25 stops at the limit");
    expect(atLimit.message == "Select a stop below.", "do not show truncation at the limit");

    const auto overLimit = transitParse(fromStops, stopArray(26).c_str());
    expect(overLimit.stops == atLimit.stops, "keep the first 25 stops when there are more");
    expect(overLimit.message == "Select a stop below. Showing the first 25; refine your search for more.",
           "explain truncation to the user");

    expectMessage(transitParse(fromStops, stopArray(25, "{}").c_str()),
                  "Unexpected stop search response.");
}

void testConnectionErrors() {
    expectMessage(transitParse(connections, "not JSON"), "Invalid JSON response from the service.");
    for (const char* json : {"[]", "{}", R"({"connections":null})", R"({"connections":{}})",
                             R"({"connections":[{},7]})"}) {
        expectMessage(transitParse(connections, json), "Unexpected connection response.");
    }
    expectMessage(transitParse(connections, R"({"error":"Unknown origin","connections":[]})"),
                  "Service error: Unknown origin");
    for (const char* json : {R"({"connections":[]})", R"({"error":"","connections":[]})",
                             R"({"error":false,"connections":[]})"}) {
        expectMessage(transitParse(connections, json), "No connections found.");
    }
}

void testConnectionDetails() {
    const auto result = transitParse(connections, R"({
        "connections": [
            {
                "date":"2026-11-22","departure":"23:48","arrival":"01:20","duration":"1 h 32 min",
                "legs":[
                    {"line":"R 601","type":"Train","direction":"Žilina","dep":"23:48",
                     "from_stop":"Bratislava","from_platform":"4","arr":"00:30","to_stop":"Trnava"},
                    {"line":"205","dep":"00:40","from_stop":"Trnava AS","arr":"01:20",
                     "to_stop":"Nitra","to_platform":"2"}
                ],
                "transfers":[null,{"stop":"Trnava","board_stop":"Trnava AS","walk":"5 min","wait":"10 min"}]
            },
            {"date":"2026-11-23","departure":"02:00","arrival":"04:00","legs":[]}
        ]
    })");
    expectMessage(result,
        "Verify times and transfers on CP.sk; this unofficial service can make mistakes.\n"
        "\nConnection 1: 2026-11-22\n23:48 - 01:20  1 h 32 min\n"
        "\nR 601 (Train) toward Žilina\n23:48  Bratislava  platform 4\n00:30  Trnava\n"
        "\n205\n00:40  Trnava AS\n01:20  Nitra  platform 2\n"
        "\nTransfer: Trnava -> Trnava AS  walk: 5 min  wait: 10 min\n"
        "\nConnection 2: 2026-11-23\n02:00 - 04:00\nLeg details unavailable.\n");
}

void testIncompleteDetails() {
    const char* advisory =
        "Verify times and transfers on CP.sk; this unofficial service can make mistakes.\n";
    const char* heading = "\nConnection 1: ?\n? - ?\n";
    for (const char* json : {R"({"connections":[{}]})", R"({"connections":[{"legs":false}]})"}) {
        expectMessage(transitParse(connections, json),
                      (std::string(advisory) + heading + "Leg details unavailable.\n").c_str());
    }

    expectMessage(transitParse(connections, R"({"connections":[{
        "Date":"ignored","departure":42,"arrival":"","duration":false,
        "legs":[null,{"line":"","type":7,"direction":"","dep":"06:00","from_platform":3}],
        "transfers":[{"stop":"Trnava","board_stop":"","walk":5}]
    }]})"), (std::string(advisory) + heading +
             "Leg details unavailable.\n\n?\n06:00  ?\n?  ?\n\nTransfer: Trnava\n").c_str());
}

struct HttpScenario {
    bool initFails = false;
    esp_err_t performResult = ESP_OK;
    int status = 200;
    std::vector<std::string> chunks;
    std::string url;
    int timeoutMs = 0;
    bool usesCertificateBundle = false;
    int inits = 0;
    int performs = 0;
    int cleanups = 0;
};

HttpScenario http;

} // namespace

struct esp_http_client {
    esp_http_client_config_t config;
};

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    ++http.inits;
    http.url = config->url;
    http.timeoutMs = config->timeout_ms;
    http.usesCertificateBundle = config->crt_bundle_attach != nullptr;
    return http.initFails ? nullptr : new esp_http_client{*config};
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
    ++http.performs;
    char header[] = "Content-Type: application/json";
    esp_http_client_event_t event {
        HTTP_EVENT_ON_HEADER, client->config.user_data, header, static_cast<int>(sizeof(header) - 1)
    };
    if (client->config.event_handler(&event) != ESP_OK) return ESP_FAIL;
    for (auto& chunk : http.chunks) {
        event = {HTTP_EVENT_ON_DATA, client->config.user_data, chunk.data(), static_cast<int>(chunk.size())};
        if (client->config.event_handler(&event) != ESP_OK) return ESP_FAIL;
    }
    return http.performResult;
}

int esp_http_client_get_status_code(esp_http_client_handle_t) {
    return http.status;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    ++http.cleanups;
    delete client;
    return ESP_OK;
}

namespace {

void testFetch() {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    http = {};
    http.status = 299;
    http.chunks = {"[\"Žil", "", "ina\"]"};
    const auto result = transitFetch(fromStops);
    expect(result.stops == std::vector<std::string>{"Žilina"}, "parse split HTTP data and ignore headers");
    expect(http.url == transitUrl(fromStops) && http.timeoutMs == 20000 && http.usesCertificateBundle,
           "configure URL, bounded timeout and certificate verification");
    expect(http.inits == 1 && http.performs == 1 && http.cleanups == 1,
           "perform and clean up a successful request exactly once");

    http = {};
    http.initFails = true;
    expectMessage(transitFetch(connections), "Could not create HTTPS client.");
    expect(http.inits == 1 && http.performs == 0 && http.cleanups == 0,
           "do not perform or clean up an uninitialized client");

    http = {};
    http.performResult = ESP_FAIL;
    http.chunks = {"[\"Valid\"]"};
    expectMessage(transitFetch(fromStops),
                  "Request failed. Check Wi-Fi, device time and HTTPS certificates.");
    expect(http.cleanups == 1, "clean up after transport failure");

    for (int status : {199, 300, 503}) {
        http = {};
        http.status = status;
        http.chunks = {"[\"Valid\"]"};
        const auto message = "Service returned HTTP " + std::to_string(status) +
                             ". Check your search and try again.";
        expectMessage(transitFetch(fromStops), message.c_str());
        expect(http.cleanups == 1, "clean up after an unsuccessful HTTP status");
    }

    constexpr std::size_t responseLimit = 96 * 1024;
    http = {};
    http.chunks = {"[]", std::string(responseLimit - 2, ' ')};
    expectMessage(transitFetch(fromStops), "No stops found.");
    expect(http.cleanups == 1, "clean up after a response at the size limit");

    http.chunks.emplace_back(" ");
    http.cleanups = 0;
    expectMessage(transitFetch(fromStops), "Service response exceeds 96 KB.");
    expect(http.cleanups == 1, "clean up after the cumulative response limit is exceeded");
#else
    expectMessage(transitFetch(connections), "This firmware has no HTTPS certificate bundle.");
    expect(http.inits == 0, "do not start an HTTPS request without the certificate bundle");
#endif
}

} // namespace

int main() {
    testUrls();
    testStops();
    testConnectionErrors();
    testConnectionDetails();
    testIncompleteDetails();
    testFetch();
    std::puts("Transit API tests passed.");
}
