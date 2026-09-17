"""Execute production schema/insert SQL in a temporary SQLite database only."""
import ast
import re
import sqlite3
import tempfile
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "database.cpp").read_text(encoding="utf-8")
schema = source.split('R"SQL(', 1)[1].split(')SQL"', 1)[0]
function = source.split('AttendanceDatabase::recordDailyAttendance(', 1)[1]
expression = function.split('const char* sql =', 1)[1].split(';', 1)[0]
insert = ''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', expression))
with tempfile.TemporaryDirectory() as folder:
    path = str(Path(folder) / 'test.db')
    db = sqlite3.connect(path)
    db.execute('PRAGMA foreign_keys=ON')
    db.executescript(schema)
    db.executescript(schema)  # 重复初始化不删除数据或报错。
    db.execute("INSERT INTO persons VALUES(2,'person_a','','2026-09-15')")
    args = (2, '2026-09-15', '2026-09-15T09:30:00.123+08:00', .86)
    assert db.execute(insert, args).rowcount == 1
    assert db.execute(insert, (2, args[1], 'later', .90)).rowcount == 0
    assert db.execute('SELECT check_time,similarity,is_test FROM daily_attendance').fetchone() == (args[2], .86, 1)
    db.commit()
    db.close()
    db = sqlite3.connect(path)
    db.execute('PRAGMA foreign_keys=ON')
    assert db.execute(insert, args).rowcount == 0  # 重启后仍去重。
    assert db.execute(insert, (2, '2026-09-16', '2026-09-16T00:00:00+08:00', .8)).rowcount == 1
    for invalid in [(999, '2026-09-15', args[2], .8), (2, '2026-09-17', args[2], 1.5)]:
        try:
            db.execute(insert, invalid)
        except sqlite3.IntegrityError:
            pass
        else:
            raise AssertionError('Invalid record was accepted')
    db.commit()
    db.execute('PRAGMA query_only=ON')
    try:
        db.execute(insert, (2, '2026-09-18', args[2], .8))
    except sqlite3.OperationalError:
        pass
    else:
        raise AssertionError('Read-only write did not fail')
    assert db.execute('SELECT count(*) FROM daily_attendance').fetchone()[0] == 2
    db.close()
print('Daily attendance SQL tests passed')
