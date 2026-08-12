# Falcon BlinkGraph：ERQ-9bit 与 ZSQ-8bit 的 ARM64 对照测试

本目录提供一套“两台 ARM64 Linux 机器”流程：A 只负责编译和打包，B 使用 SIFT1M 建索引并压测。测试工程位于 Falcon 仓库外，不修改 Falcon 源码。

## 1. 测试对象与口径

- `erq9`：`BlinkGraphERQBuilder` + `BlinkGraphERQSearcherAdaptive`。
- `zsq`：`BlinkGraphZSQBuilder` + `BlinkGraphZSQSearcherAdaptive`。
- ERQ 配置为基础码 1 bit/维加扩展码 8 bit/维，总向量码为 9 bit/维；ZSQ 使用每维一个 `uint8`，为 8 bit/维。
- 两组维度均为 128，并使用相同 BlinkGraph、旋转器和建图参数；它们分别建库、分别搜索，不会在同一次搜索中混用 ERQ 与 SQ。
- 这是 ERQ-9bit 与 ZSQ-8bit 对比，不是相同位宽对比。ERQ 每条向量还保存少量 float 校正因子，整个索引大小不会只由每维位数决定。
- 距离为 L2。搜索参数在 Falcon 中是 `search_range/ef`，不是 IVF 的 `nprobe`。
- 默认固定：`link_range=32`、`link_candidate_size=300`、`build_iter_count=3`、`batch_size_mb=1024`、矩阵旋转器。
- 默认搜索：`search_range=50,100,200,400`，`topK=1,10,100`，每组预热 1000 条查询并计时 5 轮。程序会跳过 `search_range < topK` 的无效组合。

输出包含建库耗时、索引大小、进程峰值 RSS、QPS、平均延迟、P50/P90/P95/P99 和 Recall@K，并自动合并为一个可直接传回本地绘图的 CSV。

注意：Falcon 当前建图实现使用 `std::random_device` 初始化随机图，未暴露固定随机种子的接口。因此严谨报告建议至少执行 3 次完整建库，每次都保留结果，最终报告中给出中位数与波动范围。为了保持 Falcon 零改动，本测试不会修改其随机数实现。

## 2. 文件分别上传到哪里

### 上传到 A（ARM64 编译机）

把下列目录放成同级结构。后续所有命令都在 `falcon` 的父目录执行：

```text
./
├── falcon/                         # 完整 Falcon 仓库
│   └── devel/builder/
│       └── bazel-7.4.1-linux-arm64
└── test_zsq/
    ├── BUILD
    ├── zsq_benchmark.cpp
    ├── build_on_a.sh
    ├── run_on_b.sh
    ├── extract_zsq_results.py
    ├── plot_qps_recall_svg.py
    └── README.md
```

A 不需要上传 SIFT 数据。Bazel 依赖、子模块或 Falcon 构建所需的第三方文件必须与能正常构建 Falcon 的仓库状态一致。

### 上传到 B（ARM64 测试机）

B 只需要：

1. A 生成的 `zsq_arm64_bundle.tar.gz`；
2. A 生成的 `zsq_arm64_bundle.tar.gz.sha256`；
3. SIFT1M 的三个原始二进制文件：

```text
./falcon/dataset/
├── sift_base.fvecs                 # 1,000,000 x 128
├── sift_query.fvecs                # 10,000 x 128
└── sift_groundtruth.ivecs          # 10,000 x 100
```

`sift_learn.fvecs` 不需要上传：当前 Falcon ZSQ builder 直接从 base 向量计算每维最小值/最大值，没有使用独立训练集。

## 3. A 机：编译与打包

### 3.1 前置检查

```bash
uname -m
sed -i 's/\r$//' test_zsq/build_on_a.sh test_zsq/run_on_b.sh
chmod +x falcon/devel/builder/bazel-7.4.1-linux-arm64
chmod +x test_zsq/build_on_a.sh
```

`test_zsq/*.sh` 必须使用 Unix LF 换行。上面的 `sed` 命令可重复执行，用于清除 Windows 上传或 Git 配置可能引入的 CRLF；否则 Bash 可能报 `pipefail` 是非法选项。

`uname -m` 应输出 `aarch64` 或 `arm64`。Falcon 的 `linux_arm64` 配置启用了 `armv8.2-a+crypto+crc+dotprod`，所以 B 的 CPU 也必须支持相应指令，重点检查 `asimddp/dotprod`。

### 3.2 执行编译

保持终端位于 `falcon` 的父目录，直接执行：

```bash
bash test_zsq/build_on_a.sh
```

脚本实际执行的 Bazel 目标是：

```bash
cd falcon
devel/builder/bazel-7.4.1-linux-arm64 build \
  --config=linux_arm64 \
  -c opt \
  --package_path='%workspace%:%workspace%/..' \
  //test_zsq:zsq_benchmark
```

`--package_path` 让 Bazel 从 Falcon 的父目录发现同级 `test_zsq`，不需要在 Falcon 内新增 BUILD 或源文件。

### 3.3 A 生成什么

脚本生成：

```text
./zsq_arm64_bundle.tar.gz
./zsq_arm64_bundle.tar.gz.sha256
```

压缩包内部包含：

```text
zsq_arm64_bundle/
├── bin/zsq_benchmark
├── lib/                             # ldd 收集的非 glibc 动态库
├── run_on_b.sh
├── extract_zsq_results.py           # B 上自动生成单一综合 CSV
├── README.md
└── manifest.txt                     # Falcon commit、编译器、Bazel、glibc、文件哈希
```

把上面两个文件原样传给 B。不要只传裸二进制，否则可能遗漏 Bazel `_solib_aarch64` 中解析到的依赖库。

## 4. B 机：校验、建索引、搜索

### 4.1 解压与环境检查

```bash
sha256sum -c zsq_arm64_bundle.tar.gz.sha256
tar -xzf zsq_arm64_bundle.tar.gz

uname -m
lscpu | grep -Ei 'asimddp|dotprod'
ldd --version | head -n 1
chmod +x zsq_arm64_bundle/run_on_b.sh zsq_arm64_bundle/bin/zsq_benchmark
```

要求：

- 架构是 AArch64；
- CPU 支持构建参数所需指令；
- B 的 glibc 不低于 A 编译时使用的 glibc。压缩包会携带 `libstdc++`、`libgcc_s` 等非 glibc 库，但不会替换系统 glibc；最稳妥做法是 A 与 B 使用相同发行版，或 A 使用不新于 B 的发行版。

可先检查动态库：

```bash
LD_LIBRARY_PATH="$PWD/zsq_arm64_bundle/lib" ldd zsq_arm64_bundle/bin/zsq_benchmark
```

输出中不能出现 `not found`。

### 4.2 一键运行

```bash
bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_runs_erq9
```

该命令依次完成：

1. 校验三个 SIFT 文件的字节数、记录数和抽样维度；
2. 分别构建 `erq9.index` 与 `zsq.index`；
3. 分别加载索引并完成搜索参数扫描；
4. 保存环境、日志、CSV、`/usr/bin/time -v` 输出及结果哈希；
5. 自动刷新 `Sum/zsq_key_metrics.csv`，该文件已经包含本地绘图所需的全部数据。

每次运行自动创建新目录，不覆盖旧结果：

```text
./zsq_runs_erq9/
├── zsq_run_YYYYMMDDTHHMMSSZ/
│   ├── indexes/
│   │   ├── erq9.index
│   │   └── zsq.index
│   ├── results/
│   │   ├── environment.txt
│   │   ├── validate.log
│   │   ├── build_erq9.csv
│   │   ├── build_zsq.csv
│   │   ├── search_erq9.csv
│   │   ├── search_zsq.csv
│   │   ├── build_*.log
│   │   ├── search_*.log
│   │   └── *.time.txt
│   └── SHA256SUMS
└── Sum/zsq_key_metrics.csv           # 传回本地只需这个文件
```

### 4.3 推荐固定 CPU 核

如果 B 安装了 `taskset`，可以将整个测试固定在同一组物理核上。建库线程数应与核数一致：

```bash
CPUSET=0-15 THREAD_COUNT=16 \
  bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_runs_erq9
```

搜索 benchmark 当前为单线程逐查询计时；固定到多核集合仍允许操作系统迁核。如要测最稳定的单线程延迟，可使用 `CPUSET=0`，但建库也会被限制到单核。更推荐分两次手动执行，或先以默认流程完成整体对比。

脚本只读取 CPU governor，不会改 governor、清空 page cache 或修改系统状态。

## 5. 参数覆盖与反向顺序复测

所有关键参数都可通过环境变量覆盖。例如：

```bash
THREAD_COUNT=32 \
SEARCH_RANGES=100,200,400,800 \
TOP_KS=10,100 \
WARMUP_QUERIES=1000 \
ROUNDS=10 \
  bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_runs_erq9
```

为减小温度、缓存和执行顺序造成的偏差，第二轮可反转顺序：

```bash
BUILD_ORDER=zsq,erq9 SEARCH_ORDER=zsq,erq9 \
  bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_runs_erq9
```

建议至少做三次完整运行，奇数次使用 `erq9,zsq`，偶数次使用 `zsq,erq9`。每次结束都会把当前全部有效运行重新聚合到同一个综合 CSV。每次建出的图本身也可能因 Falcon 随机初始化而不同。

可覆盖的变量：

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `BASE_COUNT` | 1000000 | base 向量数 |
| `QUERY_COUNT` | 10000 | query 数 |
| `DIM` | 128 | 向量维度 |
| `GROUNDTRUTH_K` | 100 | 每条 GT 的 ID 数 |
| `THREAD_COUNT` | 16 | 建库 OpenMP 线程数 |
| `LINK_RANGE` | 32 | 图出度，必须为 32 的正整数倍 |
| `LINK_CANDIDATE_SIZE` | 300 | 建图候选数 |
| `BUILD_ITER_COUNT` | 3 | 建图轮数 |
| `BATCH_SIZE_MB` | 1024 | adaptive RaBitQ batch 预算，不能为 0 |
| `ROTATOR_TYPE` | 0 | 0=matrix，1=FHT/Kac |
| `SEARCH_RANGES` | 50,100,200,400 | `ef/search_range` 扫描值 |
| `TOP_KS` | 1,10,100 | Recall@K 与返回条数 |
| `WARMUP_QUERIES` | 1000 | 每组参数预热查询数 |
| `ROUNDS` | 5 | 每组计时轮数 |
| `CPUSET` | 空 | 可选 taskset CPU 列表 |
| `BUILD_ORDER` | erq9,zsq | 建库顺序 |
| `SEARCH_ORDER` | erq9,zsq | 搜索顺序 |

## 6. 如何比较结果

先确认同一轮中的通用配置列完全一致，并确认 ERQ 行的 `code_bits_per_dim=9`、ZSQ 行的 `code_bits_per_dim=8`，再比较：

1. `build_*.csv`：`build_seconds`、`save_seconds`、`index_bytes`、`max_rss_kib`；
2. `search_*.csv`：在相同 `top_k + search_range` 下比较 `recall_at_k`、`qps`、`avg_us` 和 P99；
3. `search_*.time.txt`：用外部 `/usr/bin/time -v` 的 `Maximum resident set size` 交叉检查进程峰值内存；
4. 不要只比较速度：应按相同召回水平选择不同 `search_range` 后再比较速度。

Recall@K 的定义是：返回结果去重后，与 ground truth 前 K 个 ID 的交集大小除以 K，再对全部 query 汇总。计时区间只包围 Falcon 的 `Search()` 调用，召回统计不计入搜索耗时。

## 7. 单一 CSV 与本地绘图

B 每次完整运行结束后都会自动生成或刷新：

```text
zsq_runs_erq9/Sum/zsq_key_metrics.csv
```

该 CSV 同时包含：

- `scope=run`：每一轮、每个量化方案、每个 `top_k + search_range` 的原始测试点；
- `scope=aggregate`：跨轮中位数、QPS/Recall 最小值和最大值；
- QPS、Recall、平均/P50/P90/P95/P99 延迟；
- 建库和保存耗时、索引大小、搜索/建库峰值 RSS；
- 维度、线程、图参数、轮数、预热数；
- `quantizer`、`base_bit_len`、`ext_bit_len`、`code_bits_per_dim`。

因此从 B 传回本地时，只需复制：

```text
zsq_runs_erq9/Sum/zsq_key_metrics.csv
```

本地使用 Python 3.8+ 和 `test_zsq/plot_qps_recall_svg.py` 作图，不依赖第三方库：

```bash
python3 test_zsq/plot_qps_recall_svg.py \
  --csv zsq_key_metrics.csv \
  --output-dir Result/Sum
```

提取脚本仍兼容旧的 `rbq + zsq` 结果，但同一个输入目录不允许同时包含旧 RBQ 完整运行和新 ERQ9 完整运行；两代结果必须放在不同目录分别聚合。

如需手动重新生成综合 CSV：

```bash
python3 zsq_arm64_bundle/extract_zsq_results.py \
  --input zsq_runs_erq9 \
  --output zsq_runs_erq9/Sum/zsq_key_metrics.csv
```

## 8. 故障排查

### Bash 报 `invalid option name` 或错误中出现 `pipefail`

这是脚本被转换成 Windows CRLF 换行导致的。在 `falcon` 父目录执行：

```bash
sed -i 's/\r$//' test_zsq/build_on_a.sh test_zsq/run_on_b.sh
chmod +x test_zsq/build_on_a.sh
bash test_zsq/build_on_a.sh
```

### A 上找不到 `//test_zsq:zsq_benchmark`

确认 `falcon` 与 `test_zsq` 是同级目录，并保留：

```bash
--package_path='%workspace%:%workspace%/..'
```

### B 上提示 `GLIBC_x.y not found`

A 的系统比 B 新。请换成与 B 相同或更旧的 ARM64 编译环境重新打包；不要自行复制 `libc.so.6` 覆盖 B 的 glibc。

### B 上出现 `Illegal instruction`

优先检查 B 是否支持 `armv8.2-a` 和 dot-product 指令。当前构建参数来自 Falcon `.bazelrc` 的 `linux_arm64` 配置。

### 内存不足

SIFT1M 建图库会同时持有原始向量和建图中间结构，实际峰值显著高于 512 MB 数据文件。先查看 `build_*.time.txt`。不要通过降低 `BATCH_SIZE_MB` 后只改一组；任何通用参数调整必须对 ERQ 和 ZSQ 同时生效。

### 结果目录已存在或文件已存在

benchmark 主动拒绝覆盖索引和 CSV。重新运行 `run_on_b.sh` 会生成新的 UTC 时间戳目录。


# 单测zsq

```
THREAD_COUNT=32 LINK_RANGE=32 LINK_CANDIDATE_SIZE=300 BUILD_ITER_COUNT=3 SEARCH_RANGES=10,15,20,30,40,50,75,100,150,200,300,400,800 TOP_KS=10 WARMUP_QUERIES=1000 ROUNDS=5 bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_top10_l32_c300_i3
```

```
BUILD_ORDER=zsq SEARCH_ORDER=zsq THREAD_COUNT=32 LINK_RANGE=64 LINK_CANDIDATE_SIZE=600 BUILD_ITER_COUNT=5 SEARCH_RANGES=10,15,20,30,40,50,75,100,150,200,300,400,800 TOP_KS=10 WARMUP_QUERIES=1000 ROUNDS=5 bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_top10_l32_c300_i3
```


# 修改距离计算方式
可以。如果你指的是只把修改后的源码文件传到 ARM 编译机，可以只替换这一份：

```text
falcon/common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec/blink_graph_zsq_searcher_adaptive.cpp
```

然后在 ARM 编译机重新执行：

```bash
bash test_zsq/build_on_a.sh
```

Bazel会重新编译受该文件影响的目标，并生成新的测试包。

如果你指的是测试机，则不能只替换 `.cpp`。测试机至少要替换重新编译生成的：

```text
zsq_arm64_bundle/bin/zsq_benchmark
```

若编译环境和测试机依赖环境与上次完全一致，通常只替换这个二进制即可：

```bash
cp 新包/zsq_arm64_bundle/bin/zsq_benchmark \
   旧包/zsq_arm64_bundle/bin/zsq_benchmark
chmod +x 旧包/zsq_arm64_bundle/bin/zsq_benchmark
```

建议替换后确认新旧二进制哈希不同：

```bash
sha256sum 新包/zsq_arm64_bundle/bin/zsq_benchmark
sha256sum 旧包/zsq_arm64_bundle/bin/zsq_benchmark
```

最稳妥的做法仍然是整体替换新生成的 `zsq_arm64_bundle`，因为如果链接依赖发生变化，只换二进制可能出现动态库不一致。不过本次只是单个C++实现文件的函数体修改，没有新增依赖、接口或索引格式，使用相同ARM编译环境时，只替换重新编译后的 `bin/zsq_benchmark` 通常足够。
