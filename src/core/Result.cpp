#include "core/Result.hpp"

namespace monosect::core {

std::string joinForDisplay(const std::vector<std::string>& lines, std::size_t maxLength) {
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) {
            text += " | ";
        }
        text += line;
        if (text.size() > maxLength) {
            break;
        }
    }
    return text;
}

const char* errorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None: return "none";
        case ErrorCode::FileNotFound: return "file_not_found";
        case ErrorCode::FileNotReadable: return "file_not_readable";
        case ErrorCode::FileAccessDenied: return "file_access_denied";
        case ErrorCode::FileInUse: return "file_in_use";
        case ErrorCode::PathTooLong: return "path_too_long";
        case ErrorCode::UnsupportedInputFormat: return "unsupported_input_format";
        case ErrorCode::ProbeFailed: return "probe_failed";
        case ErrorCode::ProbeBinaryMissing: return "probe_binary_missing";
        case ErrorCode::FfmpegBinaryMissing: return "ffmpeg_binary_missing";
        case ErrorCode::DependencyVersionMismatch: return "dependency_version_mismatch";
        case ErrorCode::DependencyChecksumMismatch: return "dependency_checksum_mismatch";
        case ErrorCode::DependencyDownloadFailed: return "dependency_download_failed";
        case ErrorCode::ConstraintInvalid: return "constraint_invalid";
        case ErrorCode::ConstraintUnsatisfiable: return "constraint_unsatisfiable";
        case ErrorCode::SizeLimitTooSmall: return "size_limit_too_small";
        case ErrorCode::PlanFailed:
        return "plan-failed";
    case ErrorCode::SmartCutUnavailable:
        return "smart-cut-unavailable";
    case ErrorCode::PlanningFailed: return "planning_failed";
        case ErrorCode::NoBoundaryAvailable: return "no_boundary_available";
        case ErrorCode::PrecisionNotAchievable: return "precision_not_achievable";
        case ErrorCode::ProcessLaunchFailed: return "process_launch_failed";
        case ErrorCode::ProcessFailed: return "process_failed";
        case ErrorCode::ProcessCanceled: return "process_canceled";
        case ErrorCode::DiskSpaceInsufficient: return "disk_space_insufficient";
        case ErrorCode::OutputWriteFailed: return "output_write_failed";
        case ErrorCode::OutputCollisionUnresolved: return "output_collision_unresolved";
        case ErrorCode::VerificationFailed: return "verification_failed";
        case ErrorCode::SizeLimitExceeded: return "size_limit_exceeded";
        case ErrorCode::DurationLimitExceeded: return "duration_limit_exceeded";
        case ErrorCode::StreamMismatch: return "stream_mismatch";
        case ErrorCode::OutputUnreadable: return "output_unreadable";
        case ErrorCode::DecodeFailed: return "decode_failed";
        case ErrorCode::CoverageGap: return "coverage_gap";
        case ErrorCode::InvalidState: return "invalid_state";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::JobNotFound: return "job_not_found";
        case ErrorCode::DatabaseError: return "database_error";
        case ErrorCode::UnsupportedOperation: return "unsupported_operation";
        case ErrorCode::Internal: return "internal";
    }
    return "unknown";
}

bool isEnvironmentError(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::FileNotFound:
        case ErrorCode::FileNotReadable:
        case ErrorCode::FileAccessDenied:
        case ErrorCode::FileInUse:
        case ErrorCode::PathTooLong:
        case ErrorCode::UnsupportedInputFormat:
        case ErrorCode::ProbeFailed:
        case ErrorCode::ProbeBinaryMissing:
        case ErrorCode::FfmpegBinaryMissing:
        case ErrorCode::DependencyVersionMismatch:
        case ErrorCode::DependencyChecksumMismatch:
        case ErrorCode::DependencyDownloadFailed:
        case ErrorCode::DiskSpaceInsufficient:
        case ErrorCode::OutputWriteFailed:
        case ErrorCode::OutputCollisionUnresolved:
        case ErrorCode::SizeLimitTooSmall:
        case ErrorCode::ConstraintInvalid:
        case ErrorCode::ConstraintUnsatisfiable:
            return true;
        default:
            return false;
    }
}

std::string Error::toString() const {
    std::string text = message;
    if (!detail.empty()) {
        text += " (";
        text += detail;
        text += ")";
    }
    if (!remedy.empty()) {
        text += " - ";
        text += remedy;
    }
    return text;
}

} // namespace monosect::core
