# 性能采样

`perf_sample.sh` 只读取系统状态并写入采样文件，不启动或停止考勤程序，不修改数据库、识别逻辑或 systemd 配置。

先在一个 SSH 终端启动应用并保存其日志：

```bash
cd ~/edge_inspector/05_face_attendance
mkdir -p perf_logs
bash ./run_face_attendance.sh 2>&1 | tee "perf_logs/app_$(date +%Y%m%d_%H%M%S).log"
```

在另一个 SSH 终端运行5分钟采样：

```bash
cd ~/edge_inspector/05_face_attendance
bash perf_sample.sh 300
```

采样保存温度、CPU频率、可用内存、每线程CPU快照，以及 NPU负载和频率。此镜像的 NPU节点为 `/sys/devices/platform/fdab0000.npu/devfreq/fdab0000.npu/load`，格式为 `负载百分比@频率Hz`，例如 `100@1000000000Hz`。脚本将其拆成 `npu_load_percent` 与 `npu_frequency_hz` 两行数值。

这是每秒轮询快照，用于观察持续负载、频率和温度趋势，不能代表亚秒级峰值的精确NPU利用率。

完成后可查看：

```bash
latest=$(find perf_logs -mindepth 1 -maxdepth 1 -type d | sort | tail -n1)
grep -E 'detector timing|stage timing|inference FPS|capture FPS' perf_logs/app_*.log
grep -E 'npu_(load_percent|frequency_hz)' "$latest/system.csv" | tail -10
```
