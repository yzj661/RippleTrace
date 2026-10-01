// ============================================================
//  RippleTrace - cli_utf8.h
//  Lossless command-line arguments.
//
//  main(int, char**) gives arguments decoded with the ANSI code page, so a path
//  containing non-ASCII characters arrives mangled:
//      cannot read E:\VS??\?????1\...\main.cpp
//  The wide command line is the only lossless source. This helper has to be
//  used by every executable in the project - it was first written inline in the
//  main CLI and the diagnostic tools kept failing on the same input until they
//  shared it.
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace rippletrace {

// UTF-8 argv (argv[0] is the program name, matching the C convention).
// Falls back to an empty vector on platforms without a wide command line.
std::vector<std::string> utf8Args();

// Same, but derived from an explicit argc/argv pair on non-Windows platforms.
void setFallbackArgs(int argc, char** argv);

}  // namespace rippletrace
