#!/usr/bin/env bash
# =============================================================================
# build.sh — 在 ARM64 Linux (aarch64) 上编译 Faiss 并构建 benchmark_sift1m
#
# 产物:
#   build/faiss/build/faiss/libfaiss.a    Faiss 静态库 (CPU, 含 HNSW+SQ8)
#   build/benchmark_sift1m                benchmark 可执行文件
#
# 链接策略:
#   - libfaiss.a 静态链接
#   - libgomp(OpenMP)、libstdc++、libgcc 静态链接 (-static-libgcc/-static-libstdc++)
#   - glibc 保持动态 (避免全静态下的 NSS/dlopen 坑)
#   单个可执行文件依赖仅剩 libc/libm/libpthread，任何 glibc 2.17+ 的 ARM64 Linux
#   都能直接运行, 拷贝到目标机器即可。
#
# 用法:
#   ./build.sh                 # 默认: 检测 OpenBLAS, 并行编译
#   ./build.sh --sve           # 目标机器支持 ARM SVE 时启用 (需 aarch64 且有 SVE)
#   ./build.sh --jobs 8        # 并行编译任务数
#   OPENBLAS_DIR=/opt/openblas ./build.sh   # 使用自定义 OpenBLAS 安装
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
USE_SVE=0
JOBS=""
# 路径策略: 全部相对脚本所在目录 (SCRIPT_DIR)。支持两种目录结构:
#   结构A (项目根):    <dir>/faiss  +  <dir>/benchmark_sift1m.cpp + build.sh
#   结构B (faiss-hnswsq/ 子目录):  build.sh 放在 faiss-hnswsq/ 里,
#                      faiss 在上级目录 -> 自动向上查找
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAISS_SRC="${SCRIPT_DIR}/faiss"
if [[ ! -d "$FAISS_SRC" ]]; then
    FAISS_SRC="$(cd "${SCRIPT_DIR}/.." && pwd)/faiss"
fi
BUILD_DIR="${SCRIPT_DIR}/build"
BENCH_SRC="${SCRIPT_DIR}/benchmark_sift1m.cpp"
if [[ ! -f "$BENCH_SRC" ]]; then
    BENCH_SRC="$(cd "${SCRIPT_DIR}/.." && pwd)/benchmark_sift1m.cpp"
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sve) USE_SVE=1 ;;
        --jobs)
            JOBS="$2"
            shift
            ;;
        --faiss-src)
            FAISS_SRC="$2"
            shift
            ;;
        -h | --help)
            sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            exit 1
            ;;
    esac
    shift
done
JOBS="${JOBS:-$(nproc)}"

# ---------------------------------------------------------------------------
# 环境检查
# ---------------------------------------------------------------------------
echo "==> 检查构建环境"
for tool in cmake g++ make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "缺少工具: $tool" >&2
        echo "请先安装 (二选一):" >&2
        echo "  Ubuntu/Debian: sudo apt install cmake g++ make ninja-build" >&2
        echo "  openEuler/CentOS/Fedora: sudo dnf install cmake gcc-c++ make ninja-build" >&2
        exit 1
    fi
done

# faiss 1.15 要求 CMake >= 3.22。openEuler 22.03 自带 3.20, 需要单独升级:
#   sudo dnf install cmake   (24.03+ 自带 3.26+)
#   或 pip install cmake --user  (任意版本都可用)
CMAKE_MAJOR_MINOR="$(cmake --version | head -1 | grep -oP 'cmake version \K[0-9]+\.[0-9]+')"
if [[ "$(echo "$CMAKE_MAJOR_MINOR" | awk -F. '{print $1*100+$2}')" -lt 322 ]]; then
    echo "错误: 需要 CMake >= 3.22, 当前是 $CMAKE_MAJOR_MINOR" >&2
    echo "  openEuler 22.03: sudo dnf install cmake 或 pip install cmake --user" >&2
    echo "  openEuler 24.03+: sudo dnf install cmake" >&2
    exit 1
fi

# 这个版本的 faiss CMake 强制要求 BLAS/LAPACK (find_package(... REQUIRED))
# 因此 OpenBLAS 是硬依赖, 不是可选项。
# 可通过环境变量 OPENBLAS_DIR 指向自定义 OpenBLAS 安装目录
# (例如源码编译到 /opt/openblas 时:  OPENBLAS_DIR=/opt/openblas ./build.sh)。
if [[ -n "${OPENBLAS_DIR:-}" && -d "$OPENBLAS_DIR" ]]; then
    echo "    OpenBLAS: 使用自定义目录 $OPENBLAS_DIR"
elif ldconfig -p 2>/dev/null | grep -q "libopenblas.so"; then
    echo "    OpenBLAS: 已找到 (系统)"
else
    echo "错误: 未检测到 OpenBLAS。本版本 faiss 构建强制需要 BLAS。" >&2
    echo "      安装: sudo apt install libopenblas-dev" >&2
    echo "      或源码编译后设置: OPENBLAS_DIR=/path/to/openblas ./build.sh" >&2
    exit 1
fi

ARCH="$(uname -m)"
case "$ARCH" in
    aarch64 | arm64)
        echo "    检测到 ARM64 架构: $ARCH"
        ;;
    *)
        echo "警告: 当前架构是 $ARCH, 不是 aarch64。脚本会尝试编译,"
        echo "      但产物仅供本机测试, 需在 ARM64 机器上重新编译。"
        ;;
esac

# ---------------------------------------------------------------------------
# 配置 faiss
# ---------------------------------------------------------------------------
FAISS_BUILD="${BUILD_DIR}/faiss"
mkdir -p "${FAISS_BUILD}"
cd "${FAISS_BUILD}"

echo "==> 配置 faiss (BLAS=ON, SVE=$USE_SVE)"

# FAISS_OPT_LEVEL 默认就是 "generic": ARM64 上自动启用 NEON,
# 且不构建 SVE/AVX 专用目标, 生成的库在全部 aarch64 机器上可运行。
CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DFAISS_ENABLE_GPU=OFF
    -DFAISS_ENABLE_PYTHON=OFF
    -DFAISS_ENABLE_C_API=OFF
    # 重要: EXTRAS 含 demos/benchs, 其中 bench_ivf_selector 带 x86 专属
    # 编译选项 (-mavx512 等), 在 ARM 上编译会失败, 必须关闭。
    -DFAISS_ENABLE_EXTRAS=OFF
    -DFAISS_ENABLE_MKL=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

# 自定义 OpenBLAS 目录时直接告知 CMake 链接位置 (faiss 支持
# BLAS_LIBRARIES/LAPACK_LIBRARIES 用户提供模式)
if [[ -n "${OPENBLAS_DIR:-}" && -d "$OPENBLAS_DIR" ]]; then
    OPENBLAS_LIB_PATH="$(find "${OPENBLAS_DIR}" -name "libopenblas.a" | head -1)"
    if [[ -z "$OPENBLAS_LIB_PATH" ]]; then
        OPENBLAS_LIB_PATH="$(find "${OPENBLAS_DIR}" -name "libopenblas.so*" | head -1)"
    fi
    if [[ -n "$OPENBLAS_LIB_PATH" ]]; then
        CMAKE_ARGS+=(-DBLAS_LIBRARIES="${OPENBLAS_LIB_PATH}")
        CMAKE_ARGS+=(-DLAPACK_LIBRARIES="${OPENBLAS_LIB_PATH}")
        echo "    将使用: ${OPENBLAS_LIB_PATH}"
    else
        echo "错误: OPENBLAS_DIR 下找不到 libopenblas 库" >&2
        exit 1
    fi
fi

# SVE 支持: faiss 1.15 通过 -march 探测 SVE。
# 注意: --sve 编译的二进制只能在支持 SVE 的机器上运行 (如部分鲲鹏)。
if [[ "$USE_SVE" -eq 1 ]]; then
    CMAKE_ARGS+=(-DFAISS_OPT_LEVEL=sve)
    CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="-march=armv8-a+sve")
fi

cmake -S "${FAISS_SRC}" -B "${FAISS_BUILD}" "${CMAKE_ARGS[@]}"

# ---------------------------------------------------------------------------
# 编译 faiss 静态库
# ---------------------------------------------------------------------------
echo "==> 编译 faiss (${JOBS} jobs)"
cmake --build "${FAISS_BUILD}" --target faiss -j "${JOBS}"

# ---------------------------------------------------------------------------
# 编译 benchmark_sift1m
# ---------------------------------------------------------------------------
echo "==> 编译 benchmark_sift1m"
LIBFAISS="$(find "${FAISS_BUILD}" -name "libfaiss.a" | head -1)"
if [[ -z "$LIBFAISS" ]]; then
    echo "找不到 libfaiss.a" >&2
    exit 1
fi

# 收集链接依赖: libfaiss.a 中 BLAS/LAPACK 符号 (cblas_*/lapacke_*) 是
# PRIVATE 链接带出的, 最终链接必须显式补上。
#
# 优先用静态 libopenblas.a (libopenblas-dev 提供), 产物完全不依赖
# 目标机器的 OpenBLAS 安装; 找不到则退回动态 libopenblas.so。
LINK_LIBS=("${LIBFAISS}")
OPENBLAS_DEV_LIB=""
if [[ -n "${OPENBLAS_DIR:-}" ]]; then
    OPENBLAS_DEV_LIB="$(find "${OPENBLAS_DIR}" -name "libopenblas.a" 2>/dev/null | head -1)"
fi
if [[ -z "$OPENBLAS_DEV_LIB" ]]; then
    # RPM 系 (openEuler/CentOS/Fedora) 的 64 位库在 /usr/lib64
    for dir in /usr/lib64 /usr/lib/aarch64-linux-gnu /usr/lib/arm64-linux-gnu \
        /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu; do
        if [[ -f "${dir}/libopenblas.a" ]]; then
            OPENBLAS_DEV_LIB="${dir}/libopenblas.a"
            break
        fi
    done
fi
if [[ -n "$OPENBLAS_DEV_LIB" ]]; then
    echo "    静态链接 OpenBLAS: ${OPENBLAS_DEV_LIB}"
    LINK_LIBS+=("${OPENBLAS_DEV_LIB}")
else
    echo "    未找到 libopenblas.a, 回退动态链接 libopenblas.so"
    LINK_LIBS+=(-lopenblas)
fi

g++ -O3 -std=c++17 \
    -I "${FAISS_SRC}" \
    "${BENCH_SRC}" \
    "${LINK_LIBS[@]}" \
    -fopenmp -static-libgcc -static-libstdc++ \
    -lpthread -lm -ldl \
    -o "${BUILD_DIR}/benchmark_sift1m"

echo
echo "============================================================"
echo " 编译完成"
echo "   可执行文件: ${BUILD_DIR}/benchmark_sift1m"
echo "   静态库:     ${LIBFAISS}"
echo
echo " 用法示例:"
echo "   ./build/benchmark_sift1m --dataset /path/to/sift1M \\"
echo "       --sq SQ8 --M 16 --ef-construction 100 \\"
echo "       --ef-search 10,20,40,80,160 --top-k 10"
echo
echo " 依赖检查 (应为空或仅剩系统基础库):"
echo "   ldd ${BUILD_DIR}/benchmark_sift1m"
echo "============================================================"
