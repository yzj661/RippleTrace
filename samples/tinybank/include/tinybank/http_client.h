// tinybank - HTTP client facade
// Layer: transport. Upper layers call fetch_json() rather than talking to a
// socket directly, which keeps the demo repo realistic and gives the impact
// analysis a long call chain to walk.
#pragma once

#include <map>
#include <string>

namespace tinybank {

struct HttpResponse {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// Build a "GET /path HTTP/1.1\r\nHost: ..." request string.
std::string build_get_request(const std::string& host, const std::string& path);

// Extract the body from a raw HTTP response ("\r\n\r\n" separator).
std::string parse_http_body(const std::string& raw);

// Very small request abstraction; the real implementation would use a socket.
class HttpClient {
public:
    explicit HttpClient(std::string host);

    HttpResponse get(const std::string& path) const;
    std::string get_json_field(const std::string& path, const std::string& key) const;

    const std::string& host() const { return host_; }

private:
    std::string host_;
};

}  // namespace tinybank
