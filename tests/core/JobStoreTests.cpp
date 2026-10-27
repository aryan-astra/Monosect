// MonoSect — the job database: append-only, metadata only, robust to damage.
#include "TestSupport.hpp"

#include "engine/JobStore.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>

using namespace monosect::core;
using namespace monosect::test;

namespace {

std::filesystem::path temporaryDatabase(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

JobRecord makeRecord(const std::string& id, const std::string& state, int parts) {
    JobRecord record;
    record.jobId = id;
    record.startedAt = "2026-01-01T00:00:00";
    record.finishedAt = "2026-01-01T00:00:10";
    record.finalState = state;
    record.message = "test";
    record.sourcePath = "C:\\media\\a file.mp4";
    record.sourceBytes = 1234567;
    record.sourceDuration = Rational{10, 1};
    record.outputDirectory = "C:\\out";
    record.constraintDescription = "at most 2.00 GiB";
    for (int i = 1; i <= parts; ++i) {
        SegmentOutcome part;
        part.index = i;
        part.path = "C:\\out\\part " + std::to_string(i) + ".mp4";
        part.fileSizeBytes = 1000 * i;
        part.duration = Rational{2, 1};
        part.verified = true;
        part.committed = true;
        record.parts.push_back(part);
    }
    return record;
}

} // namespace

MONOSECT_TEST(records_round_trip_and_stay_metadata_only) {
    const std::filesystem::path path = temporaryDatabase("monosect-jobs-1.jsonl");
    JobStore store(path);
    CHECK(store.append(makeRecord("job-a", "completed", 3)).ok());
    CHECK(store.append(makeRecord("job-b", "failed", 0)).ok());

    auto loaded = store.loadRecent(10);
    CHECK(loaded.has_value());
    CHECK_EQ(2u, loaded.value().size());
    CHECK_EQ(std::string("job-b"), loaded.value().front().jobId);   // newest first
    CHECK_EQ(std::string("completed"), loaded.value().back().finalState);
    CHECK_EQ(3u, loaded.value().back().parts.size());
    CHECK_EQ(ByteCount{3000}, loaded.value().back().parts.back().fileSizeBytes);

    // The file must contain no media bytes: it is a text log, one object per line.
    std::string contents;
    {
        std::ifstream stream(path, std::ios::binary);
        contents.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }   // the handle is closed before the delete below, which Windows requires
    CHECK(contents.find("job-a") != std::string::npos);
    CHECK(contents.find("\\x00") == std::string::npos);
    std::filesystem::remove(path);
}

MONOSECT_TEST(a_damaged_line_is_reported_not_hidden) {
    const std::filesystem::path path = temporaryDatabase("monosect-jobs-2.jsonl");
    {
        JobStore store(path);
        CHECK(store.append(makeRecord("good", "completed", 1)).ok());
    }
    {
        std::ofstream append(path, std::ios::app);
        append << "{ this is not json\n";
    }
    {
        JobStore store(path);
        CHECK(store.append(makeRecord("also-good", "completed", 1)).ok());
        auto loaded = store.loadRecent(10);
        CHECK(loaded.has_value());
        CHECK_EQ(2u, loaded.value().size());
        CHECK_EQ(1, store.damagedRecordCount());
    }
    std::filesystem::remove(path);
}

MONOSECT_TEST(an_unwritable_database_reports_an_error_instead_of_crashing) {
    const auto blocker = temporaryDatabase(("monosect-blocker-" + makeJobId()).c_str());
    std::ofstream(blocker).put('x');
    JobStore store(blocker / "jobs.jsonl");
    const Status status = store.append(makeRecord("x", "completed", 1));
    CHECK(!status.ok());
    CHECK(!status.error.message.empty());
    std::filesystem::remove(blocker);
}

#if defined(_WIN32)
MONOSECT_TEST(default_history_preserves_unicode_profile_paths) {
    const auto root = std::filesystem::temp_directory_path() / makeJobId() /
                      std::filesystem::path(L"\u65e5\u672c\u8a9e");
    const wchar_t* previous = ::_wgetenv(L"LOCALAPPDATA");
    const std::wstring saved = previous != nullptr ? previous : L"";
    CHECK_EQ(0, ::_wputenv_s(L"LOCALAPPDATA", root.c_str()));
    const auto store = JobStore::defaultStore();
    CHECK_EQ(0, ::_wputenv_s(L"LOCALAPPDATA", saved.c_str()));
    CHECK(store.path() == root / "MonoSect" / "jobs.jsonl");
    CHECK(store.append(makeRecord("unicode-profile", "completed", 1)).ok());
    const auto loaded = store.loadRecent(1);
    CHECK(loaded.has_value());
    CHECK_EQ(1u, loaded.value().size());
    std::error_code error;
    std::filesystem::remove_all(root.parent_path(), error);
    CHECK(!error);
}
#endif
