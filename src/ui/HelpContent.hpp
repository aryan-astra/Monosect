// MonoSect — the help system (TODO 275 - TODO 283).
//
// Every concept the UI shows has one explanation, defined here, and the UI shows
// that text next to a question mark.  Keeping the text in one place matters for
// more than tidiness: the words used in the help are the words the code is
// allowed to claim, and TODO 045 of the product contract forbids saying
// "lossless" where a re-encode happened or "exact" where a keyframe was used.  A
// test checks the claims against the guarantees the engine actually makes
// (tests/core/UiModelTests.cpp).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace monosect::core {

enum class HelpTopicId {
    SizeLimit,
    DurationLimit,
    Precision,
    PreservationModes,
    StreamCopy,
    KeyframeAligned,
    SmartCut,
    FullReEncode,
    Verification,
    Boundaries,
    OutputNaming,
    JobHistory,
    ResourceModes,
    GpuEncoders,
    Merge,
    FormatSupport,
    WatchFolder,
    CancellationAndPause,
    CloudSync,
    FirstRun,
};

struct HelpTopic {
    HelpTopicId id;
    std::string idText;        // stable text id, used by the UI and by tests
    std::string title;
    std::string oneLine;       // shown as a tooltip next to a control
    std::string body;          // the full explanation, several paragraphs separated by '\n'
    std::string example;       // a concrete example or the exact wording of a limit
    std::vector<std::string> seeAlso;   // ids of related topics
};

class HelpContent {
public:
    [[nodiscard]] static const std::vector<HelpTopic>& topics();
    [[nodiscard]] static const HelpTopic* find(HelpTopicId id);
    [[nodiscard]] static const HelpTopic* findByText(std::string_view idText);
    /// Topics whose title, one-line summary or body contains `query`
    /// (case-insensitive).  An empty query returns every topic.
    [[nodiscard]] static std::vector<const HelpTopic*> search(std::string_view query);
};

} // namespace monosect::core
