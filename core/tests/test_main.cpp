#include <cstdio>
#include <cstring>
#include <exception>

#include "test_framework.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;  // optional substring filter
    int run = 0;
    for (const auto& tc : testfw::registry()) {
        if (filter && !std::strstr(tc.name, filter)) continue;
        const int before = testfw::failures();
        ++run;
        try {
            tc.fn();
        } catch (const std::exception& e) {
            testfw::report_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            testfw::report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        std::printf("[%s] %s\n", testfw::failures() == before ? " OK " : "FAIL", tc.name);
    }
    std::printf("\n%d test(s) run, %d check failure(s)\n", run, testfw::failures());
    return testfw::failures() == 0 && run > 0 ? 0 : 1;
}
