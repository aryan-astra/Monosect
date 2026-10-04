// MonoSect — the small test framework used by every tests/core file.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace monosect::test {

struct TestCase {
    const char* name;
    void (*function)();
};

[[nodiscard]] std::vector<TestCase>& registry();
extern const char* g_currentTest;
extern int g_failures;
extern int g_checks;

void reportFailure(const char* file, int line, const std::string& message);
void reportInequality(const char* file, int line, const char* expression, const std::string& expected,
                      const std::string& actual);
void reportFalse(const char* file, int line, const char* expression);

/// Registers a test case at load time.
struct Registrar {
    Registrar(const char* name, void (*function)()) { registry().push_back(TestCase{name, function}); }
};

/// Value -> text used by the macros.  Defined for the types the tests compare.
[[nodiscard]] std::string describeBytes(std::string_view value);

template <typename T>
[[nodiscard]] std::string toText(const T& value) {
    if constexpr (std::is_convertible_v<T, std::string_view>) {
        return std::string(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(value);
    } else {
        return std::string("<?>");
    }
}

} // namespace monosect::test

#define MONOSECT_TEST(name)                                                                       \
    static void monosect_test_##name();                                                           \
    namespace {                                                                                   \
    const ::monosect::test::Registrar monosect_registrar_##name(#name, &monosect_test_##name);    \
    }                                                                                             \
    static void monosect_test_##name()

#define CHECK(expression)                                                                         \
    do {                                                                                          \
        ++::monosect::test::g_checks;                                                             \
        if (!(expression)) {                                                                      \
            ::monosect::test::reportFalse(__FILE__, __LINE__, #expression);                        \
        }                                                                                         \
    } while (false)

#define CHECK_EQ(expected, actual)                                                                \
    do {                                                                                          \
        ++::monosect::test::g_checks;                                                             \
        const auto monosect_expected_value = (expected);                                          \
        const auto monosect_actual_value = (actual);                                              \
        if (!(monosect_expected_value == monosect_actual_value)) {                                \
            ::monosect::test::reportInequality(__FILE__, __LINE__, #actual,                       \
                                               ::monosect::test::toText(monosect_expected_value), \
                                               ::monosect::test::toText(monosect_actual_value));  \
        }                                                                                         \
    } while (false)

#define CHECK_NEAR(expected, actual, tolerance)                                                   \
    do {                                                                                          \
        ++::monosect::test::g_checks;                                                             \
        const double monosect_expected_value = static_cast<double>(expected);                     \
        const double monosect_actual_value = static_cast<double>(actual);                         \
        const double monosect_difference = monosect_actual_value - monosect_expected_value;       \
        if (!(monosect_difference <= (tolerance) && monosect_difference >= -(tolerance))) {       \
            ::monosect::test::reportInequality(__FILE__, __LINE__, #actual,                       \
                                               ::monosect::test::toText(monosect_expected_value), \
                                               ::monosect::test::toText(monosect_actual_value));  \
        }                                                                                         \
    } while (false)
