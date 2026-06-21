Write-Host "Set TLS1.2"
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor "Tls12"
choco install -y nsis.portable --ignore-checksums &&
./scripts/winflexbison_install.ps1

od.exe --version &&

vcpkg.exe integrate install &&
vcpkg.exe install boost-filesystem:x64-windows-static boost-multiprecision:x64-windows-static boost-date-time:x64-windows-static boost-test:x64-windows-static boost-multi-index:x64-windows-static boost-crc:x64-windows-static boost-property-tree:x64-windows-static boost-uuid:x64-windows-static boost-program-options:x64-windows-static boost-iostreams:x64-windows-static boost-process:x64-windows-static

if (-not $?) { exit 1 }

# build/ may already exist when CI restores the cached LLVM/Z3 into it, so create
# it idempotently rather than failing.
New-Item -ItemType Directory -Force -Path build | Out-Null
cd build

if (-not $?) { exit 1 }

# Common cmake configure arguments. LLVM/Z3 are downloaded+extracted into the
# build dir (build/LLVM, build/Z3) by DOWNLOAD_DEPENDENCIES; CI caches those so
# the ~1.5GB fetch happens once.
$cmakeArgs = @(
  '..',
  '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
  '-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake',
  '-DENABLE_REGRESSION=On',
  '-DBUILD_TESTING=On',
  '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
  '-DCMAKE_INSTALL_PREFIX:PATH=C:/deps/esbmc',
  '-DENABLE_PYTHON_FRONTEND=On',
  '-DENABLE_SOLIDITY_FRONTEND=On',
  '-DENABLE_JIMPLE_FRONTEND=On',
  '-DDOWNLOAD_DEPENDENCIES=On',
  '-DENABLE_Z3=ON',
  '-DENABLE_SMTLIB=OFF'
)

# Generator selection.
#   Default (local): Visual Studio generator via "-A x64" (unchanged behaviour).
#   CI/sccache:      set ESBMC_USE_NINJA=1 to use the Ninja generator, which is
#                    the only way CMAKE_<LANG>_COMPILER_LAUNCHER (sccache) is
#                    honoured on Windows. sccache cannot cache /Zi (separate PDB)
#                    objects, so we force embedded debug info (/Z7) via
#                    CMAKE_MSVC_DEBUG_INFORMATION_FORMAT (needs CMP0141=NEW).
if ($env:ESBMC_USE_NINJA -eq '1') {
  $cmakeArgs += '-GNinja'
  $cmakeArgs += '-DCMAKE_POLICY_DEFAULT_CMP0141=NEW'
  $cmakeArgs += '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded'
} else {
  $cmakeArgs += '-A'
  $cmakeArgs += 'x64'
}

# Extra args forwarded from the environment (e.g. the sccache launcher in CI).
# Whitespace-separated, mirrors the Linux build.sh EXTRA_CMAKE_ARGS pattern.
if ($env:ESBMC_EXTRA_CMAKE_ARGS) {
  $cmakeArgs += ($env:ESBMC_EXTRA_CMAKE_ARGS -split '\s+' | Where-Object { $_ })
}

Write-Host "cmake.exe $($cmakeArgs -join ' ')"
cmake.exe @cmakeArgs

if (-not $?) { exit 1 }

cmake --build . --target INSTALL --config RelWithDebInfo
