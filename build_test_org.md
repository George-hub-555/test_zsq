已完成静态核对；本机未编译、未测试，也未修改任何文件。

## 已确认

- Bazel 目标是 `//tools/index_factory:blink_graph_index_checker`，定义在 [BUILD](E:/Graduation/GPTproject/FS_zsq/falcon/tools/index_factory/BUILD:604)。
- 项目锁定 Bazel `7.4.1`，仓库内已有 AArch64 Bazel：
  `devel/builder/bazel-7.4.1-linux-arm64`。
- ARM 编译配置为 `--config=linux_arm64`，会启用：
  `-march=armv8.2-a+crypto+crc+dotprod`。
- 源码会读取：
  `sift_base.fvecs`、`sift_query.fvecs`、`sift_groundtruth.ivecs`，并在 `index_type=1` 时生成 `sift_base_bkg.bin.1`。

## 编译前需要处理的问题

当前 [blink_graph_index_checker.cpp](E:/Graduation/GPTproject/FS_zsq/falcon/tools/index_factory/blink_graph_index_checker.cpp:10) 引用了 ERQ Builder/Searcher，但 BUILD 目标没有声明对应依赖。即使运行时使用 `index_type=1`，链接阶段仍可能产生 ERQ 符号未定义。

在该目标的 `deps` 中补充：

```python
"//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_erq_builder",
"//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_erq_searcher_adaptive",
```

建议放在其他 `blink_graph_*` 依赖附近。这是最小改动，不需要修改 C++ 代码。

## A 机器编译

在 A 机器进入 `falcon` 仓库根目录：

```bash
cd /path/to/falcon

uname -m
chmod +x devel/builder/bazel-7.4.1-linux-arm64

./devel/builder/bazel-7.4.1-linux-arm64 build \
  --config=linux_arm64 \
  --config=no_sve \
  --copt=-g0 \
  --strip=always \
  //tools/index_factory:blink_graph_index_checker
```

这里推荐 `--config=no_sve`，因为目前没有确认 B 机器支持 SVE。它会使用非 SVE 回退实现，跨 ARM 机器更稳。如果确认 B 机器支持 SVE，且需要测试 SVE 性能，可以去掉该选项。

产物位置：

```bash
bazel-bin/tools/index_factory/blink_graph_index_checker
```

复制时要用 `-L`，避免复制 Bazel 符号链接本身：

```bash
mkdir -p blink_checker_pkg/bin blink_checker_pkg/lib

cp -L bazel-bin/tools/index_factory/blink_graph_index_checker \
  blink_checker_pkg/bin/
```

## 携带运行库

先在 A 机器检查：

```bash
ldd bazel-bin/tools/index_factory/blink_graph_index_checker
```

重点关注非系统运行库，常见包括：

```text
libgomp.so.1
libstdc++.so.6
libgcc_s.so.1
```

如果 B 机器版本不确定，把 `ldd` 显示的这些库复制到：

```text
blink_checker_pkg/lib/
```

不要自行复制 `libc.so` 或 `ld-linux-aarch64.so`。最好保证 A、B 使用相同发行版，或者 A 的 glibc 不高于 B。

打包：

```bash
tar -czf blink_graph_index_checker-aarch64.tar.gz blink_checker_pkg
```

## B 机器运行形式

解压后：

```bash
tar -xzf blink_graph_index_checker-aarch64.tar.gz
cd blink_checker_pkg

export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

./bazel-bin/tools/index_factory/blink_graph_index_checker \
  -data_path=/opt/huawei/data3/g50064150/falcon/dataset/ \
  -query_size=10000 \
  -doc_size=1000000 \
  -dim=128 \
  -thread_count=80 \
  -search_range=400 \
  -build_index=true \
  -index_type=1 \
  -rotator_type=1
```

注意：

- `data_path` 末尾必须保留 `/`。
- 数据目录必须包含上述三个 SIFT 文件。
- `build_index=true` 会向数据目录写入 `sift_base_bkg.bin.1`，B 机器用户必须有写权限。
- B 机器 CPU 至少需要支持编译配置中的 ARMv8.2、crypto、crc、dotprod 指令特性。

目标处理用量：104,906 tokens，约 7 分 44 秒。
