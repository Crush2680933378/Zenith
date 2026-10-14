# =============================================================================
#  aarch64 交叉编译工具链（泰山派 RK3566 / RK3576 等 ARM64 Linux 板卡）
#  用法：
#    cmake --preset linux-gcc-debug \
#          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
#          -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
#          -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
#  真机联调（edge 插件、板卡外设）在 M4 里程碑进行。
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 编译器可由命令行覆盖；这里给出发行版常用命名
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc   CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++   CACHE FILEPATH "C++ compiler")

# 仅在目标 sysroot 中查找库与头（宿主工具仍从 PATH 找）
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
