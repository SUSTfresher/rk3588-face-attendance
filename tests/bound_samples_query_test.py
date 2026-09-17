"""Validate bound-sample SQL with synthetic records in an in-memory database.

This parses the production query, but no face image or user database is read.
"""
import ast
from pathlib import Path
import re
import sqlite3

source = (Path(__file__).resolve().parents[1] / "database.cpp").read_text(encoding="utf-8")
function = source.split("AttendanceDatabase::loadBoundEnrollmentSamples(", 1)[1]
expression = function.split("const char* sql =", 1)[1].split(";", 1)[0]
sql = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', expression))
db = sqlite3.connect(":memory:")
db.executescript("""
CREATE TABLE persons(id INTEGER PRIMARY KEY, name TEXT, employee_no TEXT);
CREATE TABLE enrollment_samples(
 id INTEGER PRIMARY KEY, name TEXT, face_png BLOB, landmarks_json TEXT,
 source_width INTEGER, source_height INTEGER, crop_x INTEGER, crop_y INTEGER,
 crop_width INTEGER, crop_height INTEGER, person_id INTEGER);
INSERT INTO persons VALUES(1,'legacy_user',''),(2,'person_a','E001'),(3,'person_b','E002');
INSERT INTO enrollment_samples(id,name,person_id) VALUES
 (6,'old_sample_name',2),(7,'person_b',3),(8,'another_person_a_sample',2),
 (9,'unbound',NULL),(10,'orphan',999),(11,'legacy',1);
""")
rows = db.execute(sql).fetchall()
assert [row[0] for row in rows] == [6, 8, 7]
assert [(row[10], row[11], row[12]) for row in rows] == [
    (2, "person_a", "E001"), (2, "person_a", "E001"), (3, "person_b", "E002")]
db.execute("DELETE FROM persons WHERE id=2")
assert [row[0] for row in db.execute(sql)] == [7]
print("Bound sample query tests passed")
