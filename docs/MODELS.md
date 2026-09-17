# 模型与供应商依赖

本仓库只提供调用接口和模型耦合的先验框数据，不分发模型二进制、RKNN Runtime 或供应商 SDK 头文件。使用者必须从有权分发的来源取得文件，并保留许可证、版本号、转换脚本和 SHA-256 记录。

## 运行时所需文件

在项目根目录部署时需准备：

| 路径 | 用途 | 来源记录要求 |
|---|---|---|
| `models/RetinaFace_mobile320_rk3588.rknn` | 320x320 人脸检测与五点关键点 | 原始模型、RKNN 转换工具版本/参数、许可证、SHA-256 |
| `models/w600k_mbf_rk3588.rknn` | 112x112 人脸特征提取，预期512维 | 原始模型、RKNN 转换工具版本/参数、许可证、SHA-256 |
| `include/rknn_api.h` | RKNN C API 编译头文件 | SDK/RKNN Runtime 版本、许可证 |
| `lib/librknnrt.so` | RKNN Runtime 动态库 | 与板端驱动匹配的版本、许可证 |

`models/`、`lib/` 和 `include/rknn_api.h` 被 `.gitignore` 排除，避免无意再分发受限组件。

## 部署前核对

```bash
sha256sum models/RetinaFace_mobile320_rk3588.rknn \
          models/w600k_mbf_rk3588.rknn \
          lib/librknnrt.so
```

将输出与本次部署的来源记录比对。不要只按文件名判断模型兼容性：检测器代码假定 320 输入、4,200 个先验和五点关键点输出；识别器代码假定 112x112 输入与512维归一化特征。

## 转换与验证

模型转换参数必须与实际预处理一致，尤其是输入颜色顺序、尺寸、量化类型和归一化。转换完成后至少进行：

1. 使用 `tests/rknn_tensor_info.cpp` 记录模型 I/O 属性、量化参数和 stride。
2. 使用 `tests/feature_test.cpp` 验证识别模型输出维数、有限值和 L2 归一化。
3. 用独立的真人样本重新测量同人/异人分数、误识和拒识；不能直接沿用本原型门限。

不要猜测或复制不明来源的下载链接、模型许可证或转换参数。将可公开的转换脚本和来源说明单独审查后再加入仓库。
