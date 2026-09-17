# RK3588 端侧人脸考勤原型

一个运行在 Orange Pi 5（RK3588）上的离线人脸考勤开发原型。程序通过 V4L2 采集 NV12 相机画面，使用 RKNN Runtime 在 NPU 上执行 RetinaFace 检测和 MobileFaceNet 特征提取，在本地 SQLite 保存样本与按日去重的测试考勤记录，并使用 Qt5/linuxfb 提供竖屏触摸界面。

本仓库是可复现源码，不包含模型、RKNN Runtime、数据库、人脸图片、嵌入特征、CSV 导出或性能日志。

## 项目边界

- 面向单人、正脸、稳定光照下的开发测试，不是安全认证或商用考勤产品。
- 没有活体检测，不能抵抗照片、视频或屏幕重放攻击。
- 当前门限来自小规模开发验证，必须用独立数据和目标场景重新评估后才能扩大使用范围。
- 目前没有 RGA 加速或摄像头到 NPU 的全链路零拷贝实现；实际性能与已知边界见 [docs/PERFORMANCE.md](docs/PERFORMANCE.md)。

## 架构

```text
V4L2 NV12 camera
  -> capture thread / latest-frame mailbox
  -> inference thread: RetinaFace -> five-point alignment -> MobileFaceNet
  -> person-level ranking + three-hit confirmation
  -> Qt GUI thread: display, enrollment, SQLite daily check-in, CSV export
```

采集端只保存最新帧。推理端短暂落后时旧帧会被覆盖而不是进入无界队列，因此优先保证实时性并避免上游缓冲累积。

## 环境要求

- Orange Pi 5 / RK3588，Linux，已工作的 V4L2 NV12 摄像头（当前设备节点为 `/dev/video-camera0`）。
- Qt5 Widgets、SQLite3、OpenCV Core/Imgproc/Calib3d、qmake、C++17 编译器。
- 与设备/模型匹配的 RKNN Runtime SDK，提供 `include/rknn_api.h` 与 `lib/librknnrt.so`。
- 两个对应 RK3588 的 `.rknn` 模型。确切文件名和来源记录方式见 [docs/MODELS.md](docs/MODELS.md)。

## 构建与运行

在目标板的项目根目录准备外部依赖后：

```bash
qmake face_attendance.pro -o Makefile
make -j2
bash ./run_face_attendance.sh
```

`run_face_attendance.sh` 设置 `QT_QPA_PLATFORM=linuxfb`，默认采用 `ATTENDANCE_ROTATION=90`。物理屏幕方向相反时，使用 `ATTENDANCE_ROTATION=270 bash ./run_face_attendance.sh`；不要同时为触摸设备另加旋转。

## 数据与隐私

首次启动会创建 `data/attendance.db`。它可能包含姓名、工号、加密前的人员样本图片、关键点、特征相关数据和考勤时间，必须按敏感数据备份、访问控制和删除。`.gitignore` 已排除这些运行数据，但提交前仍应执行隐私扫描。

## 测试

纯 SQL 测试使用内存/临时数据库，不能替代板端 RKNN、摄像头和触摸验证。测试编译和运行指引见 [docs/TESTING.md](docs/TESTING.md)。现场验收流程见 [docs/FINAL_ACCEPTANCE.md](docs/FINAL_ACCEPTANCE.md)。

## 文档

- [性能和稳定性](docs/PERFORMANCE.md)
- [模型与供应商依赖](docs/MODELS.md)
- [测试说明](docs/TESTING.md)
- [考勤 CSV 导出](docs/CSV_EXPORT.md)
- [录入和图库自动刷新](docs/ENROLLMENT_UPDATE.md)
- [可选 systemd 部署](docs/DEPLOYMENT_SYSTEMD.md)
- [零拷贝可行性边界](docs/ZERO_COPY_CHECK.md)

## 许可

本项目在发布前尚未指定开源许可证。不要假定模型、RKNN Runtime、SDK 头文件或第三方转换脚本可随本仓库再分发；应分别遵守其来源方的许可证和使用条款。
