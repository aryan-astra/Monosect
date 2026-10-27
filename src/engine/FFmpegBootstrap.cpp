#include "engine/FFmpegBootstrap.hpp"

#include "core/DependencyPin.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <cstdio>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// winhttp.lib is linked via CMakeLists.txt

namespace vsm {

namespace {

bool containsUnsafeProcessContent(std::wstring_view value) {
    return value.find(L'\0') != std::wstring_view::npos ||
           value.find(L'\n') != std::wstring_view::npos ||
           value.find(L'\r') != std::wstring_view::npos;
}

// Splits the pinned https:// URL into the host and the path the downloader
// takes.  The URL is a compile-time constant so a malformed one is a build
// error in the release process, not something a user can influence.
bool splitPinnedUrl(const std::string& url, std::wstring& host, std::wstring& path) {
    const std::string prefix = "https://";
    if (url.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::size_t slash = url.find('/', prefix.size());
    if (slash == std::string::npos) {
        return false;
    }
    const std::string hostPart = url.substr(prefix.size(), slash - prefix.size());
    const std::string pathPart = url.substr(slash);
    if (hostPart.empty() || pathPart.size() < 2) {
        return false;
    }
    host.assign(hostPart.begin(), hostPart.end());
    path.assign(pathPart.begin(), pathPart.end());
    return true;
}

// Verifies the two tools against the pins.  `onStatus` is told what happened in
// the same words the verification uses, so the window and the job log cannot
// disagree about whether the tools were identified.
bool verifyToolchain(const std::filesystem::path& appDirectory,
                     const std::function<void(const std::string&)>& onStatus,
                     bool discardOnFailure) {
    const std::vector<std::pair<std::filesystem::path, monosect::core::DependencyPin>> tools = {
        {appDirectory / "ffmpeg.exe", monosect::core::pinnedFfmpeg()},
        {appDirectory / "ffprobe.exe", monosect::core::pinnedFfprobe()},
    };
    bool allOk = true;
    for (const auto& [toolPath, pin] : tools) {
        const monosect::core::DependencyCheck check = monosect::core::verifyToolFile(toolPath, pin);
        if (onStatus) {
            onStatus(pin.name + ": " + check.message);
        }
        if (!check.ok()) {
            allOk = false;
            if (discardOnFailure) {
                std::error_code error;
                std::filesystem::remove(toolPath, error);
                std::filesystem::remove(pin.sidecarPath(toolPath), error);
            }
        }
    }
    return allOk;
}

std::wstring toPowerShellSingleQuotedLiteral(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (const wchar_t ch : value) {
        if (ch == L'\'') {
            escaped += L"''";
        } else {
            escaped.push_back(ch);
        }
    }
    return L"'" + escaped + L"'";
}

// Download a URL to a local file using WinHTTP. Follows one redirect level.
bool downloadFile(const std::wstring& host, const std::wstring& path,
                  const std::filesystem::path& outputPath,
                  const std::function<void(const std::string&)>& onStatus, std::stop_token stop) {
    HINTERNET hSession = WinHttpOpen(L"Monosect/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 5000, 10000, 10000, 5000);

    auto cleanup = [&](HINTERNET h) { if (h) WinHttpCloseHandle(h); };

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { cleanup(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) { cleanup(hConnect); cleanup(hSession); return false; }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        cleanup(hRequest); cleanup(hConnect); cleanup(hSession);
        return false;
    }

    // Check for redirect (302/301)
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode >= 300 && statusCode < 400) {
        // Get redirect location
        DWORD locSize = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &locSize, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locSize > 0) {
            std::wstring location(locSize / sizeof(wchar_t), L'\0');
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                location.data(), &locSize, WINHTTP_NO_HEADER_INDEX);
            location.resize(locSize / sizeof(wchar_t));

            cleanup(hRequest); cleanup(hConnect);

            // Parse the redirect URL
            URL_COMPONENTS urlComp{};
            urlComp.dwStructSize = sizeof(urlComp);
            wchar_t hostBuf[256]{};
            wchar_t pathBuf[2048]{};
            urlComp.lpszHostName = hostBuf;
            urlComp.dwHostNameLength = 256;
            urlComp.lpszUrlPath = pathBuf;
            urlComp.dwUrlPathLength = 2048;

            if (!WinHttpCrackUrl(location.c_str(), 0, 0, &urlComp)) {
                cleanup(hSession);
                return false;
            }

            hConnect = WinHttpConnect(hSession, hostBuf, urlComp.nPort, 0);
            if (!hConnect) { cleanup(hSession); return false; }

            // Combine path + extra info
            std::wstring fullPath = pathBuf;
            if (urlComp.lpszExtraInfo && urlComp.dwExtraInfoLength > 0) {
                fullPath += urlComp.lpszExtraInfo;
            }

            hRequest = WinHttpOpenRequest(hConnect, L"GET", fullPath.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
            if (!hRequest) { cleanup(hConnect); cleanup(hSession); return false; }

            if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(hRequest, nullptr)) {
                cleanup(hRequest); cleanup(hConnect); cleanup(hSession);
                return false;
            }

            statusCode = 0;
            statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);
        }
    }

    if (statusCode != 200) {
        cleanup(hRequest); cleanup(hConnect); cleanup(hSession);
        return false;
    }

    // Stream body to file
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open()) {
        cleanup(hRequest); cleanup(hConnect); cleanup(hSession);
        return false;
    }

    DWORD totalRead = 0;
    DWORD available = 0;
    std::vector<char> buffer(65536);
    while (!stop.stop_requested() && WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        DWORD toRead = std::min(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (WinHttpReadData(hRequest, buffer.data(), toRead, &read) && read > 0) {
            out.write(buffer.data(), read);
            totalRead += read;
            if (onStatus && (totalRead % (1024 * 1024) < read)) {
                onStatus("Downloading FFmpeg... " + std::to_string(totalRead / (1024 * 1024)) + " MB");
            }
        } else {
            break;
        }
    }

    out.close();
    cleanup(hRequest); cleanup(hConnect); cleanup(hSession);
    return !stop.stop_requested() && totalRead > 0 && out.good();
}

// Extract ffmpeg.exe and ffprobe.exe from a verified zip.
//
// Why PowerShell: Windows has no documented API that reads a ZIP container.
// `cabapi.dll` decompresses XPRESS/LZMS, not the raw DEFLATE (RFC 1951) that a
// ZIP entry uses, and `RtlDecompressBuffer` offers the same formats.  Writing a
// native inflate would be a few hundred lines of new parsing code on the
// installation path - a much larger risk than the dependency it removes - so the
// documented, shipped implementation (`Expand-Archive`) is used.  The decision is
// recorded here so it is not re-litigated without new information.
//
// What is fixed relative to the previous implementation, and why it matters:
//   * the wait result is checked.  `WaitForSingleObject` returning WAIT_TIMEOUT
//     used to be discarded, `GetExitCodeProcess` then reported 259
//     (STILL_ACTIVE) and the caller concluded "failed" - while the PowerShell
//     child kept running, and its last statement deleted the archive.  On a slow
//     or large extraction that is a silently wedged, half-installed toolchain.
//     A timeout now terminates the child and waits for it to die.
//   * nothing is written into the destination directory.  Extraction happens in
//     a temporary folder that is verified *before* anything is moved into place,
//     so a failed verification can never have half-replaced a working toolchain.
//   * the command no longer passes `-ExecutionPolicy Bypass`.  Inline
//     `-Command` is not subject to the script execution policy, so the bypass was
//     both unnecessary and a suppression of a machine's policy.
bool extractFFmpegFromZip(const std::filesystem::path& zipPath,
                          const std::filesystem::path& destinationDir,
                          const std::function<void(const std::string&)>& onStatus,
                          std::string& failureReason, std::stop_token stop) {
    if (onStatus) onStatus("Extracting FFmpeg binaries...");

    const std::filesystem::path tempDir = destinationDir / "ffmpeg_extract_tmp";

    const std::wstring zipPathWide = zipPath.wstring();
    const std::wstring tempDirWide = tempDir.wstring();

    if (zipPathWide.empty() || tempDirWide.empty()) {
        failureReason = "Extraction aborted: the archive or working path is empty.";
        return false;
    }
    if (containsUnsafeProcessContent(zipPathWide) || containsUnsafeProcessContent(tempDirWide)) {
        failureReason = "Extraction aborted: a path contains a NUL or a line break.";
        return false;
    }

    std::error_code error;
    std::filesystem::remove_all(tempDir, error);
    error.clear();

    // Every path goes through the single-quoted PowerShell literal, which is the
    // only quoting form in which nothing in a Windows path is interpreted: an
    // apostrophe in a user name ("C:\Users\O'Brien\...") would otherwise end the
    // string, and a `$` or a backtick would be expanded.
    std::wstring command = L"powershell.exe -NoLogo -NoProfile -NonInteractive -Command \"";
    command += L"$ErrorActionPreference='Stop'; ";
    command += L"$zip=" + toPowerShellSingleQuotedLiteral(zipPath) + L"; ";
    command += L"$tmp=" + toPowerShellSingleQuotedLiteral(tempDir) + L"; ";
    command += L"Expand-Archive -Path $zip -DestinationPath $tmp -Force; ";
    command += L"$ff=Get-ChildItem -Path $tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1; ";
    command += L"$fp=Get-ChildItem -Path $tmp -Recurse -Filter ffprobe.exe | Select-Object -First 1; ";
    command += L"if(-not $ff){throw 'the archive does not contain ffmpeg.exe'}; ";
    command += L"if(-not $fp){throw 'the archive does not contain ffprobe.exe'}; ";
    command += L"Copy-Item -LiteralPath $ff.FullName -Destination (Join-Path $tmp 'extracted-ffmpeg.exe') -Force; ";
    command += L"Copy-Item -LiteralPath $fp.FullName -Destination (Join-Path $tmp 'extracted-ffprobe.exe') -Force; ";
    command += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        failureReason = "Extraction failed: PowerShell could not be started (Windows error " +
                        std::to_string(static_cast<unsigned long>(::GetLastError())) + ").";
        return false;
    }

    constexpr DWORD kExtractionTimeoutMs = 300000;   // 5 minutes for a ~200 MB archive
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kExtractionTimeoutMs);
    DWORD waited = WAIT_TIMEOUT;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        waited = WaitForSingleObject(pi.hProcess, 100);
        if (waited != WAIT_TIMEOUT) {
            break;
        }
    }
    bool timedOut = false;
    if (waited != WAIT_OBJECT_0) {
        timedOut = true;
        // Terminate and *wait*: a child that is still alive after this point would
        // keep writing into the folder the caller is about to delete.
        ::TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    DWORD exitCode = 1;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    if (timedOut) {
        failureReason = stop.stop_requested() ? "Extraction was cancelled." :
                       "Extraction did not finish within " + std::to_string(kExtractionTimeoutMs / 1000) +
                        " seconds and was stopped. Retry, or extract ffmpeg.exe and ffprobe.exe from the archive "
                        "yourself and place them next to MonoSect.exe.";
        std::filesystem::remove_all(tempDir, error);
        return false;
    }
    if (exitCode != 0) {
        failureReason = "Extraction failed (PowerShell exit code " + std::to_string(static_cast<unsigned long>(exitCode)) +
                        "). The archive may be incomplete; retry the download.";
        std::filesystem::remove_all(tempDir, error);
        return false;
    }

    const std::filesystem::path extractedFfmpeg = tempDir / "extracted-ffmpeg.exe";
    const std::filesystem::path extractedFfprobe = tempDir / "extracted-ffprobe.exe";
    if (!std::filesystem::exists(extractedFfmpeg) || !std::filesystem::exists(extractedFfprobe)) {
        failureReason = "Extraction reported success but the two tools are not there.";
        std::filesystem::remove_all(tempDir, error);
        return false;
    }
    return true;
}

} // namespace

bool ensureFFmpeg(const std::filesystem::path& appDirectory,
                  const std::function<void(const std::string&)>& onStatus, std::stop_token stop) {
    if (stop.stop_requested()) {
        return false;
    }
    const std::filesystem::path ffmpegPath = appDirectory / "ffmpeg.exe";
    const std::filesystem::path ffprobePath = appDirectory / "ffprobe.exe";
    const std::filesystem::path zipPath = appDirectory / "ffmpeg_download.zip";
    const monosect::core::DependencyPin archivePin = monosect::core::pinnedArchive();

    const auto report = [&onStatus](const std::string& line) {
        if (onStatus) {
            onStatus(line);
        }
    };
    const auto cleanupArchive = [&zipPath]() {
        std::error_code error;
        std::filesystem::remove(zipPath, error);
    };
    const auto discardTools = [&appDirectory, &ffmpegPath, &ffprobePath]() {
        std::error_code error;
        std::filesystem::remove(ffmpegPath, error);
        std::filesystem::remove(ffprobePath, error);
        std::filesystem::remove(monosect::core::pinnedFfmpeg().sidecarPath(ffmpegPath), error);
        std::filesystem::remove(monosect::core::pinnedFfprobe().sidecarPath(ffprobePath), error);
        (void)appDirectory;
    };

    // ---- 1. Are the tools already there, and are they identifiable? ---------
    //
    // The files still have to pass verification.  A tool that was swapped,
    // patched or half-written is not run silently, and - importantly - an
    // existing good toolchain is not thrown away before the replacement has been
    // verified either.
    if (std::filesystem::exists(ffmpegPath) && std::filesystem::exists(ffprobePath)) {
        if (verifyToolchain(appDirectory, onStatus, /*discardOnFailure=*/false)) {
            report("FFmpeg ready: the tools next to MonoSect are the pinned release.");
            return true;
        }
        report("The tools next to MonoSect are not the pinned release. A fresh copy will be downloaded.");
    }

    // ---- 2. Download the pinned archive -------------------------------------
    std::wstring host;
    std::wstring path;
    if (!splitPinnedUrl(archivePin.url, host, path)) {
        report("The pinned FFmpeg address is not a valid https URL, which is a build defect: " + archivePin.url);
        return false;
    }
    report("Downloading the pinned FFmpeg build: " + archivePin.version);
    if (!downloadFile(host, path, zipPath, onStatus, stop)) {
        cleanupArchive();
        report("The FFmpeg download failed. Place ffmpeg.exe and ffprobe.exe next to MonoSect.exe to use MonoSect "
               "without downloading anything.");
        return false;
    }

    // ---- 3. Verify the archive BEFORE anything opens it ----------------------
    //
    // This is the trust anchor: a hash mismatch means the bytes on disk are not
    // the bytes the release published, and the file is deleted rather than
    // unpacked.  When this build pins no archive hash the check is skipped rather
    // than faked, and that is said out loud.
    report("Download complete. Verifying the archive...");
    const monosect::core::DependencyCheck archiveCheck =
        monosect::core::verifyToolFile(zipPath, archivePin, /*recordWhenUnpinned=*/false);
    report(archiveCheck.message);
    if (!archiveCheck.ok()) {
        cleanupArchive();
        report("The downloaded archive was discarded because it is not the pinned release. MonoSect will not "
               "execute a tool it cannot identify.");
        return false;
    }
    if (!archivePin.hasPinnedHash()) {
        report("This build of MonoSect does not pin an archive digest, so the download could only be checked for "
               "being readable. A release build pins it.");
    }

    // ---- 4. Extract into a temporary folder ----------------------------------
    std::string extractionFailure;
    if (!extractFFmpegFromZip(zipPath, appDirectory, onStatus, extractionFailure, stop)) {
        cleanupArchive();
        report(extractionFailure);
        return false;
    }

    // ---- 5. Verify the extracted executables BEFORE installing them -----------
    const std::filesystem::path staging = appDirectory / "ffmpeg_extract_tmp";
    const std::filesystem::path stagedFfmpeg = staging / "extracted-ffmpeg.exe";
    const std::filesystem::path stagedFfprobe = staging / "extracted-ffprobe.exe";
    const std::vector<std::pair<std::filesystem::path, monosect::core::DependencyPin>> staged = {
        {stagedFfmpeg, monosect::core::pinnedFfmpeg()},
        {stagedFfprobe, monosect::core::pinnedFfprobe()},
    };
    bool stagedOk = true;
    for (const auto& [toolPath, pin] : staged) {
        const monosect::core::DependencyCheck check =
            monosect::core::verifyToolFile(toolPath, pin, /*recordWhenUnpinned=*/false);
        report(pin.name + ": " + check.message);
        if (!check.ok()) {
            stagedOk = false;
        }
    }
    if (!stagedOk) {
        std::error_code stagingError;
        std::filesystem::remove_all(staging, stagingError);
        cleanupArchive();
        discardTools();
        report("The tools inside the archive did not match the pinned release, so nothing was installed. MonoSect "
               "will not run a tool it cannot identify.");
        return false;
    }

    // ---- 6. Install ----------------------------------------------------------
    //
    // Verified files are moved into place.  MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING is used because a broken toolchain may still be
    // sitting there from the step-1 failure, and leaving it would be worse than
    // replacing it.  Only after the move is the staging folder removed, so the
    // verified bytes are never deleted before they are installed.
    std::error_code error;
    if (!MoveFileExW(stagedFfmpeg.wstring().c_str(), ffmpegPath.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
        !MoveFileExW(stagedFfprobe.wstring().c_str(), ffprobePath.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = ::GetLastError();
        std::filesystem::remove_all(staging, error);
        cleanupArchive();
        discardTools();
        report("The verified tools could not be installed next to MonoSect (Windows error " +
               std::to_string(static_cast<unsigned long>(code)) +
               "). Choose a folder you can write to.");
        return false;
    }
    std::filesystem::remove_all(staging, error);
    cleanupArchive();

    // A build without executable pins records what it just installed, so a later
    // substitution of either file is detected even though the archive was
    // verified.  This runs *after* the verified files are in place, so the
    // sidecar records the file that is actually going to be used.
    for (const auto& [toolPath, pin] : std::vector<std::pair<std::filesystem::path, monosect::core::DependencyPin>>{
             {ffmpegPath, monosect::core::pinnedFfmpeg()},
             {ffprobePath, monosect::core::pinnedFfprobe()}}) {
        if (pin.hasPinnedHash()) {
            continue;
        }
        const monosect::core::DependencyCheck record =
            monosect::core::verifyToolFile(toolPath, pin, /*recordWhenUnpinned=*/true);
        report(record.message);
    }

    if (!verifyToolchain(appDirectory, onStatus, /*discardOnFailure=*/true)) {
        discardTools();
        report("The installed tools did not pass verification and were removed.");
        return false;
    }
    report("FFmpeg ready: " + archivePin.version);
    return true;
}

}
