#include "TestSupport.hpp"
#include "engine/JobStore.hpp"
#include "engine/OutputCommitter.hpp"

#include <fstream>

using namespace monosect::core;

MONOSECT_TEST(long_temporary_paths_can_be_verified_published_and_cleaned) {
    const auto root = std::filesystem::temp_directory_path() / makeJobId();
    const auto output = root / std::string(110, 'd');
    OutputCommitter committer(output, "long-path", false);
    CHECK(committer.begin().ok());
    const auto temporary = committer.temporaryPathFor(std::string(150, 'p') + ".mp4");
    {
        std::ofstream stream(temporary, std::ios::binary);
        stream << "verified bytes";
        CHECK(stream.good());
    }
    CHECK(committer.fileSize(temporary).has_value());
    CHECK(committer.flushToDisk(temporary).ok());
    const auto final = output / "published.mp4";
    CHECK(committer.commit(temporary, final).ok());
    {
        std::ofstream(committer.temporaryPathFor("leftover.mp4")) << "leftover";
    }
    committer.cleanUp();
    CHECK(!std::filesystem::exists(committer.jobDirectory()));
    CHECK(std::filesystem::exists(final));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}
