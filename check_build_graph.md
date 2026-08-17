# A 编译、B 检测 HNSW 连通性

## A 机器编译

在 Falcon 仓库根目录执行：

```bash
cd /path/to/falcon

bazel build \
  --config=linux_arm64 \
  --copt=-g0 \
  --strip=always \
  //tools/index_factory:hierarchical_nsw_index_checker
```

编译产物位于：

```text
/path/to/falcon/bazel-bin/tools/index_factory/hierarchical_nsw_index_checker
```

复制真实文件并检查动态依赖：

```bash
mkdir -p /tmp/hnsw-connect-check
cp -L bazel-bin/tools/index_factory/hierarchical_nsw_index_checker \
  /tmp/hnsw-connect-check/

file /tmp/hnsw-connect-check/hierarchical_nsw_index_checker
ldd /tmp/hnsw-connect-check/hierarchical_nsw_index_checker
```

传到 B：

```bash
scp /tmp/hnsw-connect-check/hierarchical_nsw_index_checker \
  user@B:/opt/hnsw-connect-check/
```

## B 机器运行

假设图包含 `N` 个向量、维度为 `D`。当前程序固定按照下面的名字查找索引：

```text
hnsw_<N整除1000000>m_ef_200_M_16_thread_1.bin
```

准备目录：

```bash
mkdir -p /opt/hnsw-connect-check/data
touch /opt/hnsw-connect-check/data/sift_query.txt
touch /opt/hnsw-connect-check/data/sift_groundtruth.txt

ln -s /path/to/existing_hnsw.bin \
  /opt/hnsw-connect-check/data/hnsw_<N整除1000000>m_ef_200_M_16_thread_1.bin
```

执行只加载检测：

```bash
cd /opt/hnsw-connect-check

./hierarchical_nsw_index_checker \
  --data_format=sift \
  --data_path=/opt/hnsw-connect-check/data/ \
  --data_size=<N> \
  --dim=<D> \
  --thread_count=1 \
  --build_index=false \
  --query_size=0
```

判断日志：

```text
Total element not found 0
```

表示第 0 层所有节点都能从入口点到达；大于 `0` 表示存在不可达节点。`query_size=0` 可能额外输出无意义的 `Total recall nan`，不影响连通性结果。

## 接口与代码

- 不修改代码或公共接口。
- `--m`、`--ef_construction` 当前未真正传入配置，程序实际固定使用 `M=16、efConstruction=200`。
- 输入 `.bin` 必须是兼容当前 Falcon 版本的 HNSW 序列化文件，不能直接使用任意 hnswlib/faiss 文件。

## 验证与假设

- A、B 都是 AArch64 Linux。
- B CPU 必须支持仓库 `linux_arm64` 配置要求的 ARMv8.2、crypto、crc、dotprod 指令。
- 在 B 上先运行 `ldd ./hierarchical_nsw_index_checker`，确认没有 `not found`；若 glibc 版本不兼容，应在与 B 相同或更老的系统环境中重新编译。
