#include "TestSupport.hpp"
#include "engine/FFmpegBootstrap.hpp"
#include "engine/JobStore.hpp"

MONOSECT_TEST(cancelled_bootstrap_never_downloads_or_creates_files) {
    const auto directory = std::filesystem::temp_directory_path() / monosect::core::makeJobId();
    std::stop_source stop;
    stop.request_stop();
    CHECK(!vsm::ensureFFmpeg(directory, {}, stop.get_token()));
    CHECK(!std::filesystem::exists(directory));
}
