// MonoSect — unit test runner.
//
// No external test framework: the rules that must never regress are tested with
// a few macros, so that building the tests needs nothing but the compiler.  A
// failing check reports the file, the line and both values, and the runner exits
// non-zero - that is all a CI job needs.
#include "TestSupport.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace monosect::test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

const char* g_currentTest = "";

int g_failures = 0;
int g_checks = 0;

std::string describeBytes(std::string_view value) {
    std::string text;
    for (char c : value) {
        if (c == '\n') {
            text += "\\n";
        } else if (c == '\r') {
            text += "\\r";
        } else if (c == '\t') {
            text += "\\t";
        } else if (static_cast<unsigned char>(c) < 32) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\x%02x", static_cast<unsigned char>(c));
            text += buffer;
        } else {
            text += c;
        }
    }
    return text;
}

void reportFailure(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::printf("FAIL %s (%s:%d)\n     %s\n", g_currentTest, file, line, message.c_str());
}

void reportInequality(const char* file, int line, const char* expression, const std::string& expected,
                      const std::string& actual) {
    reportFailure(file, line, std::string(expression) + "\n     expected: [" + describeBytes(expected) +
                            "]\n     actual:   [" + describeBytes(actual) + "]");
}

void reportFalse(const char* file, int line, const char* expression) {
    reportFailure(file, line, std::string(expression) + " is false");
}

} // namespace monosect::test

int main(int argc, char** argv) {
    // --verbose prints each test name as it starts, which is how a crash inside a
    // test (rather than a failed check) is located.
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--verbose" || std::string(argv[i]) == "-v") {
            verbose = true;
        }
    }
    int passed = 0;
    for (const monosect::test::TestCase& testCase : monosect::test::registry()) {
        monosect::test::g_currentTest = testCase.name;
        if (verbose) {
            std::printf("RUN  %s\n", testCase.name);
            std::fflush(stdout);
        }
        const int before = monosect::test::g_failures;
        // A test that throws (a bad variant access on a failed parse, say) is a
        // failure of that test, not a reason to stop testing everything else.
        try {
            testCase.function();
        } catch (const std::exception& error) {
            monosect::test::reportFailure(__FILE__, __LINE__,
                                          std::string("the test threw an exception: ") + error.what());
        } catch (...) {
            monosect::test::reportFailure(__FILE__, __LINE__, "the test threw a non-standard exception");
        }
        if (monosect::test::g_failures == before) {
            ++passed;
        }
    }
    std::printf("\n%d test(s) passed, %d failed, %d check(s) run\n", passed,
                static_cast<int>(monosect::test::registry().size()) - passed, monosect::test::g_checks);
    return monosect::test::g_failures == 0 ? 0 : 1;
}
