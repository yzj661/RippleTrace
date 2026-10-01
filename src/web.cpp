// ============================================================
//  RippleTrace - web.cpp
//  Minimal HTTP/1.1 server + JSON API over the analysis engine.
// ============================================================
#include "web.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "ai_explain.h"
#include "diff.h"
#include "graph.h"
#include "impact.h"
#include "json.h"
#include "parser.h"
#include "repo.h"
#include "report.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#  define RIPPLETRACE_INVALID_SOCKET INVALID_SOCKET
#  define rippletrace_close_socket closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
#  define RIPPLETRACE_INVALID_SOCKET (-1)
#  define rippletrace_close_socket close
#endif

namespace rippletrace {
namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------
//  small helpers
// ------------------------------------------------------------

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            auto hexVal = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hexVal(in[i + 1]);
            int lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Split "a=1&b=2" into a map.
std::map<std::string, std::string> parseForm(const std::string& body) {
    std::map<std::string, std::string> out;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            out[urlDecode(pair)] = "";
        } else {
            out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
    }
    return out;
}

// ------------------------------------------------------------
//  JSON request body helpers.
//
//  The API accepts JSON, but a full JSON parser is unnecessary here.
//  These helpers extract only what the endpoints need and are strict
//  enough to reject malformed input instead of guessing.
// ------------------------------------------------------------

// Read a JSON string array: {"fn": ["a","b"]} or {"fn": "a"}
std::vector<std::string> jsonReadStringList(const std::string& body, const std::string& key) {
    std::vector<std::string> out;
    size_t pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return out;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return out;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return out;

    if (body[pos] == '[') {
        ++pos;
        while (pos < body.size() && body[pos] != ']') {
            if (body[pos] == '"') {
                std::string value;
                ++pos;
                while (pos < body.size() && body[pos] != '"') {
                    if (body[pos] == '\\' && pos + 1 < body.size()) {
                        ++pos;
                        switch (body[pos]) {
                            case 'n': value.push_back('\n'); break;
                            case 't': value.push_back('\t'); break;
                            case 'r': value.push_back('\r'); break;
                            case '"': value.push_back('"');  break;
                            case '\\': value.push_back('\\'); break;
                            case '/': value.push_back('/');  break;
                            default: value.push_back(body[pos]);
                        }
                    } else {
                        value.push_back(body[pos]);
                    }
                    ++pos;
                }
                ++pos;
                if (!value.empty()) out.push_back(value);
            } else {
                ++pos;
            }
        }
    } else if (body[pos] == '"') {
        std::string value;
        ++pos;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) ++pos;
            value.push_back(body[pos]);
            ++pos;
        }
        if (!value.empty()) out.push_back(value);
    }
    return out;
}

std::string jsonReadString(const std::string& body, const std::string& key,
                           const std::string& fallback = "") {
    auto list = jsonReadStringList(body, key);
    if (!list.empty()) return list.front();
    return fallback;
}

// ------------------------------------------------------------
//  path resolution
// ------------------------------------------------------------

std::string normaliseAbsolute(const std::string& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(utf8ToPath(p), ec);
    if (ec) {
        abs = fs::absolute(utf8ToPath(p), ec);
        if (ec) return std::string();
    }
    std::string s = pathToUtf8(abs);
    // drop a trailing slash so prefix comparison is predictable
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// Separator-insensitive containment test.
//
// Windows paths reach this function in both forms: a path normalised through
// std::filesystem comes back with backslashes, while the normaliser used here
// emits forward slashes. Comparing raw bytes then fails on a completely valid
// path - the sandbox check rejected a valid sandbox-relative path purely
// because the separators differed, with the bytes otherwise identical.
bool isInside(const std::string& childIn, const std::string& parentIn) {
    if (parentIn.empty()) return true;    // no sandbox configured
    if (childIn == parentIn) return true;

    auto slashify = [](std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    const std::string child = slashify(childIn);
    const std::string parent = slashify(parentIn);

    if (child == parent) return true;
    if (child.size() <= parent.size()) return false;
    if (child.compare(0, parent.size(), parent) != 0) return false;
    return child[parent.size()] == '/';
}

// Resolve a request path against the server's default root. Absolute paths are
// intentionally accepted by the local web UI so users can analyse a project
// outside the directory used to launch the server.
//
// Most requests send a path relative to the root the server was started with
// ("myproject/src"), and that must be JOINED to the root before it can be
// normalised or compared. Treating the relative path as absolute made every
// such request fail the prefix check and return 403 - including the default
// case of "analyse the sandbox root itself".
std::string resolveAgainstRoot(const std::string& requested, const std::string& root) {
    if (requested.empty()) return normaliseAbsolute(root);
    std::error_code ec;
    fs::path p = utf8ToPath(requested);
    if (!p.is_absolute() && !root.empty()) {
        p = utf8ToPath(root) / p;
    }
    return normaliseAbsolute(pathToUtf8(p));
}

// ------------------------------------------------------------
//  analysis services (thin wrappers over the existing engine)
// ------------------------------------------------------------

struct RepoAnalysis {
    bool ok = false;
    std::string error;
    std::string resolvedRoot;
    CallGraph graph;
};

RepoAnalysis analyzeRepo(const std::string& dir) {
    RepoAnalysis result;

    std::error_code ec;
    if (!fs::exists(utf8ToPath(dir), ec) || !fs::is_directory(utf8ToPath(dir), ec)) {
        result.error = "directory not found: " + dir;
        return result;
    }
    result.resolvedRoot = normaliseAbsolute(dir);
    if (result.resolvedRoot.empty()) {
        result.error = "cannot resolve directory: " + dir;
        return result;
    }

    ScanOptions opt;
    std::vector<SourceFile> files = scanRepository(result.resolvedRoot, opt);
    if (files.empty()) {
        result.error = "no supported sources found under " + result.resolvedRoot;
        return result;
    }

    std::map<std::string, std::unique_ptr<IRepoParser>> parsers;
    for (const auto& sf : files) {
        if (sf.language == nullptr) continue;
        std::string text;
        if (!readFileText(sf.absolutePath, text)) continue;
        auto pit = parsers.find(sf.language->id);
        if (pit == parsers.end()) {
            pit = parsers.emplace(sf.language->id, makeParser(*sf.language)).first;
        }
        if (!pit->second) continue;
        ParseResult pr;
        pit->second->parseUnit(sf.repoRelativePath, text, sf.isHeader, pr);
        result.graph.addParse(pr);
        result.graph.noteLanguageFile(sf.language->displayName);
    }
    result.graph.resolve();
    result.ok = true;
    return result;
}

// ------------------------------------------------------------
//  JSON projections
// ------------------------------------------------------------

std::string scanToJson(const RepoAnalysis& analysis, int topN) {
    const CallGraph& g = analysis.graph;
    GraphStats st = g.stats();

    std::ostringstream o;
    o << "{";
    o << "\"ok\":true,";
    o << "\"root\":\"" << jsonEscape(analysis.resolvedRoot) << "\",";
    o << "\"stats\":{"
      << "\"functions\":" << st.functions
      << ",\"edges\":" << st.edges
      << ",\"unresolved\":" << st.unresolvedEdges
      << ",\"tests\":" << st.tests
      << ",\"entryPoints\":" << st.entryPoints
      << ",\"publicApi\":" << st.publicApi
      << ",\"ambiguous\":" << st.ambiguousNames
      << "},";

    // every function, with the fields the UI needs
    auto fns = g.allFunctions();
    o << "\"functions\":[";
    for (size_t i = 0; i < fns.size(); ++i) {
        const RepoFunction* f = fns[i];
        if (i) o << ",";
        o << "{"
          << "\"id\":\"" << jsonEscape(f->qualified) << "\","
          << "\"file\":\"" << jsonEscape(f->file) << "\","
          << "\"line\":" << f->declLine << ","
          << "\"fanIn\":" << g.callersOf(f->qualified).size() << ","
          << "\"fanOut\":" << f->directCallCount << ","
          << "\"lines\":" << f->lineCount << ","
          << "\"cyclomatic\":" << f->cyclomatic << ","
          << "\"isTest\":" << (f->isTest ? "true" : "false") << ","
          << "\"isPublicApi\":" << (f->isPublicApi ? "true" : "false") << ","
          << "\"isEntryPoint\":" << (f->isEntryPoint ? "true" : "false")
          << "}";
    }
    o << "],";

    // top blast-radius ranking, computed server-side so the UI stays dumb
    struct Row { const RepoFunction* f; size_t fanIn; };
    std::vector<Row> rows;
    rows.reserve(fns.size());
    for (const RepoFunction* f : fns) rows.push_back({f, g.callersOf(f->qualified).size()});
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.fanIn != b.fanIn) return a.fanIn > b.fanIn;
        return a.f->qualified < b.f->qualified;
    });
    o << "\"topFanIn\":[";
    for (size_t i = 0; i < rows.size() && i < static_cast<size_t>(topN); ++i) {
        const RepoFunction* f = rows[i].f;
        if (i) o << ",";
        o << "{"
          << "\"id\":\"" << jsonEscape(f->qualified) << "\","
          << "\"file\":\"" << jsonEscape(f->file) << "\","
          << "\"line\":" << f->declLine << ","
          << "\"fanIn\":" << rows[i].fanIn << ","
          << "\"lines\":" << f->lineCount << ","
          << "\"isTest\":" << (f->isTest ? "true" : "false")
          << "}";
    }
    o << "]";
    o << "}";
    return o.str();
}

// Layered graph for the visualisation: depth 0 = the change, increasing
// depth = callers further away. The UI draws one column per depth.
std::string impactToJson(const CallGraph& g, const ImpactReport& rep,
                         const std::vector<std::string>& requested) {
    std::ostringstream o;
    o << "{";
    o << "\"ok\":true,";

    o << "\"requested\":[";
    for (size_t i = 0; i < requested.size(); ++i) {
        if (i) o << ",";
        o << "\"" << jsonEscape(requested[i]) << "\"";
    }
    o << "],";

    o << "\"unknown\":[";
    for (size_t i = 0; i < rep.unknownChanged.size(); ++i) {
        if (i) o << ",";
        o << "\"" << jsonEscape(rep.unknownChanged[i]) << "\"";
    }
    o << "],";

    int critical = 0, high = 0, medium = 0, low = 0;
    for (const auto& im : rep.impacted) {
        switch (im.risk) {
            case Risk::Critical: ++critical; break;
            case Risk::High:     ++high;     break;
            case Risk::Medium:   ++medium;   break;
            default:             ++low;      break;
        }
    }
    o << "\"summary\":{"
      << "\"changed\":" << rep.changed.size()
      << ",\"impacted\":" << rep.impacted.size()
      << ",\"maxDepth\":" << rep.maxDepthReached
      << ",\"tests\":" << rep.tests.size()
      << ",\"conflicts\":" << rep.conflicts.size()
      << ",\"critical\":" << critical
      << ",\"high\":" << high
      << ",\"medium\":" << medium
      << ",\"low\":" << low
      << ",\"resolvedCallees\":" << rep.resolvedCalleeCount
      << ",\"unresolvedCallees\":" << rep.unresolvedCalleeCount
      << "},";

    // ---- nodes grouped by depth (for the layered layout) ----
    std::map<int, std::vector<const ImpactedFunction*>> byDepth;
    for (const auto& im : rep.impacted) byDepth[im.depth].push_back(&im);

    o << "\"layers\":[";
    // depth 0 layer: the changed functions themselves
    o << "{\"depth\":0,\"nodes\":[";
    for (size_t i = 0; i < rep.changed.size(); ++i) {
        if (i) o << ",";
        const RepoFunction* f = g.find(rep.changed[i]);
        o << "{\"id\":\"" << jsonEscape(rep.changed[i]) << "\""
          << ",\"file\":\"" << jsonEscape(f ? f->file : std::string()) << "\""
          << ",\"line\":" << (f ? f->declLine : 0)
          << ",\"risk\":\"CHANGED\",\"score\":100,\"fanIn\":0"
          << ",\"isTest\":false,\"isPublicApi\":" << (f && f->isPublicApi ? "true" : "false")
          << "}";
    }
    o << "]}";

    for (const auto& kv : byDepth) {
        o << ",{\"depth\":" << kv.first << ",\"nodes\":[";
        const auto& nodes = kv.second;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const ImpactedFunction* im = nodes[i];
            if (i) o << ",";
            o << "{\"id\":\"" << jsonEscape(im->qualified) << "\""
              << ",\"file\":\"" << jsonEscape(im->file) << "\""
              << ",\"line\":" << im->line
              << ",\"risk\":\"" << riskName(im->risk) << "\""
              << ",\"score\":" << im->riskScore
              << ",\"fanIn\":" << im->fanIn
              << ",\"isTest\":" << (im->isTest ? "true" : "false")
              << ",\"isPublicApi\":" << (im->isPublicApi ? "true" : "false")
              << "}";
        }
        o << "]}";
    }
    o << "],";

    // ---- flat ranked list with reasons and paths ----
    o << "\"impacted\":[";
    for (size_t i = 0; i < rep.impacted.size(); ++i) {
        const ImpactedFunction& im = rep.impacted[i];
        if (i) o << ",";
        o << "{"
          << "\"id\":\"" << jsonEscape(im.qualified) << "\","
          << "\"file\":\"" << jsonEscape(im.file) << "\","
          << "\"line\":" << im.line << ","
          << "\"depth\":" << im.depth << ","
          << "\"fanIn\":" << im.fanIn << ","
          << "\"risk\":\"" << riskName(im.risk) << "\","
          << "\"score\":" << im.riskScore << ","
          << "\"isTest\":" << (im.isTest ? "true" : "false") << ","
          << "\"isPublicApi\":" << (im.isPublicApi ? "true" : "false") << ","
          << "\"reasons\":[";
        for (size_t k = 0; k < im.reasons.size(); ++k) {
            if (k) o << ",";
            o << "\"" << jsonEscape(im.reasons[k]) << "\"";
        }
        o << "],\"path\":[";
        for (size_t k = 0; k < im.path.size(); ++k) {
            if (k) o << ",";
            o << "\"" << jsonEscape(im.path[k]) << "\"";
        }
        o << "]}";
    }
    o << "],";

    o << "\"tests\":[";
    for (size_t i = 0; i < rep.tests.size(); ++i) {
        if (i) o << ",";
        o << "{\"id\":\"" << jsonEscape(rep.tests[i].qualified) << "\""
          << ",\"file\":\"" << jsonEscape(rep.tests[i].file) << "\""
          << ",\"distance\":" << rep.tests[i].distance << "}";
    }
    o << "],";

    o << "\"conflicts\":[";
    for (size_t i = 0; i < rep.conflicts.size(); ++i) {
        const ConflictPair& cp = rep.conflicts[i];
        if (i) o << ",";
        o << "{\"a\":\"" << jsonEscape(cp.a) << "\""
          << ",\"b\":\"" << jsonEscape(cp.b) << "\""
          << ",\"sharedCallers\":" << cp.sharedCallers
          << ",\"note\":\"" << jsonEscape(cp.note) << "\"}";
    }
    o << "],";

    o << "\"notes\":[";
    for (size_t i = 0; i < rep.ambiguityNotes.size(); ++i) {
        if (i) o << ",";
        o << "\"" << jsonEscape(rep.ambiguityNotes[i]) << "\"";
    }
    o << "]";

    o << "}";
    return o.str();
}

// ------------------------------------------------------------
//  HTTP plumbing
// ------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> form;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

HttpResponse jsonError(int status, const std::string& message) {
    HttpResponse r;
    r.status = status;
    r.body = "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}";
    return r;
}

// Read one HTTP request from the socket.
//
// Reading until the peer closes is WRONG here: an HTTP client keeps the
// connection open while it waits for our response, so a "read to EOF" loop
// deadlocks against it. Headers are read until the blank line, then exactly
// Content-Length bytes of body are read.
bool recvRequest(socket_t sock, std::string& out) {
    char buf[8192];
    out.clear();

    size_t headerEnd = std::string::npos;
    size_t bodyOffset = 0;

    // ---- headers ----
    while (out.size() < 64u * 1024u) {
        headerEnd = out.find("\r\n\r\n");
        if (headerEnd != std::string::npos) { bodyOffset = headerEnd + 4; break; }
        headerEnd = out.find("\n\n");
        if (headerEnd != std::string::npos) { bodyOffset = headerEnd + 2; break; }
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
    }
    if (bodyOffset == 0) return false;   // header block never terminated

    // ---- content length ----
    size_t contentLength = 0;
    {
        std::string head = toLower(out.substr(0, bodyOffset));
        size_t p = head.find("content-length:");
        if (p != std::string::npos) {
            size_t e = head.find("\r\n", p);
            std::string digits = head.substr(p + 15, (e == std::string::npos ? head.size() : e) - (p + 15));
            try { contentLength = static_cast<size_t>(std::stoul(digits)); } catch (...) { contentLength = 0; }
        }
    }
    if (contentLength > 32u * 1024u * 1024u) return false;   // hard cap

    // ---- body ----
    while (out.size() < bodyOffset + contentLength) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;   // truncated body: parse what we have
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

// Parse headers plus however much body arrived in the same read.
bool parseRequest(const std::string& raw, HttpRequest& req) {
    size_t headerEnd = raw.find("\r\n\r\n");
    size_t sepLen = 4;
    if (headerEnd == std::string::npos) {
        headerEnd = raw.find("\n\n");
        sepLen = 2;
        if (headerEnd == std::string::npos) return false;
    }

    std::string head = raw.substr(0, headerEnd);
    std::istringstream hs(head);
    std::string line;
    if (!std::getline(hs, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    {
        std::istringstream ls(line);
        std::string version;
        ls >> req.method >> req.target >> version;
        if (req.method.empty() || req.target.empty()) return false;
    }

    size_t q = req.target.find('?');
    if (q == std::string::npos) {
        req.path = req.target;
    } else {
        req.path = req.target.substr(0, q);
        req.query = req.target.substr(q + 1);
    }

    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = toLower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        req.headers[key] = value;
    }

    req.body = raw.substr(headerEnd + sepLen);
    if (!req.body.empty()) req.form = parseForm(req.body);
    return true;
}

bool sendAll(socket_t sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void sendResponse(socket_t sock, const HttpResponse& res) {
    std::ostringstream o;
    o << "HTTP/1.1 " << res.status << " " << statusText(res.status) << "\r\n";
    o << "Content-Type: " << res.contentType << "\r\n";
    o << "Content-Length: " << res.body.size() << "\r\n";
    o << "Cache-Control: no-store\r\n";
    o << "Connection: close\r\n";
    for (const auto& h : res.extraHeaders) {
        o << h.first << ": " << h.second << "\r\n";
    }
    o << "\r\n";
    o << res.body;
    sendAll(sock, o.str());
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// ------------------------------------------------------------
//  configuration passed to the request handler
// ------------------------------------------------------------
struct ServerConfig {
    std::string allowedRoot;
    std::string webRoot;
    bool verbose = true;
    // Cache of the last analysed repo so that a page session does not
    // re-parse the same tree for every interaction.
    std::shared_ptr<RepoAnalysis> cached;
    // The impact report the page is currently looking at. /api/explain must
    // validate the model against exactly this set, and re-running the whole
    // analysis on every explain request would be wasteful.
    std::shared_ptr<ImpactReport> cachedReport;
    std::vector<std::string> cachedRequested;
    // AI settings come from the server environment only. The API key is never
    // accepted from, or sent to, the browser.
    AiOptions ai;
};

std::string resolveWebRoot() {
    // index.html lives next to the executable's project folder in the
    // layout used by this repo: <project>/web/index.html
    std::vector<std::string> candidates = {
        "web",
        "../web",
        "../../web",
        "rippletrace/web"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(utf8ToPath(c) / "index.html", ec)) {
            return normaliseAbsolute(c);
        }
    }
    return std::string();
}

// Re-read the AI configuration when it was disabled at startup.
//
// Environment variables cannot change under a running process, but an
// rippletrace.env file can. Requiring a full restart just to pick up a file that
// was created a moment ago was one of the complaints in the problem report,
// so the config is re-checked (cheaply - it is a small text file) whenever a
// request arrives and the layer is still off. When it is still off the fresh
// options are kept anyway, because they carry the diagnostics that /api/health
// and the page need to explain what is missing.
void refreshAiConfigIfDisabled(ServerConfig& cfg) {
    if (cfg.ai.enabled) return;
    AiOptions refreshed = loadAiOptionsFromEnv();
    refreshed.verbose = false;
    const bool turnedOn = refreshed.enabled;
    cfg.ai = std::move(refreshed);
    if (turnedOn) {
        std::cout << "[RippleTrace] AI layer became configured after startup (source: "
                  << cfg.ai.keySource << ")\n";
        std::cout.flush();
    }
}

HttpResponse handleRequest(const HttpRequest& req, ServerConfig& cfg) {
    const std::string method = req.method;
    std::string path = req.path;

    // ---------- static front end ----------
    if (path == "/" || path == "/index.html") {
        if (cfg.webRoot.empty()) {
            return jsonError(500, "web root not found: expected web/index.html next to the project");
        }
        std::string html;
        if (!readTextFile(pathToUtf8(utf8ToPath(cfg.webRoot) / "index.html"), html)) {
            return jsonError(500, "cannot read index.html");
        }
        HttpResponse r;
        r.contentType = "text/html; charset=utf-8";
        r.body = std::move(html);
        return r;
    }

    // ---------- API ----------
    if (path == "/api/health") {
        // Report the sandbox root and whether it looks like RippleTrace's own sample
        // tree. The page needs both: the root so it can default the path box to
        // something that actually exists, and the sample hint so it only
        // auto-loads the demo when the demo is really there. Hard-coding
        // "samples/tinybank" in the page broke as soon as the server was started
        // on a different directory.
        refreshAiConfigIfDisabled(cfg);

        json::Value root = json::Value::makeObject();
        root.set("ok", json::Value::makeBool(true));
        root.set("tool", json::Value::makeString("rippletrace"));
        root.set("version", json::Value::makeString("0.1.0"));
        root.set("sandboxRoot", json::Value::makeString(cfg.allowedRoot));
        const bool hasSamples =
            !cfg.allowedRoot.empty() &&
            std::filesystem::exists(utf8ToPath(cfg.allowedRoot) / "samples" / "tinybank");
        root.set("hasSamples", json::Value::makeBool(hasSamples));

        // AI readiness, reported up front so the page can show it before the
        // user clicks and gets a 503. This carries WHERE the config came from
        // and which files would be read - never the key, which exists only in
        // this process.
        json::Value ai = json::Value::makeObject();
        ai.set("enabled", json::Value::makeBool(cfg.ai.enabled));
        ai.set("model", json::Value::makeString(cfg.ai.model));
        ai.set("endpoint", json::Value::makeString(cfg.ai.endpoint));
        ai.set("keySource", json::Value::makeString(cfg.ai.keySource));
        ai.set("configFile", json::Value::makeString(cfg.ai.configFile));
        ai.set("language", json::Value::makeString(cfg.ai.language));
        ai.set("helperAvailable", json::Value::makeBool(cfg.ai.helperEnabled));
        json::Value candidates = json::Value::makeArray();
        for (const auto& c : aiConfigFileCandidates()) {
            candidates.push(json::Value::makeString(c));
        }
        ai.set("configCandidates", candidates);
        root.set("ai", ai);

        HttpResponse r;
        r.body = json::serialize(root);
        return r;
    }

    if (path == "/api/scan") {
        auto params = parseForm(req.query);
        std::string dir = params.count("path") ? params["path"] : cfg.allowedRoot;
        if (dir.empty()) return jsonError(400, "no repository path given");

        std::string resolved = resolveAgainstRoot(dir, cfg.allowedRoot);
        if (resolved.empty()) return jsonError(400, "cannot resolve path: " + dir);
        auto analysis = std::make_shared<RepoAnalysis>(analyzeRepo(resolved));
        if (!analysis->ok) return jsonError(400, analysis->error);
        cfg.cached = analysis;
        return HttpResponse{200, "application/json; charset=utf-8",
                            scanToJson(*analysis, 12), {}};
    }

    if (path == "/api/impact" || path == "/api/diff") {
        if (method != "POST") return jsonError(405, "use POST for " + path);

        std::string dir = jsonReadString(req.body, "path");
        if (dir.empty()) dir = cfg.allowedRoot;
        if (dir.empty()) return jsonError(400, "no repository path given");

        std::string resolved = resolveAgainstRoot(dir, cfg.allowedRoot);
        if (resolved.empty()) return jsonError(400, "cannot resolve path: " + dir);
        // Reuse the cached graph when the same repo was scanned already.
        std::shared_ptr<RepoAnalysis> analysis;
        if (cfg.cached && cfg.cached->ok && cfg.cached->resolvedRoot == resolved) {
            analysis = cfg.cached;
        } else {
            analysis = std::make_shared<RepoAnalysis>(analyzeRepo(resolved));
            if (!analysis->ok) return jsonError(400, analysis->error);
            cfg.cached = analysis;
        }

        std::vector<std::string> requested = jsonReadStringList(req.body, "fn");

        if (path == "/api/diff") {
            std::string patchPath = jsonReadString(req.body, "patch");
            std::string patchText = jsonReadString(req.body, "patchText");
            if (patchText.empty()) {
                if (patchPath.empty()) return jsonError(400, "provide 'patch' or 'patchText'");
                std::string p = patchPath;
                if (!utf8ToPath(p).is_absolute()) p = pathToUtf8(utf8ToPath(resolved) / utf8ToPath(patchPath));
                if (!readFileText(p, patchText)) {
                    return jsonError(400, "cannot read patch: " + patchPath);
                }
            }
            DiffParseResult d = parseUnifiedDiff(patchText);
            auto touched = functionsTouchedByDiff(analysis->graph, d);
            requested.clear();
            for (const auto& t : touched) requested.push_back(t.qualified);
            if (requested.empty()) {
                return jsonError(400,
                                 "no function body in this repository overlaps the patch hunks");
            }
        }

        if (requested.empty()) return jsonError(400, "provide at least one function name ('fn')");

        ImpactOptions iopt;
        auto depthIt = req.form.find("maxDepth");
        (void)depthIt;
        ImpactAnalyzer analyzer(analysis->graph);
        ImpactReport rep = analyzer.analyze(requested, iopt);

        // Remember this report: /api/explain validates the model against exactly
        // the impact set the user is looking at.
        cfg.cachedReport = std::make_shared<ImpactReport>(rep);
        cfg.cachedRequested = requested;

        return HttpResponse{200, "application/json; charset=utf-8",
                            impactToJson(analysis->graph, rep, requested), {}};
    }

    // ---------------- AI explanation ----------------
    // The impact set is NOT recomputed here and the model cannot influence it:
    // we reuse the cached report that /api/impact just produced and validate the
    // model's reply against it in enforceGrounding().
    if (path == "/api/explain") {
        if (method != "POST") return jsonError(405, "use POST for /api/explain");

        if (!cfg.cached || !cfg.cached->ok || !cfg.cachedReport) {
            return jsonError(400,
                             "run an impact analysis first: the explanation is validated "
                             "against the impact set that analysis produced");
        }
        if (!cfg.ai.enabled) {
            // The config file may have been created since the server started.
            refreshAiConfigIfDisabled(cfg);
        }
        if (!cfg.ai.enabled) {
            // A disabled feature must say how to enable it, not fail vaguely.
            return jsonError(503,
                             "the AI layer is not configured on the server. Provide the key "
                             "either as the RIPPLETRACE_AI_KEY environment variable before starting "
                             "the server, or in an rippletrace.env file in the project folder "
                             "(rippletrace.env.example shows the format). Optionally set "
                             "RIPPLETRACE_AI_ENDPOINT and RIPPLETRACE_AI_MODEL as well. The API key is "
                             "only ever read on the server side and is never sent to the "
                             "browser.");
        }

        AiOptions aiOpt = cfg.ai;
        aiOpt.verbose = false;
        auto client = makeChatClient(aiOpt);
        AiExplanation ex = explainImpact(cfg.cached->graph, *cfg.cachedReport, aiOpt,
                                         client.get());

        json::Value root = json::Value::makeObject();
        root.set("ok", json::Value::makeBool(true));   // request handled; see ai.ok
        appendAiExplanationJson(root, ex);
        return HttpResponse{200, "application/json; charset=utf-8",
                            json::serialize(root), {}};
    }

    return jsonError(404, "no such endpoint: " + path);
}

}  // namespace

int runWebServer(const ServeOptions& options) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[RippleTrace] WSAStartup failed\n";
        return 1;
    }
#endif

    ServerConfig cfg;
    cfg.allowedRoot = normaliseAbsolute(options.allowedRoot.empty() ? "." : options.allowedRoot);
    cfg.webRoot = options.webRoot.empty() ? resolveWebRoot() : normaliseAbsolute(options.webRoot);
    cfg.verbose = options.verbose;
    // AI configuration is read from the server environment only; the key never
    // travels to the browser.
    cfg.ai = loadAiOptionsFromEnv();
    cfg.ai.verbose = false;

    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == RIPPLETRACE_INVALID_SOCKET) {
        std::cerr << "[RippleTrace] cannot create socket\n";
        return 1;
    }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(options.port));
    if (inet_pton(AF_INET, options.host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[RippleTrace] invalid host: " << options.host << "\n";
        rippletrace_close_socket(listener);
        return 1;
    }

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        RIPPLETRACE_INVALID_SOCKET) {
        std::cerr << "[RippleTrace] cannot bind " << options.host << ":" << options.port
                  << " (port already in use?)\n";
        rippletrace_close_socket(listener);
        return 1;
    }
    if (listen(listener, 16) == RIPPLETRACE_INVALID_SOCKET) {
        std::cerr << "[RippleTrace] listen failed\n";
        rippletrace_close_socket(listener);
        return 1;
    }

    std::cout << "\n";
    std::cout << "  涟漪 RippleTrace web UI\n";
    std::cout << "  ------------------------------------------------\n";
    std::cout << "  URL          : http://" << options.host << ":" << options.port << "/\n";
    std::cout << "  Sandbox root : "
              << (cfg.allowedRoot.empty() ? std::string("(none)") : cfg.allowedRoot) << "\n";
    std::cout << "  Web assets   : "
              << (cfg.webRoot.empty() ? std::string("NOT FOUND - check web/index.html")
                                      : cfg.webRoot)
              << "\n";
    if (cfg.ai.enabled) {
        const char* source = cfg.ai.keySource == "environment" ? "environment variable"
                           : cfg.ai.keySource == "file"        ? "config file"
                                                               : "unknown";
        std::cout << "  AI layer     : enabled  (model " << cfg.ai.model << ")\n";
        std::cout << "                 endpoint " << cfg.ai.endpoint << "\n";
        std::cout << "                 key from " << source;
        if (!cfg.ai.configFile.empty()) std::cout << " (" << cfg.ai.configFile << ")";
        std::cout << "\n";
        if (!cfg.ai.helperEnabled) {
            std::cout << "                 note: no helper script found; if the native TLS\n"
                         "                 stack is unavailable the call will fail.\n";
        }
        std::cout << "                 the explanation is validated against the computed\n";
        std::cout << "                 impact set; the key stays on this process.\n";
    } else {
        std::cout << "  AI layer     : disabled (no key found)\n";
        std::cout << "                 set RIPPLETRACE_AI_KEY, or create a rippletrace.env file:\n";
        for (const auto& c : aiConfigFileCandidates()) {
            std::cout << "                   " << c << "\n";
        }
        std::cout << "                 rippletrace.env.example shows the format. Scanned once\n";
        std::cout << "                 per request, so creating the file takes effect\n";
        std::cout << "                 without restarting this server.\n";
    }
    std::cout << "  ------------------------------------------------\n";
    std::cout << "  Absolute paths are allowed; relative paths resolve from the default root.\n";
    std::cout << "  Press Ctrl+C to stop.\n\n";
    std::cout.flush();

    for (;;) {
        sockaddr_in client{};
#if defined(_WIN32)
        int clientLen = sizeof(client);
#else
        socklen_t clientLen = sizeof(client);
#endif
        socket_t sock = accept(listener, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (sock == RIPPLETRACE_INVALID_SOCKET) continue;

        // A stalled client must not block the server forever.
#if defined(_WIN32)
        DWORD timeoutMs = 15000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
        timeval tv{};
        tv.tv_sec = 15;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        std::string raw;
        if (!recvRequest(sock, raw)) {
            rippletrace_close_socket(sock);
            continue;
        }

        HttpRequest req;
        if (!parseRequest(raw, req)) {
            sendResponse(sock, jsonError(400, "malformed HTTP request"));
            rippletrace_close_socket(sock);
            continue;
        }

        HttpResponse res;
        try {
            res = handleRequest(req, cfg);
        } catch (const std::exception& ex) {
            res = jsonError(500, std::string("internal error: ") + ex.what());
        } catch (...) {
            res = jsonError(500, "internal error");
        }

        if (cfg.verbose) {
            std::cout << "  " << req.method << " " << req.target << " -> " << res.status
                      << " (" << res.body.size() << " bytes)\n";
            std::cout.flush();
        }

        sendResponse(sock, res);
        rippletrace_close_socket(sock);
    }

    rippletrace_close_socket(listener);
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}

}  // namespace rippletrace
