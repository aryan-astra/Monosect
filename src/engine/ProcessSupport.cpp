#include "engine/ProcessRunner.hpp"
#include "engine/ProcessRunners.hpp"

namespace monosect::core {

IProcessRunner& defaultProcessRunner() {
#if defined(_WIN32)
    return windowsProcessRunner();
#else
    return posixProcessRunner();
#endif
}

namespace {

bool needsPosixQuoting(const std::string& argument) {
    if (argument.empty()) {
        return true;
    }
    for (const char c : argument) {
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\' || c == '$' || c == '&' || c == '|' ||
            c == ';' || c == '<' || c == '>' || c == '(' || c == ')' || c == '*' || c == '?' || c == '#' ||
            c == '!' || c == '~' || c == '\n') {
            return true;
        }
    }
    return false;
}

bool needsWindowsQuoting(const std::string& argument) {
    if (argument.empty()) {
        return true;
    }
    for (const char c : argument) {
        if (c == ' ' || c == '\t' || c == '"' || c == '^' || c == '&' || c == '|' || c == '<' || c == '>' ||
            c == '(' || c == ')') {
            return true;
        }
    }
    return false;
}

std::string quotePosix(const std::string& argument) {
    std::string out = "'";
    for (const char c : argument) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

/// Windows quoting follows the CommandLineToArgvW rules: backslashes are only
/// special in front of a quote.
std::string quoteWindows(const std::string& argument) {
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

} // namespace

std::string formatCommandLineForDisplay(const ProcessSpec& spec) {
#if defined(_WIN32)
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    std::string text;
    const auto append = [&text, windows](const std::string& argument) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        if (windows) {
            text += needsWindowsQuoting(argument) ? quoteWindows(argument) : argument;
        } else {
            text += needsPosixQuoting(argument) ? quotePosix(argument) : argument;
        }
    };
    append(spec.program);
    for (const std::string& argument : spec.arguments) {
        append(argument);
    }
    return text;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

} // namespace monosect::core
