#include "engine/FFmpegBootstrap.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// winhttp.lib is linked via CMakeLists.txt

namespace vsm {

namespace {

// Download a URL to a local file using WinHTTP. Follows one redirect level.
bool downloadFile(const std::wstring& host, const std::wstring& path,
                  const std::filesystem::path& outputPath,
                  const std::function<void(const std::string&)>& onStatus) {
    HINTERNET hSession = WinHttpOpen(L"Monosect/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

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
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
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
    return totalRead > 0;
}

// Extract ffmpeg.exe and ffprobe.exe from a zip using PowerShell's Expand-Archive.
bool extractFFmpegFromZip(const std::filesystem::path& zipPath,
                          const std::filesystem::path& destDir,
                          const std::function<void(const std::string&)>& onStatus) {
    if (onStatus) onStatus("Extracting FFmpeg binaries...");

    // Use PowerShell to extract + locate binaries
    const auto tempDir = destDir / "ffmpeg_temp";

    // Build PowerShell command
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
    cmd += L"$ErrorActionPreference='Stop'; ";
    cmd += L"$zip='" + zipPath.wstring() + L"'; ";
    cmd += L"$tmp='" + tempDir.wstring() + L"'; ";
    cmd += L"$dst='" + destDir.wstring() + L"'; ";
    cmd += L"if(Test-Path $tmp){Remove-Item -Recurse -Force $tmp}; ";
    cmd += L"Expand-Archive -Path $zip -DestinationPath $tmp -Force; ";
    cmd += L"$ff=Get-ChildItem -Path $tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1; ";
    cmd += L"$fp=Get-ChildItem -Path $tmp -Recurse -Filter ffprobe.exe | Select-Object -First 1; ";
    cmd += L"if($ff){Copy-Item $ff.FullName (Join-Path $dst 'ffmpeg.exe') -Force}; ";
    cmd += L"if($fp){Copy-Item $fp.FullName (Join-Path $dst 'ffprobe.exe') -Force}; ";
    cmd += L"Remove-Item -Recurse -Force $tmp; ";
    cmd += L"Remove-Item -Force $zip; ";
    cmd += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, 120000); // 2 min timeout
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0) return false;

    // Verify both exist
    return std::filesystem::exists(destDir / "ffmpeg.exe") &&
           std::filesystem::exists(destDir / "ffprobe.exe");
}

} // namespace

bool ensureFFmpeg(const std::filesystem::path& appDirectory,
                  const std::function<void(const std::string&)>& onStatus) {
    const auto ffmpegPath  = appDirectory / "ffmpeg.exe";
    const auto ffprobePath = appDirectory / "ffprobe.exe";

    // Already present?
    if (std::filesystem::exists(ffmpegPath) && std::filesystem::exists(ffprobePath)) {
        return true;
    }

    if (onStatus) onStatus("FFmpeg not found. Downloading (~30 MB)...");

    // Download from GitHub BtbN releases (static essentials build)
    const std::wstring host = L"github.com";
    const std::wstring path = L"/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
    const auto zipPath = appDirectory / "ffmpeg_download.zip";

    if (!downloadFile(host, path, zipPath, onStatus)) {
        if (onStatus) onStatus("FFmpeg download failed. Please place ffmpeg.exe and ffprobe.exe next to the application.");
        // Clean up partial download
        std::error_code ec;
        std::filesystem::remove(zipPath, ec);
        return false;
    }

    if (onStatus) onStatus("Download complete. Extracting...");

    if (!extractFFmpegFromZip(zipPath, appDirectory, onStatus)) {
        if (onStatus) onStatus("Extraction failed. Please manually extract ffmpeg.exe and ffprobe.exe.");
        return false;
    }

    // Final verify
    if (std::filesystem::exists(ffmpegPath) && std::filesystem::exists(ffprobePath)) {
        if (onStatus) onStatus("FFmpeg ready.");
        return true;
    }

    if (onStatus) onStatus("FFmpeg setup incomplete. Binaries not found after extraction.");
    return false;
}

}
