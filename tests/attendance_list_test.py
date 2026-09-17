"""Validate list SQL against a synthetic in-memory SQLite database.

The query text is extracted from the production source so this test detects a
query-semantic change. It never opens a device attendance database.
"""
import ast
import re
import sqlite3
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'database.cpp').read_text(encoding='utf-8')
expression = source.split('AttendanceDatabase::listDailyAttendance(', 1)[1].split(
    'const char* sql =', 1)[1].split(';', 1)[0]
sql = ''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', expression))
db = sqlite3.connect(':memory:')
db.executescript('''
CREATE TABLE persons(id INTEGER PRIMARY KEY,name TEXT,employee_no TEXT);
CREATE TABLE daily_attendance(id INTEGER PRIMARY KEY,person_id INTEGER,
attendance_date TEXT,check_time TEXT,is_test INTEGER);
INSERT INTO persons VALUES(2,'person_a','E001');
''')
assert db.execute(sql).fetchall() == []
db.executemany('INSERT INTO daily_attendance VALUES(?,?,?,?,?)',
    [(i, 2, '2026-09-15', '2026-09-15T09:09:28.034+08:00', 1) for i in range(1, 102)])
db.execute("INSERT INTO daily_attendance VALUES(102,999,'2026-09-16','2026-09-16T09:00:00+08:00',1)")
rows = db.execute(sql).fetchall()
assert len(rows) == 100
assert rows[0][0] == 102 and rows[0][2] is None
assert rows[1][0] == 101 and rows[-1][0] == 3
assert rows[1][2:4] == ('person_a', 'E001')
assert rows[1][5:] == ('2026-09-15T09:09:28.034+08:00', 1)
print('Attendance list SQL tests passed')
