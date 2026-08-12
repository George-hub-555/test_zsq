# Faiss HNSW-SQ 基准测试（ARM64 Linux 编译指南）

在 ARM64 Linux 机器上编译 [Faiss 1.15.0](faiss/)（CPU 版，静态库），并构建
`benchmark_sift1m` 基准程序，使用 **HNSW + SQ8 标量量化**索引在 SIFT1M 数据集上
测量召回率与查询吞吐（QPS）。

## 目录结构

```
Faiss-hnsw-sq/
├── faiss/                   # Faiss 1.15.0 源码
├── benchmark_sift1m.cpp     # 基准程序（HNSW + SQ，默认 SQ8）
├── build.sh                 # ARM64 编译脚本
└── README.md
```

## 一、准备环境（ARM64 Linux）

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build libopenblas-dev
```

依赖说明：

- **libopenblas-dev**：必须。Faiss 1.15 的 CMake 强制 `find_package(BLAS REQUIRED)`，
  没有 OpenBLAS 无法配置。装 dev 包还会提供 `libopenblas.a`，可让最终可执行文件
  完全不依赖目标机器的 OpenBLAS 安装。
- **build-essential**：g++（要求支持 C++17 和 OpenMP，GCC 8+ 即可）。

## 二、编译

```bash
chmod +x build.sh
./build.sh            # 等价于 ./build.sh --jobs $(nproc)
```

脚本做的事：

1. CMake 配置 faiss：Release、GPU/ROCm/Python/C-API 全关、静态库
   （`BUILD_SHARED_LIBS=OFF`）、`FAISS_OPT_LEVEL=generic`。
   - `generic` 是默认值：在 aarch64 上自动启用 **NEON** 向量化（aarch64 强制支持），
     不构建 SVE 专用目标，产物可在**所有** ARM64 机器（树莓派 4/5、飞腾、鲲鹏、
     Apple Silicon 等）上运行。
2. `cmake --build --target faiss` 编译 `libfaiss.a`。
3. 用 g++ 直接编译 `benchmark_sift1m.cpp`，链接：
   - `libfaiss.a`（静态）
   - `libopenblas.a`（静态，找不到则退回动态 `-lopenblas`）
   - `libgomp` / `libstdc++` / `libgcc`（静态）
   - `libc` 等 glibc 基础库（动态，避免全静态导致的 NSS/dlopen 问题）

### 可选参数

| 参数 | 说明 |
| --- | --- |
| `--jobs N` | 并行编译任务数，默认 `nproc` |
| `--sve` | 启用 ARM SVE 指令集（`-DFAISS_OPT_LEVEL=sve`）。仅当目标机器支持 SVE 时使用（如部分鲲鹏 920），**编译出的二进制不能在不支持 SVE 的机器上运行** |
| `--faiss-src PATH` | 指定 faiss 源码目录（默认 `./faiss`） |

### 产物

```
build/
├── benchmark_sift1m          # 可执行文件
└── faiss/faiss/libfaiss.a    # Faiss 静态库（含 IndexHNSWSQ 实现）
```

验证产物依赖（理想情况只显示 glibc 基础库和 libgomp/libstdc++ 已静态化）：

```bash
ldd build/benchmark_sift1m
```

## 三、准备 SIFT1M 数据集

从 [ANN_SIFT1M](http://corpus-texmex.irisa.fr/) 下载（约 600 MB）：

```bash
mkdir -p sift1M && cd sift1M
wget ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz
tar xzf sift.tar.gz
cd ..
```

需要的三个文件（放在同一个 `sift1M/` 目录）：

- `sift_base.fvecs`（1M 条 128 维 float 向量）
- `sift_query.fvecs`（1 万条查询向量）
- `sift_groundtruth.ivecs`（ground truth）

> 注意：程序内部按 `dataset/sift_base.fvecs`、`dataset/sift_query.fvecs`、
> `dataset/sift_groundtruth.ivecs` 三个路径查找，注意大小写。

## 四、运行基准

```bash
./build/benchmark_sift1m --dataset sift1M --sq SQ8 \
    --M 16 --ef-construction 100 \
    --ef-search 10,20,40,80,160,320 --top-k 10 --threads 1
```

### 全部命令行参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--dataset PATH` | `dataset` | SIFT1M 数据目录 |
| `--output-dir PATH` | `results` | CSV 输出目录 |
| `--sq NAME` | `SQ8` | 量化方式：`SQ8`/`SQ6`/`SQ4`/`SQfp16`/`SQbf16`（`Flat` 禁用量化） |
| `--M N` | `16` | HNSW 每节点最大度数 |
| `--ef-construction N` | `100` | 建图搜索宽度 |
| `--ef-search LIST` | 一串默认值 | 逗号分隔的 efSearch 列表，逐个测量 |
| `--top-k N` | `10` | 返回近邻数（用于 recall@k 计算） |
| `--repeats N` | `5` | 每个 efSearch 重复查询次数，取 QPS 中位数 |
| `--warmup-queries N` | `1000` | 预热查询数（不计入统计） |
| `--query-count N` | `0` | 查询数量，0 表示全部 1 万条 |
| `--threads N` | `1` | OpenMP 线程数 |

### 输出

```
results/
├── faiss_hnsw_raw.csv       # 每次重复的原始记录
└── faiss_hnsw_summary.csv   # 每个 efSearch 的中位数 QPS 和召回率
```

CSV 列：`ef_search, median_qps, top_k, recall_at_k, threads, M,
ef_construction, query_count, repeats, build_seconds, sq`

## 五、基准结果解读

- **recall@k**：返回的 top-k 中与 ground truth top-k 的重合比例（0~1）。
- **QPS**：每秒查询数，`query_count / 搜索耗时`。
- 增大 `ef-search` → 召回率上升、QPS 下降，这是 HNSW 的核心权衡曲线。
- 与 `--sq Flat`（不量化）对比，可量化 **SQ8 的内存节省（1/4）与召回率损失**。

## 六、常见问题

**Q: 构建时报 `Could NOT find BLAS`？**
安装 libopenblas-dev 后重新执行 `./build.sh`。注意若曾配置过，CMake 缓存会残留，
可删除 `build/faiss` 目录后重试。

**Q: 想对比不同量化方式的曲线？**
分别运行 `--sq SQ8` 与 `--sq SQfp16`（无量化损失的 2 字节精度），比较 summary CSV。

**Q: 在树莓派 4（ARMv8.0）上能跑吗？**
能。默认 `generic` 优化仅用 NEON（ARMv8.0 必需指令），产物兼容所有 aarch64。

**Q: 为什么不用全静态链接？**
glibc 全静态会在 `getaddrinfo`/`dlopen` 等路径出问题，faiss 不需要这些功能，但
全静态仍可能触发 glibc 警告；采用 `-static-libgcc -static-libstdc++` + 动态 glibc
是更稳的方案，产物仅依赖 libc/libm/libpthread。
