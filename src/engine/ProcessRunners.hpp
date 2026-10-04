// MonoSect — the process runner that belongs to the platform.
//
// Only one of these is compiled: the Windows build uses the job-object runner,
// the development/verification build uses the POSIX one.
#pragma once

#include "engine/ProcessRunner.hpp"

namespace monosect::core {

#if defined(_WIN32)
/// Windows: CreateProcessW + a Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
/// so that no FFmpeg process can outlive MonoSect, not even after a crash.
[[nodiscard]] IProcessRunner& windowsProcessRunner();
#else
/// POSIX: fork/exec with its own process group; cancellation signals the group.
[[nodiscard]] IProcessRunner& posixProcessRunner();
#endif

/// The runner that belongs to the platform this build targets.
///
/// Code that is not platform-specific - the verification harness
/// (tests/harness/monosect_cli.cpp) above all - calls this instead of choosing a
/// runner with #ifdef.  That is what lets the harness run unchanged on the
/// Windows development machine and in a Linux CI container, so the tests that
/// verify the engine are the same tests everywhere.
[[nodiscard]] IProcessRunner& defaultProcessRunner();

} // namespace monosect::core
