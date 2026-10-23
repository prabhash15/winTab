#include "harness.hpp"

#include <cstdio>

namespace testing {
namespace {

std::string g_current_name;
int g_failures_in_test = 0;
int g_checks = 0;

}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(const char* name, std::function<void()> body) {
    registry().push_back(Case{name, std::move(body)});
}

void note(const std::string& test_name) {
    g_current_name = test_name;
    g_failures_in_test = 0;
}

void fail(const char* file, int line, const std::string& message) {
    g_failures_in_test += 1;
    std::fprintf(stderr, "FAIL %s\n  %s:%d: %s\n", g_current_name.c_str(), file, line,
                 message.c_str());
}

int run_all() {
    int failures = 0;
    int passed = 0;
    for (const Case& test : registry()) {
        note(test.name);
        g_checks += 1;
        try {
            test.body();
        } catch (const std::exception& exc) {
            fail("<exception>", 0, exc.what());
        }
        if (g_failures_in_test == 0) {
            ++passed;
        } else {
            failures += 1;
        }
    }
    std::printf("%d test(s), %d passed, %d failed\n", static_cast<int>(registry().size()), passed,
                failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace testing

int main() { return testing::run_all(); }
