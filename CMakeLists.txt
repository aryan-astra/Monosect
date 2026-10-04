cmake_minimum_required(VERSION 3.24)

set(MONOSECT_VERSION_FILE "${CMAKE_SOURCE_DIR}/version.txt")
if (NOT EXISTS "${MONOSECT_VERSION_FILE}")
    message(FATAL_ERROR "version.txt not found at repository root: ${MONOSECT_VERSION_FILE}")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MONOSECT_VERSION_FILE}")
file(READ "${MONOSECT_VERSION_FILE}" MONOSECT_VERSION_RAW)
string(STRIP "${MONOSECT_VERSION_RAW}" MONOSECT_VERSION)
if (NOT MONOSECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "version.txt must contain MAJOR.MINOR.PATCH (for example: 1.1.0)")
endif ()

project(Monosect VERSION "${MONOSECT_VERSION}" LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Build layout
#
#   monosect_core   platform-neutral core + engine (no GUI, no Windows headers)
#   monosect-cli    command line front end, used for verification and CI
#   monosect-tests  unit tests for the rules that must never regress
#   MonoSect        the Windows GUI application (Windows builds only)
#
# The core is deliberately buildable on Linux and macOS: that is what makes the
# verification harness in tests/harness possible.  The product itself is the
# Windows GUI, and every Windows-only file is marked as such in its header.
# ---------------------------------------------------------------------------

option(MONOSECT_BUILD_GUI "Build the Windows GUI application" ON)
option(MONOSECT_BUILD_TESTS "Build the unit tests and the command line harness" ON)
option(MONOSECT_STRICT_WARNINGS "Treat warnings as errors in the core library" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

if (MSVC)
    add_compile_options(/W4 /permissive-)
else ()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
        # GCC 12 reports a false positive -Wrestrict from libstdc++'s
        # operator+(const char*, string&&) when it inlines basic_string::_M_replace
        # ("accessing 9223372036854775810 or more bytes ... may overlap").  The
        # diagnostic is emitted in whichever translation unit inlines the helper
        # first, so it moves between files as the code changes and cannot be fixed
        # at one call site.  It is scoped to the compiler versions that produce it;
        # GCC 13 and later, Clang and MSVC do not need it.
        add_compile_options(-Wno-restrict)
    endif ()
    if (MONOSECT_STRICT_WARNINGS)
        add_compile_options(-Werror)
    endif ()
endif ()

set(MONOSECT_CORE_SOURCES
    src/core/Result.cpp
    src/core/Bytes.cpp
    src/core/Rational.cpp
    src/core/Json.cpp
    src/core/Sha256.cpp
    src/core/MediaModel.cpp
    src/core/ProbeParsing.cpp
    src/core/PacketIndex.cpp
    src/core/Constraints.cpp
    src/core/DependencyPin.cpp
    src/core/CapabilityMatrix.cpp
    src/core/ResourcePolicy.cpp
    src/core/SplitPlan.cpp
    src/core/SizeModel.cpp
    src/core/OutputNaming.cpp
    src/core/SplitPlanner.cpp
    src/core/MergePlanner.cpp
    src/core/Verification.cpp
    src/core/JobState.cpp
    src/core/WatchReadiness.cpp
    src/engine/ProcessSupport.cpp
    src/engine/FFprobeService.cpp
    src/engine/FfmpegArgs.cpp
    src/engine/OutputCommitter.cpp
    src/engine/VerificationEngine.cpp
    src/engine/JobRunner.cpp
    src/engine/MergeRunner.cpp
    src/engine/JobStore.cpp
)

# The presentation model and the help text are platform-neutral: they are built
# on Linux too, so the wording rules can be unit-tested (tests/core/UiModelTests.cpp).
add_library(monosect_uimodel STATIC
    src/monitor/SystemMonitor.cpp
    src/monitor/HardwareCapabilities.cpp
    src/ui/UiController.cpp
    src/ui/UiModel.cpp
    src/ui/HelpContent.cpp
)
target_include_directories(monosect_uimodel PUBLIC src)
target_link_libraries(monosect_uimodel PUBLIC monosect_core)

add_library(monosect_core STATIC ${MONOSECT_CORE_SOURCES})
target_include_directories(monosect_core PUBLIC src)
target_compile_definitions(monosect_core PUBLIC MONOSECT_VERSION_STR="${PROJECT_VERSION}")

# The pinned FFmpeg the product ships (TODO 215 - TODO 225).
#
# These three variables are what turns the download into something identifiable.
# The URL points at an *immutable* autobuild tag, never at `latest`, and
# `MONOSECT_FFMPEG_SHA256` is the archive's SHA-256: the value GitHub publishes as
# the asset's `digest`, which can be read without downloading 200 MB with
#
#   gh api repos/BtbN/FFmpeg-Builds/releases/tags/<tag> --jq '.assets[]
#         | select(.name|test("win64-gpl\\.zip$")) | .digest'
#
# When the hash is empty (a developer build) core/DependencyPin.cpp falls back to
# trust-on-first-use: the hash of the first download is recorded next to the
# tools and enforced from then on, so a substituted binary is still detected.  The
# values reach C++ as compile definitions, which is why they are declared here
# rather than being read from an environment variable at run time: a build that
# cannot state what it expects must not be able to silently accept anything.
#
# Maintenance: the pin has to be refreshed when the FFmpeg release MonoSect
# tracks is updated.  That is a deliberate, visible edit with a checksum in it -
# which is the point.  Note that upstream prunes old autobuild tags, so a very
# old pin can become a 404; the bootstrap reports that as an actionable download
# error rather than falling back to an unpinned address.
set(MONOSECT_FFMPEG_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-03-18-14/ffmpeg-N-127142-g12b7b9891b-win64-gpl.zip"
    CACHE STRING "URL of the pinned FFmpeg archive MonoSect downloads")
set(MONOSECT_FFMPEG_VERSION "N-127142-g12b7b9891b (autobuild-2026-10-03-18-14, win64 gpl)"
    CACHE STRING "Release name of the pinned FFmpeg build, recorded in every job record")
set(MONOSECT_FFMPEG_SHA256 "a885f564dee2b60f69ab866c6c89b96ae531fc2ee1f24ff8b5b1a6d29960a96b"
    CACHE STRING "SHA-256 of the pinned FFmpeg archive (empty = trust on first use)")
# The two executables are separate files with separate digests, so they need their
# own pins; one archive hash cannot match both.  They are defence in depth on top
# of the archive pin, and they matter because extraction on Windows shells out to
# PowerShell.  Both are produced by extracting the pinned archive once and hashing
# the results - the digest command above does the archive, and
# `tools/pin-ffmpeg-hashes.ps1` does the executables.
set(MONOSECT_FFMPEG_EXE_SHA256 "" CACHE STRING "SHA-256 of ffmpeg.exe inside the pinned archive")
set(MONOSECT_FFPROBE_EXE_SHA256 "" CACHE STRING "SHA-256 of ffprobe.exe inside the pinned archive")
foreach (variable MONOSECT_FFMPEG_URL MONOSECT_FFMPEG_VERSION MONOSECT_FFMPEG_SHA256
                 MONOSECT_FFMPEG_EXE_SHA256 MONOSECT_FFPROBE_EXE_SHA256)
    if (NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} must have a value")
    endif ()
endforeach ()
# A pinned hash that is not 64 hex characters is a build error, not something the
# runtime discovers later: shipping a "pinned" build with a broken pin would mean
# every user hits the same failure at first run.  The length is checked with
# string(LENGTH) rather than a {64} regex, because CMake's regex flavour does not
# support counted repetition - a check that silently never fires is worse than no
# check at all.
foreach (variable MONOSECT_FFMPEG_SHA256 MONOSECT_FFMPEG_EXE_SHA256 MONOSECT_FFPROBE_EXE_SHA256)
    string(LENGTH "${${variable}}" MONOSECT_PIN_LENGTH)
    if (NOT MONOSECT_PIN_LENGTH EQUAL 0 AND NOT MONOSECT_PIN_LENGTH EQUAL 64)
        message(FATAL_ERROR
            "${variable} is ${MONOSECT_PIN_LENGTH} characters long. It must be 64 hexadecimal "
            "characters, or empty for a developer build.")
    endif ()
    string(REGEX MATCH "^[0-9a-fA-F]+$" MONOSECT_PIN_MATCH "${${variable}}")
    if (NOT MONOSECT_PIN_MATCH STREQUAL "${${variable}}")
        message(FATAL_ERROR "${variable} must contain only hexadecimal characters, or be empty.")
    endif ()
endforeach ()
target_compile_definitions(monosect_core PUBLIC
    MONOSECT_FFMPEG_URL="${MONOSECT_FFMPEG_URL}"
    MONOSECT_FFMPEG_VERSION="${MONOSECT_FFMPEG_VERSION}"
    MONOSECT_FFMPEG_SHA256="${MONOSECT_FFMPEG_SHA256}"
    MONOSECT_FFMPEG_EXE_SHA256="${MONOSECT_FFMPEG_EXE_SHA256}"
    MONOSECT_FFPROBE_EXE_SHA256="${MONOSECT_FFPROBE_EXE_SHA256}"
)

if (WIN32)
    target_sources(monosect_core PRIVATE
        src/engine/win/WindowsProcessRunner.cpp
        src/engine/WatchFolder.cpp
    )
    # bcrypt/shell32/ole32 for the Win32 pieces of the engine.  The presentation
    # model needs pdh for the disk counters (monitor/SystemMonitor.cpp), and
    # because it is a static library that requirement has to be declared *here* -
    # declaring it on the executable is too late, which is what the first Windows
    # build of this tree produced as a pile of undefined Pdh* references.
    target_link_libraries(monosect_core PUBLIC bcrypt shell32 ole32)
    target_link_libraries(monosect_uimodel PUBLIC pdh)
else ()
    target_sources(monosect_core PRIVATE src/engine/posix/PosixProcessRunner.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(monosect_core PUBLIC Threads::Threads)
endif ()

# ---------------------------------------------------------------------------
# Command line harness and tests
# ---------------------------------------------------------------------------
if (MONOSECT_BUILD_TESTS)
    include(CTest)
    add_executable(monosect-cli tests/harness/monosect_cli.cpp)
    target_link_libraries(monosect-cli PRIVATE monosect_core)

    add_executable(monosect-tests
        tests/core/TestMain.cpp
        tests/core/BytesTests.cpp
        tests/core/RationalTests.cpp
        tests/core/JsonTests.cpp
        tests/core/ProbeParsingTests.cpp
        tests/core/PacketIndexTests.cpp
        tests/core/ConstraintsTests.cpp
        tests/core/PlannerTests.cpp
        tests/core/VerificationTests.cpp
        tests/core/NamingTests.cpp
        tests/core/MergePlannerTests.cpp
        tests/core/JobStoreTests.cpp
        tests/core/UiModelTests.cpp
        tests/core/UiControllerTests.cpp
        tests/core/DependencyPinTests.cpp
        tests/core/WatchFolderTests.cpp
    )
    target_include_directories(monosect-tests PRIVATE src tests)
    target_link_libraries(monosect-tests PRIVATE monosect_core monosect_uimodel)

    add_test(NAME monosect-unit-tests COMMAND monosect-tests)

    # The end-to-end suite drives the real FFmpeg tools through monosect-cli and
    # then re-checks every written file with an *independent* checker, so a bug in
    # MonoSect's own verification cannot make the suite pass.  It is written in
    # Python because Python is the only interpreter MonoSect can rely on for this
    # (it is already needed for check_parts.py, and it is the interpreter the
    # FFmpeg fixtures are generated through).  It used to be a bash script, which
    # meant the suite simply could not run on a stock Windows machine - bash is not
    # part of Windows, and the developer's own machine has none.
    find_program(MONOSECT_PYTHON_EXECUTABLE NAMES python3 python)
    find_program(MONOSECT_FFMPEG_EXECUTABLE ffmpeg)
    find_program(MONOSECT_FFPROBE_EXECUTABLE ffprobe)
    if (MONOSECT_FFMPEG_EXECUTABLE AND MONOSECT_FFPROBE_EXECUTABLE AND MONOSECT_PYTHON_EXECUTABLE)
        add_test(
            NAME monosect-end-to-end
            COMMAND ${CMAKE_COMMAND} -E env
                    "MONOSECT_FFMPEG=${MONOSECT_FFMPEG_EXECUTABLE}"
                    "MONOSECT_FFPROBE=${MONOSECT_FFPROBE_EXECUTABLE}"
                    ${MONOSECT_PYTHON_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/harness/end_to_end.py
                    $<TARGET_FILE:monosect-cli>
                    ${CMAKE_CURRENT_BINARY_DIR}/e2e
        )
        set_tests_properties(monosect-end-to-end PROPERTIES TIMEOUT 3600)
    else ()
        message(STATUS "monosect: ffmpeg, ffprobe or python not found - the end-to-end test is not registered")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# The Windows GUI application
# ---------------------------------------------------------------------------
if (NOT WIN32)
    message(STATUS "monosect: not a Windows build - the GUI is skipped (core, CLI and tests are built).")
    return ()
endif ()

if (NOT MONOSECT_BUILD_GUI)
    message(STATUS "monosect: MONOSECT_BUILD_GUI=OFF - the GUI is skipped.")
    return ()
endif ()

if (MSVC)
    message(FATAL_ERROR "The GUI is built with MinGW-w64 g++ (see README.md).")
endif ()

add_compile_options(-O2)

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
if (NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    FetchContent_Declare(
        imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
    )
    FetchContent_MakeAvailable(imgui_src)
    set(IMGUI_DIR "${imgui_src_SOURCE_DIR}")
endif ()

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw opengl32)

# The Windows GUI.  Everything here is Windows-only: it is never compiled on the
# development machine.  The decisions it shows live in monosect_uimodel, which is
# compiled and tested everywhere.
set(APP_SOURCES
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/Branding.cpp
    src/engine/FFmpegBootstrap.cpp
    resources/Monosect.rc
)

set(MONOSECT_ICON_SQUARE "${CMAKE_SOURCE_DIR}/Monosect-icon-square.png")
set(MONOSECT_LOGO_FULL "${CMAKE_SOURCE_DIR}/Monosect-full-logo.png")
set(MONOSECT_ICO "${CMAKE_SOURCE_DIR}/resources/Monosect.ico")

if (EXISTS "${MONOSECT_ICON_SQUARE}")
    execute_process(
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
        "Add-Type -AssemblyName System.Drawing; $src='${MONOSECT_ICON_SQUARE}'; $dst='${MONOSECT_ICO}'; $sizes=@(16,32,48,64,256); $source=[System.Drawing.Image]::FromFile($src); $images=@(); foreach($s in $sizes){ $bmp=New-Object System.Drawing.Bitmap($s,$s); $g=[System.Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic; $g.DrawImage($source,0,0,$s,$s); $ms=New-Object System.IO.MemoryStream; $bmp.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); $images += ,@($s,$ms.ToArray()); }; $source.Dispose(); $fs=[System.IO.File]::Open($dst,[System.IO.FileMode]::Create); $bw=New-Object System.IO.BinaryWriter($fs); $bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$images.Count); $offset=6 + 16*$images.Count; foreach($img in $images){ $s=[int]$img[0]; $bytes=[byte[]]$img[1]; $bw.Write([byte]($(if($s -ge 256){0}else{$s}))); $bw.Write([byte]($(if($s -ge 256){0}else{$s}))); $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$bytes.Length); $bw.Write([UInt32]$offset); $offset += $bytes.Length; }; foreach($img in $images){ $bw.Write([byte[]]$img[1]); } $bw.Close(); $fs.Close();"
        RESULT_VARIABLE MONOSECT_ICO_RESULT
    )
    if (NOT MONOSECT_ICO_RESULT EQUAL 0)
        message(WARNING "Failed to generate Monosect.ico from Monosect-icon-square.png")
    endif ()
else ()
    message(WARNING "Monosect-icon-square.png not found; executable icon resource may be missing")
endif ()

add_executable(Monosect ${APP_SOURCES})
target_include_directories(Monosect PRIVATE src)
target_link_libraries(Monosect PRIVATE monosect_core monosect_uimodel imgui glfw opengl32 gdiplus winhttp
    shell32 comdlg32 ole32 pdh)   # folder/file dialogs, ShellExecute, performance counters
target_compile_options(Monosect PRIVATE -O2 -march=x86-64-v2 -mtune=generic)
set_target_properties(Monosect PROPERTIES WIN32_EXECUTABLE ON VERSION "${PROJECT_VERSION}")

if (EXISTS "${MONOSECT_ICON_SQUARE}")
    add_custom_command(TARGET Monosect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MONOSECT_ICON_SQUARE}" "$<TARGET_FILE_DIR:Monosect>/Monosect-icon-square.png")
endif ()
if (EXISTS "${MONOSECT_LOGO_FULL}")
    add_custom_command(TARGET Monosect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MONOSECT_LOGO_FULL}" "$<TARGET_FILE_DIR:Monosect>/Monosect-full-logo.png")
endif ()
