cmake_minimum_required(VERSION 3.24)
project(Monosect VERSION 1.1.0 LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "This refactor targets Windows + MinGW only.")
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING "Production profile expects Release build.")
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (MSVC)
    message(FATAL_ERROR "This configuration is intended for MinGW-w64 g++.")
else ()
    find_program(GCC_AR gcc-ar)
    find_program(GCC_RANLIB gcc-ranlib)
    if (GCC_AR)
        set(CMAKE_AR ${GCC_AR})
    endif ()
    if (GCC_RANLIB)
        set(CMAKE_RANLIB ${GCC_RANLIB})
    endif ()

    add_compile_options(-Wall -Wextra -Wpedantic)
    add_link_options(-static)
endif ()

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
        GIT_TAG docking
    )
    FetchContent_MakeAvailable(imgui_src)
    set(IMGUI_DIR "${imgui_src_SOURCE_DIR}")
endif ()

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw opengl32)

set(APP_SOURCES
    src/main.cpp
    src/ui/App.cpp
    src/ui/Branding.cpp
    src/engine/FFmpegBootstrap.cpp
    src/engine/FFmpegProcess.cpp
    src/engine/GOPAnalyzer.cpp
    src/engine/MediaProbe.cpp
    src/engine/SplitEngine.cpp
    src/engine/ThreadPool.cpp
    src/engine/WatchFolder.cpp
    src/precision/SizePrecisionEngine.cpp
    src/monitor/SystemMonitor.cpp
    src/monitor/HardwareDetector.cpp
    resources/Monosect.rc
)

set(MONOSECT_ICON_SQUARE "${CMAKE_SOURCE_DIR}/Monosect-icon-square.png")
set(MONOSECT_LOGO_FULL "${CMAKE_SOURCE_DIR}/Monosect-full-logo.png")
set(MONOSECT_ICO "${CMAKE_SOURCE_DIR}/resources/Monosect.ico")

if (EXISTS "${MONOSECT_ICON_SQUARE}")
    execute_process(
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
        "Add-Type -AssemblyName System.Drawing;\
         $src='${MONOSECT_ICON_SQUARE}'; $dst='${MONOSECT_ICO}';\
         $sizes=@(16,32,48,64,256);\
         $source=[System.Drawing.Image]::FromFile($src);\
         $images=@();\
         foreach($s in $sizes){\
           $bmp=New-Object System.Drawing.Bitmap($s,$s);\
           $g=[System.Drawing.Graphics]::FromImage($bmp);\
           $g.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic;\
           $g.DrawImage($source,0,0,$s,$s);\
           $ms=New-Object System.IO.MemoryStream;\
           $bmp.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png);\
           $g.Dispose(); $bmp.Dispose();\
           $images += ,@($s,$ms.ToArray());\
         };\
         $source.Dispose();\
         $fs=[System.IO.File]::Open($dst,[System.IO.FileMode]::Create);\
         $bw=New-Object System.IO.BinaryWriter($fs);\
         $bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$images.Count);\
         $offset=6 + 16*$images.Count;\
         foreach($img in $images){\
           $s=[int]$img[0]; $bytes=[byte[]]$img[1];\
           $bw.Write([byte]($(if($s -ge 256){0}else{$s})));\
           $bw.Write([byte]($(if($s -ge 256){0}else{$s})));\
           $bw.Write([byte]0); $bw.Write([byte]0);\
           $bw.Write([UInt16]1); $bw.Write([UInt16]32);\
           $bw.Write([UInt32]$bytes.Length);\
           $bw.Write([UInt32]$offset);\
           $offset += $bytes.Length;\
         };\
         foreach($img in $images){ $bw.Write([byte[]]$img[1]); }\
         $bw.Close(); $fs.Close();"
        RESULT_VARIABLE MONOSECT_ICO_RESULT
    )
    if (NOT MONOSECT_ICO_RESULT EQUAL 0)
        message(WARNING "Failed to generate Monosect.ico from Monosect-icon-square.png")
    endif ()
else ()
    message(WARNING "Monosect-icon-square.png not found; executable icon resource may be missing")
endif ()

add_executable(Monosect ${APP_SOURCES})

target_compile_definitions(Monosect PRIVATE
    MONOSECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MONOSECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MONOSECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    MONOSECT_VERSION_STR=\"${PROJECT_VERSION}\"
)

target_compile_options(Monosect PRIVATE -O3 -march=native -flto -ffat-lto-objects)
target_link_options(Monosect PRIVATE -flto)

target_include_directories(Monosect PRIVATE src)

target_link_libraries(Monosect
    PRIVATE
        imgui
        glfw
    opengl32
    gdiplus
    winhttp
)

set_target_properties(Monosect PROPERTIES WIN32_EXECUTABLE ON)

if (EXISTS "${MONOSECT_ICON_SQUARE}")
    add_custom_command(TARGET Monosect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MONOSECT_ICON_SQUARE}" "$<TARGET_FILE_DIR:Monosect>/Monosect-icon-square.png")
endif ()
if (EXISTS "${MONOSECT_LOGO_FULL}")
    add_custom_command(TARGET Monosect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MONOSECT_LOGO_FULL}" "$<TARGET_FILE_DIR:Monosect>/Monosect-full-logo.png")
endif ()
