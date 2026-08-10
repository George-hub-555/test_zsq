# Faiss HNSWFlat：SIFT1M QPS–Recall

本目录只测试原版 Faiss `IndexHNSWFlat`，不会修改 `../faiss` 中的源码，也不包含 ZSQ。

默认参数与 Falcon 现有 RaBitQ/ZSQ 相关测试的图参数保持一致：

- `topK = 10`
- `M = 16`
- `efConstruction = 100`
- `efSearch = 10,15,20,30,40,50,75,100,150,200,300,400,800`
- 单线程搜索、1000 条预热查询、5 次正式搜索，QPS 取中位数
- Recall 定义为返回 top-10 与 ground truth top-10 的交集数除以 `nq * 10`

计时区间仅包含 `IndexHNSWFlat::search`。数据读取、建图、结果数组分配、预热和 Recall 计算均不计入 QPS。

## 数据目录

将 SIFT1M 文件放到 `faiss-zsq/dataset`：

```text
dataset/
  sift_base.fvecs
  sift_query.fvecs
  sift_groundtruth.ivecs
```

程序会使用全部 base 和 query。可通过 `--query-count` 限制查询数，但正式对比时 Falcon 与 Faiss 必须使用相同的 query 集合和数量。

## 在 AArch64 机器构建

需要 CMake 3.24+、C++20 编译器、OpenMP、BLAS 和 LAPACK。不要使用 `-march=native`；当前 CMake 固定构建 Faiss 的 `generic` CPU 版本，便于在另一台 AArch64 机器运行。

```bash
cd /path/to/Faiss-zsq/faiss-zsq
cmake -S . -B build-aarch64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBLA_VENDOR=OpenBLAS
cmake --build build-aarch64 -j
```

本项目未在 x86 本机进行编译或测试。构建和运行应在目标 AArch64 环境完成。

## 执行 benchmark

默认命令已经包含上述 13 个 efSearch 点：

```bash
./build-aarch64/faiss_hnsw_sift1m \
  --dataset ./dataset \
  --output-dir ./results
```

如 Falcon 的测量使用多线程，Faiss 必须显式使用相同线程数，例如：

```bash
./build-aarch64/faiss_hnsw_sift1m \
  --dataset ./dataset \
  --output-dir ./results \
  --threads 8
```

结果文件：

- `results/faiss_hnsw_raw.csv`：每个 efSearch 的 5 次原始结果
- `results/faiss_hnsw_summary.csv`：每个 efSearch 的中位 QPS 和 Recall@10

## 绘图

目标机或其他装有 Python 与 matplotlib 的机器均可绘图：

```bash
python3 plot_qps_recall.py \
  --input results/faiss_hnsw_summary.csv \
  --output results/faiss_hnsw_qps_recall.png
```

纵轴默认使用对数刻度；需要线性纵轴时增加 `--linear-y`。

## 可覆盖参数

```bash
./build-aarch64/faiss_hnsw_sift1m --help
```

其中 `--ef-search` 接受逗号分隔列表。为了与本次 Falcon 曲线可比，不建议改动默认的 `topK`、`M`、`efConstruction` 或 efSearch 列表。
