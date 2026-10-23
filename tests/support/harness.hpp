// A deliberately small test harness (no framework headers are installed here).
//
//   TEST_CASE(name) { CHECK(expr); CHECK_EQ(a, b); CHECK_NEAR(a, b, eps); }
//
// A failing CHECK records the failure and keeps going, so one bad assertion
// does not hide the rest of the test.
#pragma once

#include <cmath>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Case {
    std::string name;
    std::function<void()> body;
};

std::vector<Case>& registry();
int run_all();

void fail(const char* file, int line, const std::string& message);
void note(const std::string& test_name);

struct Registrar {
    Registrar(const char* name, std::function<void()> body);
};

template <typename T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

template <typename A, typename B>
void check_equal(const A& a, const B& b, const char* expression, const char* file, int line) {
    if (a == b) return;
    std::ostringstream message;
    message << expression;
    if constexpr (Streamable<A> && Streamable<B>) message << " -- " << a << " != " << b;
    fail(file, line, message.str());
}

inline void check_near(double a, double b, double epsilon, const char* expression, const char* file,
                       int line) {
    if (std::fabs(a - b) <= epsilon) return;
    std::ostringstream message;
    message << expression << " -- " << a << " != " << b << " (within " << epsilon << ")";
    fail(file, line, message.str());
}

// A template so anything with an explicit operator bool (Image, optionals)
// works in a boolean context, which is where it is meant to be used.
template <typename T>
void check_true(const T& value, const char* expression, const char* file, int line) {
    if (value) return;
    fail(file, line, std::string(expression) + " -- expected true");
}

}  // namespace testing

#define TEST_CASE_NAMED(unique, name)                                       \
    static void unique();                                                   \
    static ::testing::Registrar unique##_registrar(#name, unique);          \
    static void unique()

#define TEST_CASE(name) TEST_CASE_NAMED(name, #name)

#define CHECK(expression) ::testing::check_true((expression), #expression, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::testing::check_equal((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) ::testing::check_near((a), (b), (eps), #a " ~= " #b, __FILE__, __LINE__)
