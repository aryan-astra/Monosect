// MonoSect — Windows process runner.
//
// Rules this file follows, each traceable to the Microsoft documentation named
// beside it:
//
//  * CreateProcessW (Win32 Process Creation) is used directly, never
//    `cmd.exe /c`.  A file name with a quotation mark, a percent sign or an
//    ampersand is data, never syntax.
//  * The argument string is built with the quoting rules documented for
//    CommandLineToArgvW (backslashes are only special in front of a quote), so
//    what the child parses is exactly the vector the caller passed.
//  * The child is created suspended, assigned to a Job Object, and only then
//    resumed.  The job object has JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (Job Objects
//    documentation), so when MonoSect exits - normally or by crashing - every
//    FFmpeg process it started is terminated by the kernel.  This is why the
//    runner can promise that a cancelled or crashed job never leaves an encoder
//    writing in the background.
//  * Cancelling terminates the *job* (TerminateJobObject), which kills the
//    whole tree - FFmpeg alone does not always kill its own children.
//  * stdout/stderr are read with ReadFile on anonymous pipes from dedicated
//    threads; the pipe is drained continuously, because FFmpeg blocks writing to
//    a full pipe and would otherwise deadlock (a documented pitfall of
//    CreateProcess with pipes).
//  * Low priority is set with SetPriorityClass(BELOW_NORMAL_PRIORITY_CLASS) on
//    the child, which is the documented way to run a child below normal.
//  * stdin is NUL rather than an inherited handle, so FFmpeg can never try to
//    read from the console MonoSect was started from.  Every caller also passes
//    -nostdin.
//
// Windows-only verification: this translation unit is compiled and exercised only
// by the Windows build, which is where the product runs and where the end-to-end
// suite executes.  A POSIX build compiles PosixProcessRunner.cpp instead, and the
// two implement the same interface, so the engine above them is identical code on
// both platforms.
#if defined(_WIN32)

#include "engine/ProcessRunner.hpp"

#include <windows.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace monosect::core {
namespace {

constexpr double kTerminationGraceSeconds = 5.0;

/// Quotes one argument for CreateProcessW according to the CommandLineToArgvW
/// rules.  A quote inside an argument is escaped with a backslash, and the
/// backslashes that precede it are doubled.
std::string quoteArgument(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
        return argument;
    }
    std::string out;
    out.push_back('"');
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

std::string buildCommandLine(const std::string& program, const std::vector<std::string>& arguments) {
    std::string line = quoteArgument(program);
    for (const std::string& argument : arguments) {
        line.push_back(' ');
        line += quoteArgument(argument);
    }
    return line;
}

std::string windowsErrorMessage(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                              FORMAT_MESSAGE_IGNORE_INSERTS,
                                          nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message;
    if (length > 0 && buffer != nullptr) {
        // The message is wide; convert it to UTF-8 for the log.
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr,
                                               nullptr);
        if (size > 0) {
            message.resize(static_cast<std::size_t>(size));
            ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), message.data(), size, nullptr,
                                  nullptr);
        }
        ::LocalFree(buffer);
    }
    if (message.empty()) {
        message = "Windows error " + std::to_string(error);
    }
    return message;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

/// Reads one pipe until it is closed, splitting text into lines.  Runs on its own
/// thread so that a full pipe can never block the child (documented behaviour:
/// the writer blocks until someone reads).
void drainPipe(HANDLE handle, const ProcessLineCallback& onLine, std::string* capture, std::size_t maxBytes,
               const std::function<void(const char*, std::size_t)>& onBytes) {
    std::string pending;
    char buffer[8192];
    while (true) {
        DWORD read = 0;
        const BOOL ok = ::ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr);
        if (!ok || read == 0) {
            break;
        }
        if (onBytes) {
            onBytes(buffer, static_cast<std::size_t>(read));
            continue;
        }
        for (DWORD i = 0; i < read; ++i) {
            if (buffer[i] == '\n') {
                if (capture != nullptr && capture->size() < maxBytes) {
                    capture->append(pending);
                    capture->push_back('\n');
                }
                if (onLine) {
                    onLine(pending);
                }
                pending.clear();
            } else if (buffer[i] != '\r') {
                pending.push_back(buffer[i]);
            }
        }
    }
    if (!pending.empty()) {
        if (capture != nullptr && capture->size() < maxBytes) {
            capture->append(pending);
        }
        if (onLine) {
            onLine(pending);
        }
    }
    if (onBytes) {
        onBytes(nullptr, 0);
    }
    ::CloseHandle(handle);
}

/// A job object per runner instance.  Everything the runner starts goes into it,
/// and closing the handle (which happens when MonoSect exits, even after a
/// crash) kills whatever is still running.
HANDLE ensureJobObject() {
    static HANDLE job = []() -> HANDLE {
        HANDLE created = ::CreateJobObjectW(nullptr, nullptr);
        if (created == nullptr) {
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            ::CloseHandle(created);
            return nullptr;
        }
        return created;
    }();
    return job;
}

} // namespace

class WindowsProcessRunner final : public IProcessRunner {
public:
    ProcessResult run(const ProcessSpec& spec, const ProcessCallbacks& callbacks) override {
        ProcessResult result;
        const auto startTime = std::chrono::steady_clock::now();

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;

        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE stderrWrite = nullptr;
        if (!::CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0) ||
            !::CreatePipe(&stderrRead, &stderrWrite, &attributes, 0)) {
            result.launchError = "the pipes for the child process could not be created: " +
                                 windowsErrorMessage(::GetLastError());
            return result;
        }
        // The reading ends are used by this process only; the child must not
        // inherit them, or the pipe would never report end-of-file.
        ::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = stdoutWrite;
        startup.hStdError = stderrWrite;
        // No console and no stdin: FFmpeg must never try to read from a terminal
        // (the callers always pass -nostdin as well).
        startup.hStdInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                          OPEN_EXISTING, 0, nullptr);

        PROCESS_INFORMATION process{};
        std::string commandLine = buildCommandLine(spec.program, spec.arguments);
        std::wstring wideCommand = widen(commandLine);
        std::wstring wideWorkingDirectory = widen(spec.workingDirectory);

        const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
        const BOOL created = ::CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                              wideWorkingDirectory.empty() ? nullptr : wideWorkingDirectory.c_str(),
                                              &startup, &process);
        ::CloseHandle(stdoutWrite);
        ::CloseHandle(stderrWrite);
        if (startup.hStdInput != INVALID_HANDLE_VALUE && startup.hStdInput != nullptr) {
            ::CloseHandle(startup.hStdInput);
        }
        if (!created) {
            const DWORD error = ::GetLastError();
            ::CloseHandle(stdoutRead);
            ::CloseHandle(stderrRead);
            result.launchError = windowsErrorMessage(error);
            return result;
        }

        // Assign to the job object *before* the child runs a single instruction,
        // so a crash of MonoSect between here and the first read cannot leave the
        // child behind.
        HANDLE job = ensureJobObject();
        if (job != nullptr) {
            ::AssignProcessToJobObject(job, process.hProcess);
        }
        if (spec.lowPriority) {
            ::SetPriorityClass(process.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
        }
        ::ResumeThread(process.hThread);

        std::string stdoutCapture;
        std::string stderrCapture;
        std::thread stdoutThread(drainPipe, stdoutRead, callbacks.onStdoutLine, &stdoutCapture,
                                 spec.maxCapturedBytes, spec.onStdoutBytes);
        std::thread stderrThread(drainPipe, stderrRead, callbacks.onStderrLine, &stderrCapture,
                                 spec.maxCapturedBytes, nullptr);

        bool canceled = false;
        DWORD waitResult = WAIT_TIMEOUT;
        while (true) {
            waitResult = ::WaitForSingleObject(process.hProcess, 50);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            if (callbacks.shouldCancel && callbacks.shouldCancel() && !canceled) {
                canceled = true;
                if (job != nullptr) {
                    ::TerminateJobObject(job, 1);
                } else {
                    ::TerminateProcess(process.hProcess, 1);
                }
                // Grace: give the process a moment to die after termination.
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(static_cast<int>(kTerminationGraceSeconds * 1000));
                while (std::chrono::steady_clock::now() < deadline) {
                    if (::WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
                        break;
                    }
                }
                break;
            }
        }

        // Wait for the reader threads: the process has ended, so the pipes see
        // end-of-file and both threads finish.
        stdoutThread.join();
        stderrThread.join();

        DWORD exitCode = 0;
        ::GetExitCodeProcess(process.hProcess, &exitCode);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);

        result.launched = true;
        result.canceled = canceled;
        result.exitCode = static_cast<int>(exitCode);
        result.stdoutText = stdoutCapture;
        result.stderrText = stderrCapture;
        result.stderrTail = splitLines(stderrCapture);
        result.wallSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        return result;
    }

    void terminateAll() override {
        if (HANDLE job = ensureJobObject(); job != nullptr) {
            ::TerminateJobObject(job, 1);
        }
    }
};

IProcessRunner& windowsProcessRunner() {
    static WindowsProcessRunner runner;
    return runner;
}

} // namespace monosect::core

#endif // _WIN32
