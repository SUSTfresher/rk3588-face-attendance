# 可选部署：开机启动与异常退出恢复

这是部署参考，不属于应用新增功能。当前仓库未安装或启用任何服务，也不修改程序、启动脚本或数据库。下文以专用账户 `attendance` 和目录 `/opt/face-attendance` 为例；部署前必须替换为实际、已验证的设备账户和目录。

## 工作方式与边界

- systemd 启动已有脚本，脚本通过 exec 运行 face_attendance，主进程可以直接被跟踪。
- Restart=on-failure 在非零退出、SIGSEGV、SIGKILL 等异常终止后重启；RestartSec=5s 防止连续立即重启。
- 界面“退出程序”正常返回0，不会自动重启；systemctl stop 也不会触发重启。
- 当前摄像头采集失败只停止采集线程并显示错误，主进程还在。进程卡死也不等于退出。本方案不检测这两类情况，不能声称实现全部故障自愈。以后若需要，应单独实现错误退出策略或配套心跳监控；仅写 WatchdogSec 不足以实现应用心跳。
- 保留应用原有时间同步检查：未同步时允许识别，但暂停写考勤。After=chrony.service 只保证启动顺序，不保证时间已经同步。
- 不要让手动实例和服务实例同时占用摄像头/屏幕。先从管理菜单退出应用，再测试。

## 一次性验证：不设置开机启动

先在板端确认普通账户权限：

```bash
id attendance
ls -l /dev/fb0 /dev/video-camera0 /dev/v4l-subdev2 /dev/input/event* /dev/dri/render* /dev/rknpu
```

不同镜像可能没有最后几种设备节点，以实际 RKNN 驱动为准。当前普通账户能运行程序是已有依据，但 systemd 会话未必继承登录会话的设备 ACL；如果日志报 Permission denied，再针对实际设备组处理，不要直接将设备设为所有人可写。还需确认不会与桌面环境或控制台持续绘制冲突。

退出手动运行的考勤程序，在 SSH 终端逐条执行：

```bash
sudo systemd-run --unit=face-attendance-check --property=User=attendance --property=WorkingDirectory=/opt/face-attendance --property=Restart=on-failure --property=RestartSec=5s /bin/bash /opt/face-attendance/run_face_attendance.sh
systemctl status face-attendance-check --no-pager
systemctl show face-attendance-check -p MainPID -p NRestarts -p ActiveState
sudo journalctl -u face-attendance-check -n 40 --no-pager
```

这是临时服务，不会在重启后自动启动。观察竖屏、触摸、摄像头及识别是否正常。不要在录入、删除或导出过程中进行下面的故障模拟。

确认程序空闲后，模拟异常终止并等待5至10秒：

```bash
sudo systemctl kill --kill-who=main --signal=SIGKILL face-attendance-check.service
```

随后检查：

```bash
systemctl show face-attendance-check -p MainPID -p NRestarts -p ActiveState
sudo journalctl -u face-attendance-check -n 40 --no-pager
```

通过标准：MainPID 改变、NRestarts 增加、ActiveState=active，并且界面和摄像头重新可用。数据库内容应仍可查询；SIGKILL 会丢失内存中的临时识别状态。

测试结束：

```bash
sudo systemctl stop face-attendance-check.service
```

等待超过5秒，确认没有自行重启。临时单元停止后可能被自动回收，此时显示 not found 属正常情况。可以重新手动启动原程序。

## 以后需要开机启动时

以下配置仅作为 GitHub 部署示例。当前不用安装。

保存为 /etc/systemd/system/face-attendance.service：

```ini
[Unit]
Description=Orange Pi Face Attendance
After=local-fs.target chrony.service
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=attendance
WorkingDirectory=/opt/face-attendance
Environment=QT_QPA_PLATFORM=linuxfb
Environment=ATTENDANCE_ROTATION=90
ExecStart=/bin/bash /opt/face-attendance/run_face_attendance.sh
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

设备权限、设备别名创建和显示冲突需在目标镜像验证。连续故障达到启动限额后会停止重试；排除问题后使用 systemctl reset-failed face-attendance 再启动。

以后确认要部署时执行：

```bash
sudo systemd-analyze verify /etc/systemd/system/face-attendance.service
sudo systemctl daemon-reload
sudo systemctl enable --now face-attendance.service
```

enable 才建立开机启动关系。完整验收还需实际重启设备，确认自动启动、权限、显示及校时状态，不能用一次临时服务测试替代。关闭可选部署：

```bash
sudo systemctl disable --now face-attendance.service
```

当前验证范围：本地已核对启动路径、exec 行为、正常退出和摄像头失败处理代码。Windows 环境没有 systemd，尚未执行板端服务测试、配置解析或开机验证。以后根据实际测试结果更新此处，不将说明示例写成已验收功能。
