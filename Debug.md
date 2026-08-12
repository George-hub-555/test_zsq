在 B 机进入压缩包所在目录，执行：

```bash
sha256sum -c zsq_arm64_bundle.tar.gz.sha256
tar -xzf zsq_arm64_bundle.tar.gz
chmod +x zsq_arm64_bundle/run_on_b.sh zsq_arm64_bundle/bin/zsq_benchmark
```

确认新二进制可加载：

```bash
LD_LIBRARY_PATH="$PWD/zsq_arm64_bundle/lib" \
ldd zsq_arm64_bundle/bin/zsq_benchmark
```

不能出现 `not found`。

只运行 ZSQ：

```bash
BUILD_ORDER=zsq SEARCH_ORDER=zsq \
THREAD_COUNT=32 \
LINK_RANGE=32 \
LINK_CANDIDATE_SIZE=300 \
BUILD_ITER_COUNT=3 \
SEARCH_RANGES=10,15,20,30,40,50,75,100,150,200,300,400,800 \
TOP_KS=10 \
WARMUP_QUERIES=1000 \
ROUNDS=5 \
bash zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_decode_l2_test
```

如果执行后长时间完全没有输出，用调试模式运行同一命令：

```bash
BUILD_ORDER=zsq SEARCH_ORDER=zsq \
THREAD_COUNT=32 \
LINK_RANGE=32 \
LINK_CANDIDATE_SIZE=300 \
BUILD_ITER_COUNT=3 \
SEARCH_RANGES=10,15,20,30,40,50,75,100,150,200,300,400,800 \
TOP_KS=10 \
WARMUP_QUERIES=1000 \
ROUNDS=5 \
bash -x zsq_arm64_bundle/run_on_b.sh falcon/dataset zsq_decode_l2_test \
2>&1 | tee zsq_run_debug.log
```

正常情况下应依次看到：

```text
[1/4] Validate SIFT files
[2/4] Build indexes in order: zsq
Build zsq
BUILD_OK ...
[3/4] Search indexes in order: zsq
Search zsq
SEARCH_OK variant=zsq ...
```

需要注意：

- 修改后每次邻居打分都要解码 SQ，搜索会比原版慢很多，但不应该完全没有输出。
- `SEARCH_OK` 是每个完整参数点完成后才输出；单个参数点期间可能沉默一段时间。
- 只测 ZSQ 时，最后的汇总步骤可能因缺少 ERQ 配对结果而报错。这不影响此前生成的原始结果。
- 有效结果在：

```bash
find zsq_decode_l2_test -type f -name search_zsq.csv -print
```

如果连 `[1/4]` 都长时间看不到，把 `zsq_run_debug.log` 最后约50行发给我。
