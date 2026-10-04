// MonoSect — running external processes.
//
// FFmpeg and ffprobe are external programs.  They are always started with an
// explicit argument vector (never through a shell), they are always placed in a
// job object / process group so that cancelling the job cannot leave an orphan
// encoder running, and their output is streamed line by line so the UI can show
// real progress.
#pragma once

#include "core/Result.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace monosect::core {

struct ProcessSpec {
    std::string program;                     // full path to the executable
    std::vector<std::string> arguments;      // arguments, without the program name
    std::string workingDirectory;            // empty = inherit
    bool captureStdout = false;              // collect stdout into ProcessResult::stdoutText
    bool captureStderr = false;              // collect stderr into ProcessResult::stderrText
    bool lowPriority = false;                // run below normal priority
    std::size_t maxCapturedBytes = 32u * 1024u * 1024u;
    /// When set, stdout is delivered as raw bytes instead of text lines.  Used
    /// for `-f data -`, which streams packet payloads: hashing a stream must not
    /// hold it in memory.  `captureStdout` is ignored in that case.
    std::function<void(const char* data, std::size_t bytes)> onStdoutBytes;
};

struct ProcessResult {
    bool launched = false;
    bool canceled = false;
    int exitCode = -1;
    std::string launchError;                 // set when the program could not be started
    std::string stdoutText;
    std::string stderrText;
    std::vector<std::string> stderrTail;     // last lines, always kept (used for error mapping)
    double wallSeconds = 0.0;
};

using ProcessLineCallback = std::function<void(const std::string& line)>;
using CancelCheck = std::function<bool()>;

struct ProcessCallbacks {
    ProcessLineCallback onStdoutLine;
    ProcessLineCallback onStderrLine;
    CancelCheck shouldCancel;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    /// Runs to completion, streaming output through the callbacks.  Cancelling
    /// terminates the process tree and returns `canceled = true`.
    [[nodiscard]] virtual ProcessResult run(const ProcessSpec& spec, const ProcessCallbacks& callbacks) = 0;

    /// Kills every process this runner has started (used when a job is
    /// cancelled and on shutdown).
    virtual void terminateAll() = 0;
};

/// Command line for logs and error messages.  Quoting follows the rules of the
/// platform that is running (documented in docs/RESEARCH.md).
[[nodiscard]] std::string formatCommandLineForDisplay(const ProcessSpec& spec);

/// Runs `program` with `arguments`, discards its stdout and returns true when it
/// exited with code 0.  Used for single-purpose tools (ffmpeg -version, ...).
[[nodiscard]] ProcessResult runQuiet(IProcessRunner& runner, const std::string& program,
                                     const std::vector<std::string>& arguments);

/// Splits captured text into lines, dropping the trailing empty line.
[[nodiscard]] std::vector<std::string> splitLines(const std::string& text);

} // namespace monosect::core
