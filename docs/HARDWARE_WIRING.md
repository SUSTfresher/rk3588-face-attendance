# 硬件连接与上电检查

本说明记录当前原型已经从源码、系统输出和现场照片确认的连接关系，并把尚未确认的物理接口明确标为“待确认”。不要根据示意猜测 FFC 排线触点方向、风扇针脚或显示器供电电压。

## 已确认的运行拓扑

```text
合规 5 V 电源
       |
       v
Orange Pi 5 / RK3588
   | CAM3                              | HDMI
   |                                    |
   v                                    v
Orange Pi 13 MP camera             7-inch IPS touch display
(model 13855 + adapter board)      (1024 x 600 native panel)
   |                                    |
   +--> /dev/video-camera0              +--> /dev/fb0 (current mode: 720 x 480)
                                         +--> Type-C power
```

现场硬件信息已确认如下：

- 相机为 Orange Pi 1300 万像素模组，型号 `13855`，标注支持 RK3588/RK3588S。镜头模组先连接蓝色 `CAM ADAPTER` 转接板，转接板的主板侧 FFC 排线接 Orange Pi 5 的 **CAM3** 接口。
- 显示器为 7 英寸 IPS 触摸屏，面板原生分辨率 `1024 x 600`；视频走 **HDMI**，显示器走 **Type-C** 供电。
- 程序打开 `/dev/video-camera0` 并配置 NV12 720 x 480，同时通过 `/dev/v4l-subdev2` 设置传感器控制。这与 CAM3 的 CSI 相机驱动路径一致。
- 程序在 `linuxfb` 上显示。当前系统观测到 `/dev/fb0` 的虚拟尺寸为 `720 x 480`，应用内部旋转为 `480 x 720` 竖屏逻辑坐标。`720 x 480` 是此次系统实际输出模式，**不是**这块屏幕的原生 `1024 x 600` 能力。

显示器的视频接口和相机连接口现已确认；风扇针脚、Orange Pi 的具体供电适配器规格，以及 Type-C 线是否同时承担触摸 USB 数据仍须以实物和产品说明为准。

## 连接步骤

### 1. 电源

1. 断开开发板电源后再插拔 FFC、显示或风扇连接器。
2. 按 Orange Pi 5 官方硬件手册使用符合要求的稳定 USB-C 5 V 电源，并为显示器、风扇等外设保留电流余量。
3. 不要从 CSI/DSI 排线、未知 GPIO 针脚或 USB 数据线给开发板反向供电。
4. 上电前检查所有外露导体、排线和风扇线没有碰到散热器或金属外壳。

### 2. CAM3 相机

1. 相机镜头侧 FFC 接入 `13855` 模组的 `TO CAMERA` 接口；转接板主板侧 FFC（标有 `TO MB`）接入 Orange Pi 5 的 **CAM3**。
2. **不能**把该排线插到 DSI 显示接口或其他 CAM 接口。模型、接口和设备树必须相互对应。
3. 以 Orange Pi 5 CAM3 接口和相机转接板的丝印/官方手册规定的方向放置排线触点；本仓库不假定“蓝面朝上”或“金手指朝某一侧”。
4. 将锁扣完全打开，排线平直插到底后再压紧锁扣；避免锐角弯折和受力拉扯。
4. 上电后，先验证设备节点再启动应用：

```bash
ls -l /dev/video-camera0 /dev/v4l-subdev2
v4l2-ctl --list-devices 2>/dev/null || true
```

若 `/dev/video-camera0` 不存在，先排查排线方向、CSI 接口、内核设备树和相机驱动，不要修改识别门限或应用代码。

### 3. HDMI 显示与触摸

| 连接 | 连接方式 | 作用 |
|---|---|---|
| 视频 | Orange Pi 5 HDMI 输出 -> 显示器 HDMI 输入 | 输出画面。当前内核实际 framebuffer 模式为 720 x 480。 |
| 供电 | 合规 USB-A/Type-C 或独立 Type-C 电源 -> 显示器 Type-C | 为显示器供电，电压/电流以显示器说明为准。 |
| 触摸（按实际设备确认） | 显示器 USB 数据口 -> Orange Pi USB 主机口 | 许多 HDMI 触摸屏通过 USB HID 回传触摸；若 Type-C 线仅供电，则需另接数据线。 |

不要把 HDMI 显示器接到 Orange Pi 的 MIPI-DSI FFC 接口，也不要把 CAM3 相机排线接到显示器接口。

启动后确认 framebuffer，并再运行程序：

```bash
cat /sys/class/graphics/fb0/name
cat /sys/class/graphics/fb0/virtual_size
bash ./run_face_attendance.sh
```

当前项目默认 `ATTENDANCE_ROTATION=90`。画面方向相反时使用：

```bash
ATTENDANCE_ROTATION=270 bash ./run_face_attendance.sh
```

不要再为触摸设备增加第二层旋转；`PortraitShell` 已在 Qt 图形视图中同步转换显示与输入坐标。若要切换 Linux HDMI 输出到原生 `1024 x 600`，这是显示驱动/桌面配置问题，完成后必须重新验证全屏布局、触摸命中位置和 `ATTENDANCE_ROTATION`，不能只修改应用窗口尺寸。

### 4. 散热与开发网络

- 现场原型使用风扇散热。风扇的电压、正负极和针脚必须以实际风扇标签及 Orange Pi 5 官方 pinout 为准；未知两线风扇不要盲接 GPIO。
- 以太网接笔记本共享网络只用于 SSH、时间同步和部署。考勤识别、SQLite 写入和竖屏界面均可离线运行；网络不是日常考勤的依赖。

## 提交照片前的核对清单

若要把硬件照片或接线图公开到 GitHub，先确认照片不含人脸、姓名、工号、IP 地址、Wi-Fi 信息、终端令牌或其他个人信息。当前现场照片中的屏幕包含测试人脸与身份信息，不能原样提交。

为了把本说明升级为包含实际针脚/插头照片的精确接线图，还需要两张不含敏感信息的近照：

1. Orange Pi 5 正面，能看到 CSI、DSI、HDMI、USB-C 和风扇连接位置。
2. 显示屏背面及其电源/触摸/信号线连接位置。
