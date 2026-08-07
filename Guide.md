已实现：新的测试是独立的 `ERQ-9bit` 对比 `ZSQ-8bit`，未修改 Falcon 源码。

主要改动：

- [zsq_benchmark.cpp](E:/Graduation/GPTproject/FS_zsq/test_zsq/zsq_benchmark.cpp)：ERQ Builder/Searcher、9-bit 配置及 CSV 位宽字段。
- [BUILD](E:/Graduation/GPTproject/FS_zsq/test_zsq/BUILD)：增加 ERQ Bazel 依赖。
- [run_on_b.sh](E:/Graduation/GPTproject/FS_zsq/test_zsq/run_on_b.sh)：默认运行 `erq9,zsq`，测试结束自动生成综合 CSV。
- [extract_zsq_results.py](E:/Graduation/GPTproject/FS_zsq/test_zsq/extract_zsq_results.py)：支持新 ERQ9 和旧 RBQ，但禁止混合聚合。
- [plot_qps_recall_svg.py](E:/Graduation/GPTproject/FS_zsq/test_zsq/plot_qps_recall_svg.py)：显示 `ERQ-9bit` 和 `ZSQ-8bit`。
- [README.md](E:/Graduation/GPTproject/FS_zsq/test_zsq/README.md)：已更新完整上传、运行和绘图说明。
- `build_on_a.sh` 现在会把数据提取脚本一起打包。

A 机重新编译。如果旧压缩包仍存在，建议使用新名字：

```bash
bash test_zsq/build_on_a.sh \
  falcon \
  zsq_arm64_bundle_erq9.tar.gz
```

上传给 B：

```text
zsq_arm64_bundle_erq9.tar.gz
zsq_arm64_bundle_erq9.tar.gz.sha256
```

B 机执行：

```bash
sha256sum -c zsq_arm64_bundle_erq9.tar.gz.sha256
tar -xzf zsq_arm64_bundle_erq9.tar.gz

sed -i '/ldd --version 2>&1 | head -n 1/c\    ldd --version 2>&1' \
  zsq_arm64_bundle/run_on_b.sh

bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_runs
echo "exit_code=$?"

bash zsq_arm64_bundle/run_on_b.sh \
  falcon/dataset \
  zsq_runs_erq9
```
Maybe?
```
THREAD_COUNT=16 \
SEARCH_RANGES=10,15,20,30,40,50,75,100,150,200,300,400 \
TOP_KS=10 \
WARMUP_QUERIES=1000 \
ROUNDS=5 \
BUILD_ORDER=rbq,zsq \
SEARCH_ORDER=rbq,zsq \
bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_runs

echo "exit_code=$?"
```
每次测试结束都会自动刷新：

```text
zsq_runs_erq9/Sum/zsq_key_metrics.csv
```

最终只需把这一个 CSV 传回本地。它包含原始测试点、跨轮聚合、QPS、Recall、波动范围、延迟、建库时间、索引大小、内存和量化位宽。

本地作图：

```bash
python3 test_zsq/plot_qps_recall_svg.py \
  --csv zsq_key_metrics.csv \
  --output-dir Result/Sum
```

验证情况：Python 语法检查和 Shell `bash -n` 均通过，Shell 已永久转换为 LF。ARM64 Bazel 实际编译需要你在 A 机执行上述命令。`test_zsq/__pycache__` 是本地语法检查产生的缓存，无需上传。
