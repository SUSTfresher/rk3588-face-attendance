"""Validate production export SQL in memory; board C++ tests cover CSV bytes."""
import ast
import re
import sqlite3
from pathlib import Path
source = (Path(__file__).resolve().parents[1] / 'database.cpp').read_text(encoding='utf-8')
body = source.split('AttendanceDatabase::exportDailyAttendanceCsv(',1)[1]
expression = body.split('const char* sql =',1)[1].split(';',1)[0]
query = ''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', expression))
db = sqlite3.connect(':memory:')
db.executescript('CREATE TABLE persons(id,name,employee_no); CREATE TABLE daily_attendance(id,person_id,attendance_date,check_time,similarity,is_test);')
assert db.execute(query).fetchall()==[]
db.execute('INSERT INTO persons VALUES(2,?,?)', ('synthetic,"name"\nrecord','E001'))
for i in range(105):
    db.execute('INSERT INTO daily_attendance VALUES(?,?,?,?,?,?)', (i+1,2,'2026-09-15','2026-09-15T09:09:28.034+08:00',.84,1))
db.execute("INSERT INTO daily_attendance VALUES(106,999,'2026-09-16','time',.7,0)")
rows = db.execute(query).fetchall()
assert len(rows)==106 and rows[0][0]==1 and rows[-1][0]==106
assert rows[0][3]=='E001' and rows[0][5].endswith('+08:00') and rows[0][7]==1
assert rows[-1][2:4]==(None,None) and rows[-1][7]==0
print('CSV query tests passed (empty, >100, order, orphan, identifiers, test flag)')
