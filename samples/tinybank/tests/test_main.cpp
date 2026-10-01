// tinybank - test runner
#include <iostream>

#include "tinybank/log.h"

int test_sha256_suite();
int test_domain_suite();
int test_service_suite();
int test_integration_suite();

int main() {
    tinybank::set_log_level(tinybank::LogLevel::Error);   // keep output readable

    std::cout << "=== tinybank test run ===\n";
    std::cout << "[suite] sha256\n";
    test_sha256_suite();
    std::cout << "[suite] domain\n";
    test_domain_suite();
    std::cout << "[suite] services\n";
    test_service_suite();
    std::cout << "[suite] integration\n";
    test_integration_suite();

    std::cout << "=== done ===\n";
    return 0;
}
