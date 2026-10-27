# Third-party software in MonoSect

MonoSect's own source is licensed GPL-3.0-only; see LICENSE.

- **FFmpeg / ffprobe 9.0.2 essentials:** external executables from GyanD/codexffmpeg, pinned in `resources/ffmpeg-pin.json`. The package includes the upstream GPL license and build README as `FFmpeg-LICENSE.txt` and `FFmpeg-README.txt`. Source: https://github.com/FFmpeg/FFmpeg/tree/n9.0.2. Build information: https://github.com/GyanD/codexffmpeg/tree/9.0.2 and https://www.gyan.dev/ffmpeg/builds/. Consult the upstream README for linked libraries and source information.
- **GLFW 3.4:** zlib/libpng license, reproduced in `licenses/GLFW.txt`. Source: https://github.com/glfw/glfw/tree/3.4.
- **Dear ImGui 1.91.5:** MIT license, reproduced in `licenses/DearImGui.txt`. Source: https://github.com/ocornut/imgui/tree/v1.91.5.
- **GCC and MinGW runtime DLLs:** GPLv3 with GCC Runtime Library Exception where applicable; exception and MinGW notices are in `licenses/`. The packaged runtime files depend on the compiler used; `build-manifest.json` records that compiler.

Redistributors are responsible for providing Corresponding Source and notices as required by the applicable licenses, including bundled libraries. These links are source references, not a written source offer. A passing build or smoke test does not establish redistribution compliance.
