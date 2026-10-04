// MonoSect — error taxonomy and a small result type.
//
// Design rules that matter for this product:
//  * Every error carries a human sentence (what happened), a detail string (the
//    values that were involved) and a remedy (what the user can do).  The UI
//    shows the sentence, the log keeps all three.  "Something went wrong" is not
//    an acceptable message anywhere in MonoSect.
//  * No error is ever swallowed: every fallback has to be reported through the
//    plan or through a warning list.
//  * `Result<T>` is used instead of exceptions for expected failures.  FFmpeg is
//    an external process and its failures are normal, not exceptional.
#pragma once

#include <optional>
#include <string>
#include <vector>
#include <utility>
#include <variant>

namespace monosect::core {

enum class ErrorCode {
    None = 0,

    // Input / dependencies
    FileNotFound,
    FileNotReadable,
    FileAccessDenied,
    FileInUse,
    PathTooLong,
    UnsupportedInputFormat,
    ProbeFailed,
    ProbeBinaryMissing,
    FfmpegBinaryMissing,
    DependencyVersionMismatch,
    DependencyChecksumMismatch,
    DependencyDownloadFailed,

    // Constraints
    ConstraintInvalid,
    ConstraintUnsatisfiable,
    SizeLimitTooSmall,

    // Planning
    PlanningFailed,
    PlanFailed,
    NoBoundaryAvailable,
    PrecisionNotAchievable,
    /// A part would have to start between keyframes, which stream copying cannot
    /// do without re-encoding.  Callers either re-encode the boundary or move the
    /// boundary to a keyframe; the error says which streams are affected.
    SmartCutUnavailable,

    // Execution
    ProcessLaunchFailed,
    ProcessFailed,
    ProcessCanceled,
    DiskSpaceInsufficient,
    OutputWriteFailed,
    OutputCollisionUnresolved,

    // Verification
    VerificationFailed,
    SizeLimitExceeded,
    DurationLimitExceeded,
    StreamMismatch,
    OutputUnreadable,
    DecodeFailed,
    CoverageGap,

    // Housekeeping
    InvalidState,
    Cancelled,
    JobNotFound,
    DatabaseError,
    UnsupportedOperation,
    Internal,
};

/// Returns a stable, lower-case identifier for logging and for the job database.
[[nodiscard]] const char* errorCodeName(ErrorCode code) noexcept;

/// Returns true when the failure is caused by something outside the app (a bad
/// file, a missing tool) as opposed to a bug in the app.
[[nodiscard]] bool isEnvironmentError(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;   // one sentence, shown to the user
    std::string detail;    // values, paths, exit codes - shown in the log
    std::string remedy;    // what the user can do about it

    Error() = default;
    Error(ErrorCode code, std::string message) : code(code), message(std::move(message)) {}
    Error(ErrorCode code, std::string message, std::string detail, std::string remedy = {})
        : code(code), message(std::move(message)), detail(std::move(detail)), remedy(std::move(remedy)) {}

    [[nodiscard]] bool empty() const noexcept { return code == ErrorCode::None; }

    /// "message (detail) - remedy" for single-line logs.
    [[nodiscard]] std::string toString() const;
};

/// A value or an error.  `value()` must only be called after `has_value()`.
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<0>(data_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(data_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(data_); }

    [[nodiscard]] T value_or(T fallback) const {
        return has_value() ? std::get<0>(data_) : std::move(fallback);
    }

    template <typename U>
    [[nodiscard]] T value_or(U fallback) const {
        return has_value() ? std::get<0>(data_) : static_cast<T>(fallback);
    }

private:
    std::variant<T, Error> data_;
};

/// Success/failure without a value.
struct Status {
    Error error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    static Status success() { return Status{}; }
    static Status failure(Error e) { return Status{std::move(e)}; }
};

/// Joins the last lines of a tool's output into one single-line detail string.
[[nodiscard]] std::string joinForDisplay(const std::vector<std::string>& lines, std::size_t maxLength = 600);

[[nodiscard]] inline Status makeError(ErrorCode code, std::string message, std::string detail = {},
                                      std::string remedy = {}) {
    return Status::failure(Error{code, std::move(message), std::move(detail), std::move(remedy)});
}

/// Helper used by tests and callers to build a failed Result<T>.
template <typename T>
[[nodiscard]] Result<T> failure(Error error) {
    return Result<T>(std::move(error));
}

} // namespace monosect::core
