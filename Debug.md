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


需要看具体是哪种格式差异。请在 B 机执行并把完整输出发来：

```bash
uname -m
file zsq_arm64_bundle/run_on_b.sh
file zsq_arm64_bundle/bin/zsq_benchmark
head -n 2 zsq_arm64_bundle/run_on_b.sh | od -An -tx1c
bash -n zsq_arm64_bundle/run_on_b.sh
```

预期应为：

```text
uname：aarch64
run_on_b.sh：ASCII/UTF-8 text，不含 CRLF
zsq_benchmark：ELF 64-bit LSB executable, ARM aarch64
bash -n：无输出，退出码0
```

可再查看退出码：

```bash
bash -n zsq_arm64_bundle/run_on_b.sh
echo $?
```

常见差异对应关系：

- `CRLF line terminators`：执行 `sed -i 's/\r$//' zsq_arm64_bundle/run_on_b.sh`
- 二进制显示 `x86-64`：编译架构错误，必须在 ARM64 配置下重新编译
- 二进制显示 `PE32/PE32+`：这是 Windows 可执行文件，Linux 不能运行
- 显示 `ELF ... ARM aarch64`：架构正确，继续检查动态库
- `bash -n` 非0：脚本语法或换行格式错误

动态库也检查一下：

```bash
LD_LIBRARY_PATH="$PWD/zsq_arm64_bundle/lib" \
ldd zsq_arm64_bundle/bin/zsq_benchmark
```

把上述输出贴出来，我才能准确判断是哪一种格式问题。
