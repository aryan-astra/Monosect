// MonoSect — verification against what is on disk.
//
// The engine measures a written part with ffprobe (and optionally decodes it),
// then hands the measurements to the pure rules in core/Verification.  Nothing is
// published before this step passed.
#pragma once

#include "core/Verification.hpp"
#include "engine/FFprobeService.hpp"
#include "engine/ProcessRunner.hpp"

#include <filesystem>
#include <string>

namespace monosect::core {

class VerificationEngine {
public:
    VerificationEngine(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath,
                       VerificationPolicy policy);

    /// Measures one file: size, duration, streams, first video packet, and
    /// optionally a full decode or a stream hash.
    [[nodiscard]] Result<SegmentMeasurement> measure(const std::filesystem::path& file,
                                                     const CancelCheck& shouldCancel = {}) const;

    /// Full verification of one written part against its plan.
    [[nodiscard]] Result<VerificationResult> verifyPart(const SplitPlan& plan, const SegmentPlan& segment,
                                                        const std::filesystem::path& file,
                                                        std::vector<SegmentMeasurement>* measurements,
                                                        const CancelCheck& shouldCancel = {}) const;

    /// After every part: coverage and total duration.
    [[nodiscard]] Result<VerificationResult> verifySequence(
        const SplitPlan& plan, const std::vector<SegmentMeasurement>& measurements) const;

    [[nodiscard]] const VerificationPolicy& policy() const noexcept { return policy_; }
    void setPolicy(VerificationPolicy policy) { policy_ = std::move(policy); }

private:
    [[nodiscard]] Result<bool> firstVideoPacketIsKeyframe(const std::filesystem::path& file,
                                                          const CancelCheck& shouldCancel) const;

    FFprobeService& prober_;
    IProcessRunner& runner_;
    std::string ffmpegPath_;
    VerificationPolicy policy_;
};

} // namespace monosect::core
