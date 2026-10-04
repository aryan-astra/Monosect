// MonoSect — POSIX process runner.
//
// Used by the development and verification harness on Linux and macOS.  The
// Windows build uses engine/win/WindowsProcessRunner.cpp; both implement the same
// interface so that the executor, the verifier and the job runner are the same
// code on every platform.
//
// Cancellation: the child is put into its own process group with setpgid() and the
// whole group is signalled - SIGTERM first, then SIGKILL after a grace period.
// That is the POSIX equivalent of killing a Windows job object.
#include "engine/ProcessRunner.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace monosect::core {
namespace {

constexpr double kTerminationGraceSeconds = 5.0;

void drainPipe(int handle, const ProcessLineCallback& onLine, std::string* capture, std::size_t maxBytes,
               const std::function<void(const char*, std::size_t)>& onBytes) {
    std::string pending;
    char buffer[8192];
    while (true) {
        const ssize_t read = ::read(handle, buffer, sizeof(buffer));
        if (read > 0) {
            if (onBytes) {
                onBytes(buffer, static_cast<std::size_t>(read));
                continue;
            }
            for (ssize_t i = 0; i < read; ++i) {
                if (buffer[i] == '\n') {
                    if (capture != nullptr && capture->size() < maxBytes) {
                        capture->append(pending);
                        capture->push_back('\n');
                    }
                    if (onLine) {
                        onLine(pending);
                    }
                    pending.clear();
                } else {
                    pending.push_back(buffer[i]);
                }
            }
            continue;
        }
        if (read < 0 && errno == EINTR) {
            continue;
        }
        break;
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
        onBytes(nullptr, 0);   // end-of-stream marker so the consumer can flush a partial line
    }
    ::close(handle);
}

} // namespace

class PosixProcessRunner final : public IProcessRunner {
public:
    ProcessResult run(const ProcessSpec& spec, const ProcessCallbacks& callbacks) override {
        ProcessResult result;
        const auto startTime = std::chrono::steady_clock::now();

        int stdoutPipe[2] = {-1, -1};
        int stderrPipe[2] = {-1, -1};
        if (::pipe(stdoutPipe) != 0) {
            result.launchError = std::string("pipe() failed: ") + std::strerror(errno);
            return result;
        }
        if (::pipe(stderrPipe) != 0) {
            ::close(stdoutPipe[0]);
            ::close(stdoutPipe[1]);
            result.launchError = std::string("pipe() failed: ") + std::strerror(errno);
            return result;
        }

        std::vector<std::string> arguments;
        arguments.reserve(spec.arguments.size() + 1);
        arguments.push_back(spec.program);
        for (const std::string& argument : spec.arguments) {
            arguments.push_back(argument);
        }

        const pid_t child = ::fork();
        if (child < 0) {
            ::close(stdoutPipe[0]);
            ::close(stdoutPipe[1]);
            ::close(stderrPipe[0]);
            ::close(stderrPipe[1]);
            result.launchError = std::string("fork() failed: ") + std::strerror(errno);
            return result;
        }

        if (child == 0) {
            ::setpgid(0, 0);
            ::dup2(stdoutPipe[1], STDOUT_FILENO);
            ::dup2(stderrPipe[1], STDERR_FILENO);
            ::close(stdoutPipe[0]);
            ::close(stdoutPipe[1]);
            ::close(stderrPipe[0]);
            ::close(stderrPipe[1]);
            if (!spec.workingDirectory.empty()) {
                (void)::chdir(spec.workingDirectory.c_str());
            }
            if (spec.lowPriority) {
                (void)::nice(10);
            }
            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);
            for (const std::string& argument : arguments) {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);
            // execvp (not execv): the same PATH search that CreateProcessW does on
            // Windows, so a bare "ffmpeg" behaves the same on both platforms.  The
            // product always passes a full path; this is for the test harness.
            ::execvp(spec.program.c_str(), argv.data());
            std::fprintf(stderr, "MonoSect: could not execute %s: %s\n", spec.program.c_str(),
                         std::strerror(errno));
            ::_exit(127);
        }

        ::close(stdoutPipe[1]);
        ::close(stderrPipe[1]);

        std::string stdoutCapture;
        std::string stderrCapture;
        std::thread stdoutThread(drainPipe, stdoutPipe[0], callbacks.onStdoutLine, &stdoutCapture,
                                 spec.maxCapturedBytes, spec.onStdoutBytes);
        std::thread stderrThread(drainPipe, stderrPipe[0], callbacks.onStderrLine, &stderrCapture,
                                 spec.maxCapturedBytes, nullptr);

        int status = 0;
        bool canceled = false;
        bool childReaped = false;
        bool terminationSent = false;
        std::chrono::steady_clock::time_point terminationStart{};

        while (!childReaped) {
            const pid_t waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) {
                childReaped = true;
                break;
            }
            if (waited < 0 && errno != EINTR) {
                result.launchError = std::string("waitpid() failed: ") + std::strerror(errno);
                break;
            }
            if (callbacks.shouldCancel && callbacks.shouldCancel() && !canceled) {
                canceled = true;
                ::kill(-child, SIGTERM);
                terminationSent = true;
                terminationStart = std::chrono::steady_clock::now();
            }
            if (terminationSent && !childReaped) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - terminationStart).count();
                if (elapsed > kTerminationGraceSeconds) {
                    ::kill(-child, SIGKILL);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        stdoutThread.join();
        stderrThread.join();

        result.launched = true;
        result.canceled = canceled;
        if (WIFEXITED(status)) {
            result.exitCode = WEXITSTATUS(status);
            if (result.exitCode == 127) {
                result.launched = false;
                result.launchError = "the program could not be executed";
            }
        } else if (WIFSIGNALED(status)) {
            result.exitCode = 128 + WTERMSIG(status);
        }
        result.stdoutText = stdoutCapture;
        result.stderrText = stderrCapture;
        result.stderrTail = splitLines(stderrCapture);
        result.wallSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        return result;
    }

    void terminateAll() override {}
};

IProcessRunner& posixProcessRunner() {
    static PosixProcessRunner runner;
    return runner;
}

} // namespace monosect::core
