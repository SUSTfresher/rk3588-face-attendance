"""Check production migration/transaction SQL without a board or user data."""
import ast
import re
import sqlite3
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'database.cpp').read_text(encoding='utf-8')
schema = source.split('R"SQL(', 1)[1].split(')SQL"', 1)[0]
def sql_in(function):
    text = source.split('AttendanceDatabase::' + function + '(', 1)[1]
    text = text.split('const char* sql =', 1)[1].split(';', 1)[0]
    return ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', text))

db = sqlite3.connect(':memory:')
db.execute('PRAGMA foreign_keys=ON')
db.executescript(schema)
db.execute("INSERT INTO persons VALUES(1,'legacy','','old')")
db.commit()
for _ in range(2):
    with db:
        for table, col in [('persons', 'employee_no'), ('enrollment_samples', 'person_id')]:
            if col not in [row[1] for row in db.execute('PRAGMA table_info('+table+')')]:
                ddl = re.search(r'"(ALTER TABLE '+table+r' ADD COLUMN '+col+r'[^"]*)"', source)[1]
                db.execute(ddl)
        db.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_persons_employee_no ON persons(employee_no) WHERE employee_no <> ''")
assert db.execute('SELECT name FROM persons WHERE id=1').fetchone() == ('legacy',)
insert_person = sql_in('savePersonEnrollment')
insert_sample = sql_in('saveEnrollmentSample')
with db:
    person = db.execute(insert_person, ('person_a', 'E001', 'now')).lastrowid
    for _ in range(2):
        sample = db.execute(insert_sample, ('person_a', b'png', '[]', 112, 112, 0, 0, 112, 112, 'now')).lastrowid
        db.execute('UPDATE enrollment_samples SET person_id=? WHERE id=?', (person, sample))
try:
    with db:
        db.execute(insert_person, ('person_b', 'E002', 'now'))
        db.execute(insert_sample, ('person_b', b'', '[]', 112, 112, 0, 0, 112, 112, 'now'))
except sqlite3.IntegrityError:
    pass
else:
    raise AssertionError('empty image accepted')
assert db.execute("SELECT count(*) FROM persons WHERE employee_no='E002'").fetchone()[0] == 0
assert db.execute('SELECT count(*) FROM enrollment_samples WHERE person_id=?', (person,)).fetchone()[0] == 2
print('Enrollment migration/transaction SQL tests passed')
