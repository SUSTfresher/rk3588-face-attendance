# 考勤 CSV 导出

入口：管理 → 考勤记录 → 导出全部 CSV。导出后关闭列表并显示完成提示。

导出全部 daily_attendance，按日期和记录编号升序，不受界面100条限制。列为记录编号、人员编号、姓名、工号、考勤日期、首次打卡时间、相似度、是否测试（1是0否）。姓名和工号取当前人员表；人员资料缺失时保留该考勤行，姓名和工号为空。空库导出只有表头。

文件在程序旁的 exports 文件夹，文件名含上海时间及随机标识，每次生成新文件。UTF-8 BOM，CRLF换行，双引号转义；文本以公式符号开头时加单引号避免执行。失败不提交半成品，不修改考勤。

## 板端验证

退出程序并备份数据库后，将已审查的仓库同步到目标板的应用目录。以下示例使用 `/opt/face-attendance`；请按设备账户、权限和实际部署目录调整。

```bash
cd /opt/face-attendance
g++ -std=c++17 -O2 -Wall -fPIC -I. tests/csv_export_test.cpp database.cpp \
  $(pkg-config --cflags --libs Qt5Core) -lsqlite3 -o csv_export_test
./csv_export_test
qmake face_attendance.pro -o Makefile
make -j2 && bash ./run_face_attendance.sh
```

测试只使用临时数据库，覆盖空库、105条记录、中文、逗号、引号、换行、公式文本、工号前导零、时区、测试标记和写入失败。本地仅验证实际查询SQL及原有SQL回归；完整Qt编译和测试需板端执行。

## 导出后复制到电脑

使用受控的传输方式把 `exports/` 中所需 CSV 复制到管理电脑，并在导入后按组织的数据保留规则处理。不要把导出文件放进仓库或公开共享位置。

Excel请用“数据 → 从文本/CSV”导入，将工号列设为文本（必要时在“转换数据”中移除自动类型转换再设置文本），否则双击CSV可能把001显示成1。原始CSV仍保存001。是否测试=1仍是开发测试考勤。

验收：导出条数与 `SELECT COUNT(*) FROM daily_attendance;` 一致；文件中文字正常、工号原值保留、时间含+08:00；连续导出生成两个文件；返回管理和识别界面正常。
