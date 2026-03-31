#include "engine/FFmpegProcess.hpp"

#include <windows.h>

#include <sstream>
#include <thread>

namespace vsm {

namespace {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::wstring quoteArg(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring escaped = L"\"";
    for (wchar_t c : arg) {
        if (c == L'\"') {
            escaped += L"\\\"";
        } else {
            escaped.push_back(c);
        }
    }
    escaped += L"\"";
    return escaped;
}

std::wstring buildCommandLine(const std::string& program, const std::vector<std::string>& args) {
    std::wstringstream stream;
    stream << quoteArg(utf8ToWide(program));
    for (const auto& arg : args) {
        stream << L' ' << quoteArg(utf8ToWide(arg));
    }
    return stream.str();
}

std::string readPipe(HANDLE handle) {
    std::string data;
    constexpr DWORD bufferSize = 4096;
    char buffer[bufferSize];
    DWORD bytesRead = 0;
    while (ReadFile(handle, buffer, bufferSize, &bytesRead, nullptr) && bytesRead > 0) {
        data.append(buffer, buffer + bytesRead);
    }
    return data;
}

void streamPipeLines(HANDLE handle, std::string& aggregate, const FFmpegProcess::OutputCallback& callback) {
    constexpr DWORD bufferSize = 4096;
    char buffer[bufferSize];
    DWORD bytesRead = 0;
    std::string pending;

    while (ReadFile(handle, buffer, bufferSize, &bytesRead, nullptr) && bytesRead > 0) {
        aggregate.append(buffer, buffer + bytesRead);
        pending.append(buffer, buffer + bytesRead);
        std::size_t lineStart = 0;
        while (true) {
            const std::size_t newline = pending.find('\n', lineStart);
            if (newline == std::string::npos) {
                pending.erase(0, lineStart);
                break;
            }
            std::string line = pending.substr(lineStart, newline - lineStart);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (callback) {
                callback(line);
            }
            lineStart = newline + 1;
        }
    }

    if (!pending.empty()) {
        if (!pending.empty() && pending.back() == '\r') {
            pending.pop_back();
        }
        if (callback) {
            callback(pending);
        }
    }
}

} // namespace

FFmpegProcess::FFmpegProcess(std::string ffmpegBinary, std::string ffprobeBinary)
    : ffmpegBinary_(std::move(ffmpegBinary)), ffprobeBinary_(std::move(ffprobeBinary)) {}

FFmpegProcess::Result FFmpegProcess::runFFmpeg(
    const std::vector<std::string>& args,
    const OutputCallback& onStdErr,
    const CancelCallback& shouldCancel) const {
    return runProcess(ffmpegBinary_, args, nullptr, onStdErr, shouldCancel);
}

FFmpegProcess::Result FFmpegProcess::runFFprobe(const std::vector<std::string>& args) const {
    return runProcess(ffprobeBinary_, args, nullptr, nullptr, nullptr);
}

namespace {

std::string formatWindowsError(DWORD code) {
    LPSTR messageBuffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string message = "Win32 error " + std::to_string(code);
    if (size > 0 && messageBuffer != nullptr) {
        message += ": ";
        message.append(messageBuffer, messageBuffer + size);
        LocalFree(messageBuffer);
    }
    return message;
}

} // namespace

FFmpegProcess::Result FFmpegProcess::runProcess(
    const std::string& program,
    const std::vector<std::string>& args,
    const OutputCallback& onStdOut,
    const OutputCallback& onStdErr,
    const CancelCallback& shouldCancel) const {
    Result result;

    if (program.empty()) {
        result.stdErr = "Program path is empty";
        return result;
    }

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;

    if (!CreatePipe(&outRead, &outWrite, &securityAttributes, 0)) {
        result.stdErr = "Failed to create stdout pipe: " + formatWindowsError(GetLastError());
        return result;
    }

    if (!CreatePipe(&errRead, &errWrite, &securityAttributes, 0)) {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        result.stdErr = "Failed to create stderr pipe: " + formatWindowsError(GetLastError());
        return result;
    }

    if (!SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        CloseHandle(errRead);
        CloseHandle(errWrite);
        result.stdErr = "Failed to set handle inheritance flags: " + formatWindowsError(GetLastError());
        return result;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(STARTUPINFOW);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = outWrite;
    startupInfo.hStdError = errWrite;

    PROCESS_INFORMATION processInfo{};
    auto commandLine = buildCommandLine(program, args);
    std::wstring mutableCommand = commandLine;

    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    CloseHandle(outWrite);
    CloseHandle(errWrite);

    if (!created) {
        CloseHandle(outRead);
        CloseHandle(errRead);
        result.stdErr = "Failed to start process: " + formatWindowsError(GetLastError());
        return result;
    }

    std::string stdOutBuffer;
    std::string stdErrBuffer;

    std::thread outReader([&]() {
        if (onStdOut) {
            streamPipeLines(outRead, stdOutBuffer, onStdOut);
            return;
        }
        stdOutBuffer = readPipe(outRead);
    });
    std::thread errReader([&]() {
        if (onStdErr) {
            streamPipeLines(errRead, stdErrBuffer, onStdErr);
            return;
        }
        stdErrBuffer = readPipe(errRead);
    });

    DWORD exitCode = 1;
    bool canceled = false;
    while (true) {
        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 50);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_FAILED) {
            result.stdErr = "Failed waiting for process: " + formatWindowsError(GetLastError());
            break;
        }

        if (shouldCancel && shouldCancel()) {
            TerminateProcess(processInfo.hProcess, 130);
            canceled = true;
            break;
        }
    }

    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
        if (!result.stdErr.empty()) {
            result.stdErr += "\n";
        }
        result.stdErr += "Failed to get process exit code: " + formatWindowsError(GetLastError());
        exitCode = 1;
    }

    if (canceled) {
        exitCode = 130;
    }

    if (outReader.joinable()) {
        outReader.join();
    }
    if (errReader.joinable()) {
        errReader.join();
    }

    result.stdOut = std::move(stdOutBuffer);
    if (!result.stdErr.empty() && !stdErrBuffer.empty()) {
        result.stdErr += "\n";
    }
    result.stdErr += std::move(stdErrBuffer);
    result.exitCode = static_cast<int>(exitCode);
    result.ok = (exitCode == 0) && !canceled;
    if (canceled) {
        if (!result.stdErr.empty()) {
            result.stdErr += "\n";
        }
        result.stdErr += "Process canceled";
    }

    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    return result;
}

}
