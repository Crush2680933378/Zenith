# 把 nInfer-all 源码树改成可编 MSVC 插件（幂等，可重复执行）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\apply-ninfer-all-msvc.ps1 -Src C:\path\to\ninfer-all-src
#
# 改两处：
#   1) src/ops/sparse_moe/streamed_cpu_avx2.cpp —— MSVC 不声明标量 _cvtsh_ss，
#      换成向量 F16C 形式（与该仓库 gguf_expert_cpu_avx2.cpp 里已有的写法一致）；
#   2) 根 CMakeLists.txt 末尾追加 add_subdirectory(zenith_native_abi)，
#      并生成该子目录：把 Zenith 的 C ABI shim + 插件外壳编成 DLL。

param(
    [Parameter(Mandatory = $true)][string]$Src,
    [string]$ZenithRepo = "C:/Users/26809/Desktop/Code/c++/Zenith",
    [string]$PluginOut = "C:/Users/26809/Desktop/Code/c++/Zenith/build/m9/plugins-native"
)

$ErrorActionPreference = "Stop"

# ---- 1) F16C intrinsic 的 MSVC 写法 ----------------------------------------
$f = Join-Path $Src "src/ops/sparse_moe/streamed_cpu_avx2.cpp"
if (-not (Test-Path $f)) { throw "找不到 $f" }
$c = Get-Content -LiteralPath $f -Raw -Encoding utf8
if ($c -notmatch 'MSVC 不声明') {
    $old = 'float half(std::uint16_t bits) noexcept { return _cvtsh_ss(bits); }'
    $new = @'
#if defined(_MSC_VER)
// MSVC 不声明标量 _cvtsh_ss：用向量 F16C 形式（与 gguf_expert_cpu_avx2.cpp 同款）
float half(std::uint16_t bits) noexcept {
    return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(static_cast<int>(bits))));
}
#else
float half(std::uint16_t bits) noexcept { return _cvtsh_ss(bits); }
#endif
'@
    if ($c -notmatch [regex]::Escape($old)) { throw "streamed_cpu_avx2.cpp 里没找到预期的那一行" }
    $c = $c.Replace($old, $new)
    Set-Content -LiteralPath $f -Value $c -Encoding utf8 -NoNewline
    Write-Host "[1/2] 已补 MSVC F16C 写法"
} else {
    Write-Host "[1/2] F16C 补丁已存在，跳过"
}

# ---- 2) 注入插件子目录 ------------------------------------------------------
$inj = Join-Path $Src "zenith_native_abi"
New-Item -ItemType Directory -Force -Path $inj | Out-Null
$cm = @"
# 由 Zenith 注入：把自研 C ABI shim 编成插件 DLL（**MSVC 侧**，与 CUDA 引擎同编译单元）
set(ZENITH_REPO "$ZenithRepo")
set(ZENITH_PLUGIN_OUT "$PluginOut")

add_library(zenith_ai_ninfer_native MODULE
    "`${ZENITH_REPO}/plugins/ai_ninfer_native/ninfer_c_shim.cpp"
    "`${ZENITH_REPO}/plugins/ai_ninfer_native/plugin_entry.cpp")

target_include_directories(zenith_ai_ninfer_native PRIVATE
    "`${ZENITH_REPO}/include"
    "`${ZENITH_REPO}/abi")

target_compile_definitions(zenith_ai_ninfer_native PRIVATE ZENITH_NINFER_SHIM_BUILD=1)
target_link_libraries(zenith_ai_ninfer_native PRIVATE ninfer::engine)

set_target_properties(zenith_ai_ninfer_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "zenith_ai_ninfer_native"
    LIBRARY_OUTPUT_DIRECTORY "`${ZENITH_PLUGIN_OUT}")
"@
Set-Content -LiteralPath (Join-Path $inj "CMakeLists.txt") -Value $cm -Encoding utf8

$rootCm = Join-Path $Src "CMakeLists.txt"
$content = Get-Content -LiteralPath $rootCm -Raw -Encoding utf8
if ($content -notmatch 'zenith_native_abi') {
    Add-Content -LiteralPath $rootCm -Value "`nadd_subdirectory(zenith_native_abi)`n" -Encoding utf8
    Write-Host "[2/2] 已注入 add_subdirectory(zenith_native_abi)"
} else {
    Write-Host "[2/2] 注入已存在，跳过"
}

Write-Host "完成。接下来（在已 vcvars 的环境里）："
Write-Host "  cmake -S `"$Src`" -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release ``"
Write-Host "    -DCMAKE_CUDA_ARCHITECTURES=120a -DNINFER_BUILD_APPS=OFF -DBUILD_TESTING=OFF ``"
Write-Host "    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ``"
Write-Host "    -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF"
Write-Host "  cmake --build <build> --target zenith_ai_ninfer_native --parallel 8"
