#include "ui/HelpContent.hpp"

#include <algorithm>
#include <cctype>

namespace monosect::core {
namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const std::vector<HelpTopic>& topicList() {
    static const std::vector<HelpTopic> topics = {
        {
            HelpTopicId::SizeLimit,
            "size-limit",
            "Maximum size per part",
            "A maximum size is a hard limit in exact bytes: no part is ever published above it, and every published "
            "part is measured after it is written.",
            "MonoSect stores the limit as an integer number of bytes. \"2 GiB\" is 2,147,483,648 bytes; \"2 GB\" is "
            "2,000,000,000 bytes. The two are deliberately different, and the limit field always shows the exact "
            "byte count next to the friendly number so there is nothing to guess.\n"
            "A part that would exceed the limit is not written at all: the planner stops before the packet that would "
            "cross it, writes the part, measures the written file, and only then reports it as finished. If the "
            "measurement is over the limit the part is discarded and rewritten smaller - never published.\n"
            "Because the limit is a maximum, a part can be smaller than it. MonoSect then spends extra work (the "
            "\"closeness\" passes) to place the boundary at the largest position that still fits.",
            "Limit: 2 GiB  =  2,147,483,648 bytes.  A part of 2,146,900,224 bytes is accepted; 2,147,483,649 bytes is "
            "not.",
            {"precision", "verification"},
        },
        {
            HelpTopicId::DurationLimit,
            "duration-limit",
            "Maximum duration per part",
            "A maximum duration is a hard limit: a part is never longer than the limit, and the measured duration is "
            "checked after every write.",
            "Durations are kept as exact fractions of seconds (for example 120000/1001), never as floating point, so "
            "\"2 minutes\" and \"2:00\" mean exactly 120 s and nothing drifts over thousands of boundaries.\n"
            "When the boundary can fall on a keyframe, the part ends exactly where asked. When the material forces a "
            "keyframe-aligned cut, the part gets shorter and MonoSect says by how much: the \"requested\" and "
            "\"actual\" boundary of every part are both shown.\n"
            "The duration that is verified is the duration of the finished file, measured with ffprobe - not the "
            "duration the planner intended.",
            "Limit: 0:02.000. Keyframes at 0:00, 0:02.100 -> part 1 is 0:02.100 long only if that still fits the "
            "limit; otherwise it ends at 0:02.000 or earlier and the difference is reported.",
            {"size-limit", "boundaries", "verification"},
        },
        {
            HelpTopicId::Precision,
            "precision",
            "Precision policy",
            "How much extra work MonoSect may spend to get closer to the limit without ever crossing it.",
            "Fast: plan from the source index, write every part once. The prediction comes from the packet bytes in "
            "the index plus a container-overhead model; the hard limit still holds because every written part is "
            "measured and checked.\n"
            "Balanced (default): after the first part is written, its real size replaces the estimate for the rest of "
            "the job. One correction pass at most per part.\n"
            "Maximum: adds a short calibration render and searches between candidate boundaries so that every part is "
            "as large as it can safely be. It uses more CPU and disk time - which is the trade the product contract "
            "asks for when the user wants closeness.",
            "Fast writes 5 parts in one pass; Maximum may write a 2-second calibration clip first and then place each "
            "boundary one keyframe later if the measured overhead leaves room.",
            {"size-limit", "preservation-modes"},
        },
        {
            HelpTopicId::PreservationModes,
            "preservation-modes",
            "What is preserved",
            "Four modes, each with a different promise about the encoded media. The mode is chosen explicitly and is "
            "never changed silently.",
            "Stream copy and keyframe-aligned copy never re-encode: every packet of the source that falls inside a "
            "part is copied bit for bit.\n"
            "Smart cut copies the body of each part and re-encodes only the short region at a boundary where a "
            "keyframe is missing. The re-encoded region is reported per part, so \"what was re-encoded\" is never a "
            "mystery.\n"
            "Full re-encode decodes and encodes everything; it is the only mode that can guarantee a byte budget on "
            "material whose codec does not allow small enough keyframe steps, and it says so before starting.",
            "A 10 s H.264 file cut at 0:03 with keyframes every 2 s: keyframe-aligned mode produces a part ending at "
            "0:02 (copied bit for bit), smart cut produces one ending at 0:03.000 with 1 s re-encoded.",
            {"stream-copy", "keyframe-aligned", "smart-cut", "full-reencode"},
        },
        {
            HelpTopicId::StreamCopy,
            "stream-copy",
            "Stream copy",
            "The bytes of the encoded streams are moved, not decoded and re-encoded. Nothing about the picture or "
            "sound changes.",
            "Stream copy is exact at the packet level: the encoded data of a part is a byte-identical subset of the "
            "source's packets. It cannot cut between two packets, so the boundary is the last keyframe at or before "
            "the requested position.\n"
            "Container-level information is written fresh for each part: it has to be, because a part is a complete "
            "file with its own index. That is why a copied part's byte-for-byte size is not a slice of the source "
            "file, even though all of its media packets are.",
            "Two files that differ only in metadata: the video packets inside them are identical because they were "
            "copied, not encoded.",
            {"preservation-modes", "keyframe-aligned"},
        },
        {
            HelpTopicId::KeyframeAligned,
            "keyframe-aligned",
            "Keyframe-aligned copy",
            "Every part is written as a normal seekable file and its boundaries are the source's own keyframes.",
            "A video frame can only be decoded from the last keyframe before it, so a copy that started anywhere else "
            "would produce a file whose beginning is undecodable. MonoSect therefore places every boundary on an "
            "existing keyframe and reports the distance from the position the limit alone would have chosen.\n"
            "The requested and actual time of every boundary are both shown, with the difference in milliseconds, so "
            "\"as close as possible\" is measurable instead of a claim.",
            "Requested boundary 0:04.512, nearest keyframe 0:04.000 -> a delta of 0:00.512 is reported, not hidden.",
            {"stream-copy", "boundaries", "smart-cut"},
        },
        {
            HelpTopicId::SmartCut,
            "smart-cut",
            "Smart cut",
            "The body of a part is copied; only the short piece between the last keyframe and the requested boundary "
            "is re-encoded.",
            "Smart cut is the answer to \"I want the cut exactly here, and I do not want to re-encode an hour of "
            "video to get it\". The re-encoded region starts at the previous keyframe and ends at the requested "
            "position, which is typically well under two seconds.\n"
            "It is not free and it is not lossless: that region is decoded and encoded again, which changes the "
            "pixels in it slightly. MonoSect reports the length of every re-encoded region and the encoder settings "
            "used, so the cost is visible before the job starts.",
            "A 2 s GOP with a cut requested at 0:05.400 could re-encode 0:04.000 - 0:05.400 and copy the remaining "
            "eight hours.",
            {"full-reencode", "keyframe-aligned", "preservation-modes"},
        },
        {
            HelpTopicId::FullReEncode,
            "full-reencode",
            "Full re-encode",
            "Every stream is decoded and encoded again. Nothing of the original encoded data survives.",
            "This mode exists for two cases: the target container cannot hold the source's codecs, or the source has "
            "no usable keyframes and a byte budget must nevertheless be honoured. It is the slowest and the only "
            "lossy mode, so MonoSect never selects it automatically: it must be chosen, and the estimate of what it "
            "costs (quality and time) is shown first.",
            "H.265 copied into an MP4 container that rejects it, or a byte budget on a stream with a single keyframe "
            "at the start.",
            {"smart-cut", "preservation-modes"},
        },
        {
            HelpTopicId::Verification,
            "verification",
            "Verification",
            "\"Verified\" is a statement about checks that ran, not about a file that merely opened.",
            "Every part is verified after it is written and before it is published: the file exists and is readable, "
            "its stream signatures match the source (codec, profile, level, pixel format, dimensions, sample rate, "
            "channels, time base), its measured size is inside the hard limit, its measured duration is inside the "
            "duration limit, it starts on a keyframe when it is a copy, and the packet counts per stream equal the "
            "plan.\n"
            "Two optional checks go further: \"decode check\" decodes every part to prove it plays, and the hash "
            "check compares the copied packets with the source byte for byte.\n"
            "A part that fails any check is deleted and the job stops. It is never published and never recorded as "
            "completed.",
            "Part 3 measured 2,147,100,000 bytes against a 2,147,483,648 byte limit and its signatures matched: "
            "verified. Part 4 measured 2,147,600,000 bytes: discarded, rewritten, and only the smaller rewrite was "
            "published.",
            {"boundaries", "size-limit", "job-history"},
        },
        {
            HelpTopicId::Boundaries,
            "boundaries",
            "Requested and actual boundaries",
            "Every boundary is reported twice: where the limit asked for it, and where it really happened.",
            "A plan contains, for each part, the requested time (derived from the size or duration limit), the actual "
            "time of the first packet, the difference, and a sentence explaining what moved it: a keyframe, a hard "
            "limit, the end of the file, or a re-encoded region that made an exact time possible.\n"
            "The plan also shows whether the boundary is exact (the actual time equals the requested time) - and that "
            "word is only used when it is true.",
            "part 2: requested 0:04.512, actual 0:04.000, delta -0:00.512, reason: the next frame would not fit the "
            "size limit.",
            {"keyframe-aligned", "verification"},
        },
        {
            HelpTopicId::OutputNaming,
            "output-naming",
            "Output names",
            "Parts are named from a pattern; an existing file is never overwritten by accident.",
            "The default pattern is \"{source name} - part {number}\" and the extension follows the container. The "
            "pattern understands {name}, {index}, {index:03}, {start}, {end}, {duration}, {bytes} and {method}.\n"
            "If a name is already taken, MonoSect appends a short token and says what it did instead of replacing the "
            "file. Characters Windows forbids in a name (\\ / : * ? \" < > |) are replaced with an underscore, "
            "device names such as CON or LPT1 are escaped, and trailing dots and spaces are removed - on every "
            "platform, so a folder copied between machines keeps working.",
            "\"holiday - part 001.mp4\"; if that exists: \"holiday - part 001 (2).mp4\".",
            {"job-history"},
        },
        {
            HelpTopicId::JobHistory,
            "job-history",
            "Job history",
            "Every job is remembered locally, with the plan, the measured sizes, the boundary times and the "
            "verification result.",
            "The history is a local file (one JSON object per line) in the application data folder, so it works with "
            "no account, no network and no service. A record is written for completed jobs, for failed jobs and for "
            "cancelled jobs, because \"what did I already do\" is exactly the question a history has to answer.\n"
            "Nothing about the media itself is stored: names, sizes, times, hashes and the plan - never the video and "
            "never a copy of it.",
            "A record: source, constraint \"at most 2.00 GiB (2,147,483,648 bytes)\", five parts with their measured "
            "sizes, verified: true, ffmpeg version.",
            {"verification", "cloud-sync"},
        },
        {
            HelpTopicId::ResourceModes,
            "resource-modes",
            "Resource modes",
            "How much of the machine a job may use. The chosen mode sets thread counts, process priority and how many "
            "parts are written at once.",
            "Conservative keeps the machine usable: fewer encoder threads and a lower process priority.\n"
            "Balanced is the default: most cores, background priority, one part at a time.\n"
            "Maximum uses everything that can help and raises priority. It is still one part at a time by default, "
            "because writing several parts at once only helps on storage where the writes do not compete - and "
            "MonoSect says which case it detected instead of guessing in the UI.\n"
            "The panel shows what was derived and why, and a metric that cannot be measured shows as \"n/a\" rather "
            "than as a zero.",
            "8 logical / 4 physical cores: Balanced picks 4 encoder threads, Maximum picks 8 and raises priority.",
            {"gpu-encoders", "verification"},
        },
        {
            HelpTopicId::GpuEncoders,
            "gpu-encoders",
            "GPU encoders",
            "An encoder that exists is not automatically an encoder that works. MonoSect tests it before offering it.",
            "The list of encoders comes from ffmpeg's own list, but a driver can list an encoder whose hardware or "
            "driver version cannot run it. MonoSect therefore runs a one-frame encode with the encoder and only "
            "offers it when that succeeds, showing the reason when it does not.\n"
            "GPU encoding is only used where re-encoding happens (smart cut or full re-encode). A pure copy job is "
            "limited by storage, not by the encoder.",
            "h264_nvenc present but the probe failed: \"listed by ffmpeg, but the driver refused a test encode\".",
            {"resource-modes"},
        },
        {
            HelpTopicId::Merge,
            "merge",
            "Merging parts",
            "Merging joins files that carry the same streams, by copying - and refuses anything else instead of "
            "producing a file that breaks halfway through.",
            "Before writing, every input is probed and the streams are compared (codec, profile, level, pixel format, "
            "dimensions, sample rate, channels, time base, display rotation). A difference is reported in words and "
            "the merge stops; nothing is written.\n"
            "After writing, the result is verified against the inputs: the same streams, the same number of packets, "
            "every packet byte-identical (the first packet after a junction may carry the next input's parameter sets "
            "in band, which is redundant data the container already holds), and the decoded video frames identical in "
            "order.",
            "Merging five parts of a split reproduces the source's 250 decoded frames exactly; the container is "
            "0.06 s longer because the last packet of every part ends one audio frame later.",
            {"verification", "preservation-modes"},
        },
        {
            HelpTopicId::FormatSupport,
            "formats",
            "Format support",
            "The capability matrix lists what has been tested for each container - not what FFmpeg can probably do.",
            "Each container row says whether it can be read, stream-copied into, cut exactly, smart-cut and merged, "
            "and each cell carries the evidence: Verified means the automated fixture suite covers it; Supported "
            "means it was validated by hand; Caveats means it works with a documented restriction; Unsupported means "
            "the planner refuses it with an explanation.",
            "MKV: read, copy, exact cut, merge - verified. MP4 with subtitles: refused, because MP4 cannot carry "
            "SubRip; MKV can.",
            {"preservation-modes"},
        },
        {
            HelpTopicId::WatchFolder,
            "watch-folder",
            "Watch folder",
            "A folder MonoSect watches for new files and processes with a saved set of constraints.",
            "A file is only picked up when it is *finished*, not merely when it is stable: a size that stops growing "
            "may just be a slow encoder, so MonoSect additionally requires the file to be exclusive-openable and "
            "unchanged across two checks separated by a settling interval. The interval is configurable and shown.\n"
            "The watcher is notified by the operating system and survives notification buffer overflows by "
            "re-scanning, so a burst of copies into the folder cannot lose a file.",
            "\"Downloads\" watched for *.mkv with a 30 s settling time: a 4 GB copy triggers only when the copying "
            "program has closed it.",
            {"job-history"},
        },
        {
            HelpTopicId::CancellationAndPause,
            "cancellation",
            "Cancelling and pausing",
            "Cancel stops the work and never publishes a partial file. Pause stops after the current part, not in the "
            "middle of one.",
            "Cancelling terminates the running FFmpeg process, waits for it, discards the unfinished part and records "
            "the job as cancelled. Parts that were already verified and published stay - that is what makes a "
            "cancelled job resumable - and the record says exactly which parts exist.\n"
            "Pause is a segment-boundary pause by design. Suspending a process mid-write on Windows can leave a "
            "muxer with a half-written index and a file that looks complete; MonoSect does not do that. Pressing "
            "pause lets the current part finish and verify, and then waits. The UI says which of the two is "
            "happening: \"finishing part 3, then paused\".",
            "Pause pressed during part 3 of 5: part 3 is written and verified, parts 4 and 5 wait, and the status "
            "line says the job is paused after part 3.",
            {"verification", "resource-modes"},
        },
        {
            HelpTopicId::CloudSync,
            "cloud-sync",
            "Cloud synchronisation (not enabled)",
            "The history is local. Synchronisation is designed for metadata only and is not built yet.",
            "Two facts decide the design: a machine cannot be recognised as \"the same person\" after a reinstall "
            "without a persistent secret (an account, a recovery key or a synced credential), and a media tool that "
            "uploads file names by accident is a privacy problem.\n"
            "So: nothing leaves the machine today. When synchronisation is enabled it will carry metadata only - "
            "never media, never file contents - it will be opt-in, and it will be built on an explicit recovery key "
            "the user can print, not on a hardware fingerprint.",
            "The history file stays in %LOCALAPPDATA%\\MonoSect\\jobs.jsonl until the user turns synchronisation on.",
            {"job-history"},
        },
        {
            HelpTopicId::FirstRun,
            "first-run",
            "How to split a file",
            "Choose a file, set what the parts must not exceed, look at the plan, then start it.",
            "1. Drop a file on the window (or choose one). MonoSect probes it and shows the streams, the duration and "
            "how reliable that duration is.\n"
            "2. Set the limit: a maximum size per part, a maximum duration per part, or both. The limit field shows "
            "the exact byte count or the exact seconds it will use.\n"
            "3. Look at the plan: number of parts, predicted size and duration of each, and the requested vs actual "
            "boundary of every cut. Nothing has been written yet.\n"
            "4. Start. Parts are written one at a time, verified, then published with a progress line that says which "
            "part is being written and what it has done so far.",
            "A 40 minute 1080p recording with a 2 GiB limit: 3 parts, boundaries on keyframes, every part measured "
            "after writing.",
            {"size-limit", "duration-limit", "verification", "boundaries"},
        },
    };
    return topics;
}

} // namespace

const std::vector<HelpTopic>& HelpContent::topics() {
    return topicList();
}

const HelpTopic* HelpContent::find(HelpTopicId id) {
    for (const HelpTopic& topic : topicList()) {
        if (topic.id == id) {
            return &topic;
        }
    }
    return nullptr;
}

const HelpTopic* HelpContent::findByText(std::string_view idText) {
    for (const HelpTopic& topic : topicList()) {
        if (topic.idText == idText) {
            return &topic;
        }
    }
    return nullptr;
}

std::vector<const HelpTopic*> HelpContent::search(std::string_view query) {
    std::vector<const HelpTopic*> matches;
    const std::string needle = lowered(query);
    for (const HelpTopic& topic : topicList()) {
        if (needle.empty()) {
            matches.push_back(&topic);
            continue;
        }
        const std::string haystack = lowered(topic.title + "\n" + topic.oneLine + "\n" + topic.body + "\n" +
                                            topic.example + "\n" + topic.idText);
        if (haystack.find(needle) != std::string::npos) {
            matches.push_back(&topic);
        }
    }
    return matches;
}

} // namespace monosect::core
