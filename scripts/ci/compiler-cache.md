# Optional MSVC compile cache

CI uses the runner's installed MSVC through `msvc-dev-cmd` and `Ninja Multi-Config`. It does not require a particular Visual Studio release or a developer preset. The x86 job uses `amd64_x86`; x64 uses the native environment.

Dot-source `compiler-cache.ps1`, then pass the array returned by `Get-MsimeCompilerCacheArguments -BuildDirectory <build> -Enable` to CMake. Missing sccache/Python, failed setup or failed service startup return empty compiler launchers. A cache failure during compilation disables it for the current build and reruns the original compiler with the same argv. Real compiler errors still fail. Only optional cache setup has `continue-on-error`; configure, compile, tests and package verification remain required.

`Show-MsimeCompilerCacheStats` belongs in a separate reporting step. Reconfigure clears a previous cache-disable flag. Visual Studio generators receive no launcher because they do not use this CMake hook.

From a Visual Studio developer shell, standalone Engine builds can use `scripts/build-engine.ps1 -VcpkgRoot <registry> -UseCompilerCache`. Omit the switch for ordinary MSVC. Select the registry explicitly; the script does not download model weights.

`select-vcpkg.ps1` reads `VCPKG_INSTALLATION_ROOT` on GitHub-hosted runners and preserves the configured per-job registry on self-hosted runners. It verifies both the executable and toolchain, then persists the same root for installer/configure steps. SignPath release jobs use hosted Windows; certificate and unsigned modes keep the existing self-hosted labels. This routing does not grant SignPath eligibility or configure signing credentials.

Regression entry points: `test_compile_cache_launcher.py`, `Test-CompilerCache.ps1`, `Test-RunnerVcpkg.ps1`, and `Test-CompileCacheNative.ps1`. The last requires a real MSVC developer environment and Ninja, compiles a Unicode/spaced source path through a deliberately failing cache, runs the resulting binary, and checks a subsequent incremental compile bypasses the failed cache.
