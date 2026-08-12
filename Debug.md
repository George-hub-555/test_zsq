这里大概率实际是退出码 `141`，不是 `1141`。Bash 进程退出码范围只有 `0–255`。

`141 = 128 + SIGPIPE(13)`，与脚本这行高度吻合：

```bash
ldd --version 2>&1 | head -n 1
```

脚本开启了：

```bash
set -euo pipefail
```

在部分 ARM/Linux 环境中，`head` 读取第一行后提前关闭管道，`ldd` 收到 `SIGPIPE`，整个脚本在输出 `[1/4]` 前直接退出。因此只留下近乎空的运行目录。

先在 B 机验证：

```bash
set -o pipefail
ldd --version 2>&1 | head -n 1
printf 'exit_code=%d\n' "$?"
```

如果输出 `141`，就确认了。

只修改 B 机上的脚本副本，将 `head` 换成会读完整输入的 `sed`：

```bash
sed -i "s#ldd --version 2>&1 | head -n 1#ldd --version 2>\&1 | sed -n '1p'#" zsq_arm64_bundle/run_on_b.sh
```

确认修改：

```bash
grep -n 'ldd --version' zsq_arm64_bundle/run_on_b.sh
bash -n zsq_arm64_bundle/run_on_b.sh
```

预期看到：

```bash
ldd --version 2>&1 | sed -n '1p'
```

然后重新运行原命令即可。这是 `run_on_b.sh` 的 Linux环境兼容问题，与刚修改的 ZSQ C++搜索代码无关。
