"""Transitions between the platforms of bazel/platforms.

kernel_transition moves a dependency to //bazel/platforms:ios_kernel_arm64 (the kernel cc toolchain) and remembers the
platforms it came from in //bazel/platforms:outer; outer_transition moves back to exactly those, so what the kernel's
compiles read from the rest of the build (XNU's headers, generated headers) is the same configured target, at the same
paths, as what the rest of the build uses: XNU is configured (and built) once.

macos_user_transition moves a dependency to //bazel/platforms:macos_user_arm64: a host twin of iOS user-space code (the
Apple toolchain's macOS recipe), not the host's.
"""

KERNEL_PLATFORM = "//bazel/platforms:ios_kernel_arm64"
MACOS_USER_PLATFORM = "//bazel/platforms:macos_user_arm64"
_PLATFORMS = "//command_line_option:platforms"
_OUTER = "//bazel/platforms:outer"

# Stands for "no --platforms" (the host platform) in _OUTER.
_HOST = "-"

def _kernel_impl(settings, attr):
    outer = settings[_OUTER]
    if not outer:
        outer = ",".join([str(p) for p in settings[_PLATFORMS]]) or _HOST
    return {_PLATFORMS: [str(Label(KERNEL_PLATFORM))], _OUTER: outer}

kernel_transition = transition(implementation = _kernel_impl, inputs = [_PLATFORMS, _OUTER], outputs = [_PLATFORMS, _OUTER])

def _outer_impl(settings, attr):
    outer = settings[_OUTER]
    if not outer:
        return {_PLATFORMS: settings[_PLATFORMS], _OUTER: ""}
    return {_PLATFORMS: [] if outer == _HOST else outer.split(","), _OUTER: ""}

outer_transition = transition(implementation = _outer_impl, inputs = [_PLATFORMS, _OUTER], outputs = [_PLATFORMS, _OUTER])

def _macos_user_impl(_settings, _attr):
    return {_PLATFORMS: [str(Label(MACOS_USER_PLATFORM))]}

macos_user_transition = transition(implementation = _macos_user_impl, inputs = [], outputs = [_PLATFORMS])
