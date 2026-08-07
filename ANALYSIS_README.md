# RBQ/ZSQ 结果提取与离线绘图

两个脚本兼容 Python 3.8.3，只使用标准库，不需要安装 matplotlib、pandas 或 numpy。

## 输入目录

在 `falcon` 父目录准备：

```text
Result/
├── zsq_run_时间1/
│   ├── result/ 或 results/
│   │   ├── build_rbq.csv
│   │   ├── build_zsq.csv
│   │   ├── search_rbq.csv
│   │   └── search_zsq.csv
│   └── SHA256SUMS
├── zsq_run_时间2/
├── zsq_run_时间3/
├── manifest.txt
└── Sum/
```

脚本递归查找 CSV，因此中间目录叫 `result` 或 `results` 都可以。因早期失败产生、缺少搜索 CSV 的运行目录会被警告并跳过。

## 执行

始终在 `falcon` 父目录执行：

```bash
python3 test_zsq/extract_zsq_results.py \
  --input Result \
  --output Result/Sum/zsq_key_metrics.csv

python3 test_zsq/plot_qps_recall_svg.py \
  --csv Result/Sum/zsq_key_metrics.csv \
  --output-dir Result/Sum
```

成功时两个命令的退出码均为 0：

```bash
echo "exit_code=$?"
```

## 输出

```text
Result/Sum/
├── zsq_key_metrics.csv
├── qps_recall_top10_linear.svg
├── qps_recall_top10_log.svg
├── qps_recall_top10.html
└── analysis_warnings.txt
```

`zsq_key_metrics.csv` 是唯一需要复制或粘贴给分析人员的关键数据文件：

- `scope=run`：每轮原始测试点；
- `scope=aggregate`：相同 variant、TopK、search_range 的跨轮中位数；
- `qps_min/qps_max`、`recall_min/recall_max`：跨轮波动范围；
- 建库耗时、索引大小和内存数据已合并到同一行。

SVG 和 HTML 均为纯文本离线文件，可直接用浏览器打开。HTML 不引用网络资源。
