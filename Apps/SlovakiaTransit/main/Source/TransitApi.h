#pragma once

#include <string>
#include <vector>

enum class RequestKind { Connections, FromStops, ToStops };

struct TransitRequest {
    RequestKind kind;
    std::string from;
    std::string to;
    std::string date;
    std::string time;
};

struct TransitResult {
    std::string message;
    std::vector<std::string> stops;
};

std::string transitUrl(const TransitRequest& request);
TransitResult transitParse(const TransitRequest& request, const char* json);
TransitResult transitFetch(const TransitRequest& request);
