# 测试说明

`tests/` 内的测试不会打开业务数据库 `data/attendance.db`。Python 测试读取主源码中的 SQL 字符串，在内存或临时 SQLite 数据库验证迁移、排序、去重和导出查询语义；C++ 数据库测试使用 Qt 临时目录。

## Windows 或普通 Linux 的 SQL 测试

在仓库根目录运行：

```bash
python tests/daily_attendance_test.py
python tests/attendance_list_test.py
python tests/bound_samples_query_test.py
python tests/csv_query_test.py
python tests/enrollment_sql_test.py
```

## 纯 C++ 状态机测试

```bash
g++ -std=c++17 -O2 -Wall -I. tests/recognition_state_test.cpp -o recognition_state_test
./recognition_state_test
g++ -std=c++17 -O2 -Wall -I. tests/person_ranking_test.cpp -o person_ranking_test
./person_ranking_test
```

## 板端 Qt/RKNN 测试

这些测试需要目标板上的 Qt、OpenCV、RKNN SDK/Runtime、模型或测试数据库。示例编译命令必须把仓库根目录作为 include 路径，例如 `-I.`，并根据测试添加 `database.cpp`、`face_alignment.cpp`、`face_recognizer.cpp` 与相应库。它们用于开发验证，不能替代 [FINAL_ACCEPTANCE.md](FINAL_ACCEPTANCE.md) 的真实摄像头和触摸验收。

任何实际样本图片、临时数据库、特征文本或导出 CSV 都不应提交。
