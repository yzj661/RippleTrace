// tinybank - HTTP client facade
#include "tinybank/http_client.h"

#include "tinybank/json.h"
#include "tinybank/log.h"
#include "tinybank/strings.h"

namespace tinybank {

std::string build_get_request(const std::string& host, const std::string& path) {
    return "GET " + path + " HTTP/1.1\r\nHost: " + host +
           "\r\nConnection: close\r\n\r\n";
}

std::string parse_http_body(const std::string& raw) {
    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) return std::string();
    return raw.substr(split + 4);
}

HttpClient::HttpClient(std::string host) : host_(std::move(host)) {}

HttpResponse HttpClient::get(const std::string& path) const {
    HttpResponse response;
    const std::string request = build_get_request(host_, path);
    log_debug("http_client: sending " + std::to_string(request.size()) + " bytes to " + host_);

    // The demo build has no socket layer. A path-dependent canned response keeps
    // the call chain intact while remaining deterministic for tests and demos.
    response.status = 200;
    if (path.find("/v1/complete") != std::string::npos) {
        response.body = "{\"answer\":\"Your balance is 50.00\",\"model\":\"local-demo\"}";
    } else {
        response.body = "{\"status\":\"ok\",\"rate\":\"1.5\"}";
    }
    return response;
}

std::string HttpClient::get_json_field(const std::string& path,
                                       const std::string& key) const {
    const HttpResponse response = get(path);
    if (!response.ok()) {
        log_warn("http_client: request to " + path + " failed");
        return std::string();
    }
    const std::string body = parse_http_body(response.body);
    return json_extract_string(body.empty() ? response.body : body, key);
}

}  // namespace tinybank
