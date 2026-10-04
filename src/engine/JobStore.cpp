#include "engine/JobStore.hpp"

#include "core/Json.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <system_error>

namespace monosect::core {
namespace {

JsonValue rationalJson(const Rational& value) {
    JsonValue object = JsonValue::makeObject();
    object.set("num", JsonValue::makeInt(value.numerator));
    object.set("den", JsonValue::makeInt(value.denominator));
    return object;
}

Rational rationalFromJson(const JsonValue& value) {
    if (!value.isObject()) {
        return Rational{0, 1};
    }
    const auto numerator = value.find("num");
    const auto denominator = value.find("den");
    if (numerator == nullptr || denominator == nullptr) {
        return Rational{0, 1};
    }
    return Rational{numerator->asInt64().value_or(0), denominator->asInt64().value_or(1)};
}

std::filesystem::path applicationDataDirectory() {
#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr) {
        return std::filesystem::path(localAppData) / "MonoSect";
    }
    return std::filesystem::temp_directory_path() / "MonoSect";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "monosect";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path(home) / ".local" / "share" / "monosect";
    }
    return std::filesystem::temp_directory_path() / "monosect";
#endif
}

} // namespace

std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::string makeJobId() {
    std::random_device device;
    std::mt19937_64 generator(device());
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%llx%llx", static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(generator() & 0xFFFFFFFFFFFFull));
    return buffer;
}

JobStore JobStore::defaultStore() {
    return JobStore(applicationDataDirectory() / "jobs.jsonl");
}

Status JobStore::append(const JobRecord& record) const {
    std::error_code error;
    std::filesystem::create_directories(file_.parent_path(), error);
    if (error) {
        return makeError(ErrorCode::DatabaseError, "The folder for the job history could not be created.",
                         file_.parent_path().string() + ": " + error.message(),
                         "Check that your user profile is writable.");
    }
    std::ofstream out(file_, std::ios::binary | std::ios::app);
    if (!out) {
        return makeError(ErrorCode::DatabaseError, "The job history could not be opened for writing.",
                         file_.string(), "Check that the file is not read-only.");
    }
    out << record.toJson() << '\n';
    out.flush();
    if (!out) {
        return makeError(ErrorCode::DatabaseError, "The job history could not be written.",
                         file_.string(), "Check that the disk has free space.");
    }
    return Status::success();
}

Result<std::vector<JobRecord>> JobStore::loadRecent(int limit) const {
    std::vector<JobRecord> records;
    std::error_code error;
    if (!std::filesystem::exists(file_, error)) {
        return records;  // no history yet is not an error
    }
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        return Error{ErrorCode::DatabaseError, "The job history could not be read.", file_.string(),
                     "Check that the file is readable."};
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = parseJson(line);
        if (!parsed.has_value() || !parsed.value().isObject()) {
            ++damagedRecords_;
            continue;   // a damaged line never hides the lines that are fine
        }
        records.push_back(JobRecord::fromJson(parsed.value()));
    }
    // Newest first, limited.
    std::reverse(records.begin(), records.end());
    if (limit > 0 && records.size() > static_cast<std::size_t>(limit)) {
        records.resize(static_cast<std::size_t>(limit));
    }
    return records;
}

Result<JobRecord> JobStore::find(const std::string& jobId) const {
    auto records = loadRecent(0);
    if (!records.has_value()) {
        return failure<JobRecord>(records.error());
    }
    for (const auto& record : records.value()) {
        if (record.jobId == jobId) {
            return record;
        }
    }
    return Error{ErrorCode::JobNotFound, "That job is not in the history.", "job id: " + jobId,
                 "It may have been recorded by another user account."};
}

std::string JobRecord::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("job_id", JsonValue::makeString(jobId));
    root.set("started_at", JsonValue::makeString(startedAt));
    root.set("finished_at", JsonValue::makeString(finishedAt));
    root.set("state", JsonValue::makeString(finalState));
    root.set("message", JsonValue::makeString(message));
    root.set("source_path", JsonValue::makeString(sourcePath));
    root.set("source_bytes", JsonValue::makeInt(sourceBytes));
    root.set("source_duration", rationalJson(sourceDuration));
    root.set("output_directory", JsonValue::makeString(outputDirectory));
    root.set("constraint", JsonValue::makeString(constraintDescription));
    root.set("mode", JsonValue::makeString(modeDescription));
    root.set("app_version", JsonValue::makeString(appVersion));
    root.set("ffmpeg_version", JsonValue::makeString(ffmpegVersion));
    root.set("ffprobe_version", JsonValue::makeString(ffprobeVersion));
    root.set("largest_part_bytes", JsonValue::makeInt(largestPartBytes));
    root.set("longest_part_duration", rationalJson(longestPartDuration));
    root.set("planning_seconds", JsonValue::makeDouble(planningSeconds));
    root.set("execution_seconds", JsonValue::makeDouble(executionSeconds));
    root.set("verification_seconds", JsonValue::makeDouble(verificationSeconds));
    root.set("parts_rewritten", JsonValue::makeInt(partsRewritten));
    root.set("plan", JsonValue::makeString(planJson));

    JsonValue partList = JsonValue::makeArray();
    for (const auto& part : parts) {
        auto parsed = parseJson(part.toJson());
        partList.push(parsed.has_value() ? parsed.value() : JsonValue::makeNull());
    }
    root.set("parts", std::move(partList));

    JsonValue warningList = JsonValue::makeArray();
    for (const std::string& warning : warnings) {
        warningList.push(JsonValue::makeString(warning));
    }
    root.set("warnings", std::move(warningList));
    return root.dump();
}

JobRecord JobRecord::fromJson(const JsonValue& value) {
    JobRecord record;
    record.jobId = value.stringOr("job_id");
    record.startedAt = value.stringOr("started_at");
    record.finishedAt = value.stringOr("finished_at");
    record.finalState = value.stringOr("state");
    record.message = value.stringOr("message");
    record.sourcePath = value.stringOr("source_path");
    record.outputDirectory = value.stringOr("output_directory");
    record.constraintDescription = value.stringOr("constraint");
    record.modeDescription = value.stringOr("mode");
    record.appVersion = value.stringOr("app_version");
    record.ffmpegVersion = value.stringOr("ffmpeg_version");
    record.ffprobeVersion = value.stringOr("ffprobe_version");
    record.planJson = value.stringOr("plan");
    if (const JsonValue* bytes = value.find("source_bytes"); bytes != nullptr) {
        record.sourceBytes = bytes->asInt64().value_or(0);
    }
    if (const JsonValue* duration = value.find("source_duration"); duration != nullptr) {
        record.sourceDuration = rationalFromJson(*duration);
    }
    if (const JsonValue* bytes = value.find("largest_part_bytes"); bytes != nullptr) {
        record.largestPartBytes = bytes->asInt64().value_or(0);
    }
    if (const JsonValue* duration = value.find("longest_part_duration"); duration != nullptr) {
        record.longestPartDuration = rationalFromJson(*duration);
    }
    if (const JsonValue* seconds = value.find("planning_seconds"); seconds != nullptr) {
        record.planningSeconds = seconds->asDouble().value_or(0.0);
    }
    if (const JsonValue* seconds = value.find("execution_seconds"); seconds != nullptr) {
        record.executionSeconds = seconds->asDouble().value_or(0.0);
    }
    if (const JsonValue* seconds = value.find("verification_seconds"); seconds != nullptr) {
        record.verificationSeconds = seconds->asDouble().value_or(0.0);
    }
    if (const JsonValue* count = value.find("parts_rewritten"); count != nullptr) {
        record.partsRewritten = static_cast<int>(count->asInt64().value_or(0));
    }
    if (const JsonValue* warnings = value.find("warnings"); warnings != nullptr && warnings->isArray()) {
        for (const JsonValue& item : warnings->items()) {
            if (auto text = item.asString(); text.has_value()) {
                record.warnings.push_back(*text);
            }
        }
    }
    if (const JsonValue* parts = value.find("parts"); parts != nullptr && parts->isArray()) {
        for (const JsonValue& item : parts->items()) {
            if (!item.isObject()) {
                continue;
            }
            SegmentOutcome part;
            part.index = static_cast<int>(item.find("index") != nullptr
                                              ? item.find("index")->asInt64().value_or(0)
                                              : 0);
            part.wrote = item.find("wrote") != nullptr && item.find("wrote")->asBool().value_or(false);
            part.path = item.stringOr("path");
            part.fileSizeBytes = item.find("bytes") != nullptr ? item.find("bytes")->asInt64().value_or(0) : 0;
            if (const JsonValue* duration = item.find("duration"); duration != nullptr) {
                part.duration = rationalFromJson(*duration);
            }
            if (const JsonValue* start = item.find("start"); start != nullptr) {
                part.startSeconds = rationalFromJson(*start);
            }
            if (const JsonValue* end = item.find("end"); end != nullptr) {
                part.endSeconds = rationalFromJson(*end);
            }
            part.verified = item.find("verified") != nullptr && item.find("verified")->asBool().value_or(false);
            part.committed = item.find("committed") != nullptr && item.find("committed")->asBool().value_or(false);
            part.verificationSummary = item.stringOr("verification");
            record.parts.push_back(std::move(part));
        }
    }
    return record;
}

} // namespace monosect::core
