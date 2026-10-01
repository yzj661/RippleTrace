// ============================================================
//  RippleTrace - web.h
//  A dependency-free HTTP/1.1 server that exposes the analysis engine
//  as a small JSON API, plus the static single-page front end.
//
//  Design notes:
//   * No third-party libraries: plain winsock, so the same "zero
//     dependency" property that holds for the CLI holds for the server.
//   * The analysis core is reused verbatim. The server only does request
//     parsing, path sandboxing and JSON projection - it never duplicates
//     graph logic.
//   * Requests are served sequentially. This is a local single-user tool,
//     so a thread pool would add failure modes without adding value.
// ============================================================
#pragma once

#include <string>

namespace rippletrace {

struct ServeOptions {
    std::string host = "127.0.0.1";
    int port = 8080;
    // Directory that may be analysed. Requests are refused if they resolve
    // outside it, so the server cannot be used to read arbitrary paths.
    std::string allowedRoot;
    std::string webRoot;          // where index.html lives
    bool verbose = true;
};

// Runs until the process is killed. Returns a process exit code.
int runWebServer(const ServeOptions& options);

}  // namespace rippletrace
