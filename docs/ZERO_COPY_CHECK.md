# RKNN tensor-memory 可行性检查

板端已经验证：`include/rknn_api.h` 声明了 `rknn_create_mem`、`rknn_destroy_mem`、`rknn_mem_sync`、`rknn_set_io_mem`；`lib/librknnrt.so` 也导出了相同符号。因此可研究 RKNN tensor-memory 输入路径。

这不等于摄像头到NPU全链路零拷贝。当前路径仍为 V4L2 摄像头帧 → QImage → RGB/缩放/letterbox/BGR 转换 → NPU。tensor-memory 实验只尝试替换 `rknn_inputs_set` 的普通输入缓冲提交，目标是检查约8毫秒输入提交开销及其抖动。全链路 DMA-BUF 需要单独接入 V4L2 buffer、RGA 与 RKNN `rknn_create_mem_from_fd`，不能混入稳定版本。

先查询模型的真实 tensor 类型、尺寸与量化参数：

```bash
cd ~/edge_inspector/05_face_attendance
g++ -std=c++17 -O2 -Wall -fPIC rknn_tensor_info.cpp \
  -Iinclude -I/usr/include/aarch64-linux-gnu/qt5 \
  -I/usr/include/aarch64-linux-gnu/qt5/QtCore \
  -L./lib -Wl,-rpath,'$ORIGIN/lib' -lrknnrt -lQt5Core \
  -o rknn_tensor_info
./rknn_tensor_info models/RetinaFace_mobile320_rk3588.rknn
```

输出必须确认：输入 `size_with_stride`、格式和类型；每个输出的 `type`、`qnt_type`、`zp`、`scale`、`size_with_stride`。使用 `rknn_set_io_mem` 的输出缓冲与既有 `rknn_outputs_get(... want_float=1)` 语义不同：若输出被量化，必须按每个 output 的零点和 scale 显式反量化。没有该信息前不改检测器，避免正确性回退。
