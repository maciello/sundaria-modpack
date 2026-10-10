#!/usr/bin/env python3
"""All game DataTables / CurveTables in one SQLite db (build/game-data.sqlite; game-derived, gitignored). Skill ref: references/game-data.md.
  tables.py                 build if the pak changed, print counts
  tables.py <text>          full-text search over table names, row names and text values
  tables.py --sql "<sql>"   raw SQL
Schema: tables(name, path, kind, row_struct, rows, columns) · rows(tbl, row, json) · curves(tbl, row, x, y) · fts(tbl, row, text)"""
import json
import re
import sqlite3
import sys
import time

import data

DB = data.HERE.parent / "build" / "game-data.sqlite"


def stamp():
    st = data.pak().stat()
    return f"{st.st_size}-{int(st.st_mtime)}-v1"


def texts(x):
    if isinstance(x, dict):
        for v in x.values():
            yield from texts(v)
    elif isinstance(x, list):
        for v in x:
            yield from texts(v)
    elif isinstance(x, str):
        yield x


def build():
    t0 = time.time()
    dump = data.cache() / "tables.jsonl"
    if data.reader("tables-dump", str(dump), out=sys.stderr).returncode:
        sys.exit("!! tables-dump failed")
    DB.parent.mkdir(exist_ok=True)
    tmp = DB.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)
    con = sqlite3.connect(tmp)
    con.executescript("""create table tables(name, path primary key, kind, row_struct, rows, columns);
        create table rows(tbl, row, json); create table curves(tbl, row, x, y);
        create virtual table fts using fts5(tbl, row, text); create table meta(stamp);
        create index rows_i on rows(tbl, row); create index curves_i on curves(tbl, row);""")
    for line in dump.open():
        t = json.loads(line)
        name = t["path"].rsplit("/", 1)[1]
        rws = data.unguid(t["rows"])
        st = re.sub(r"^\w+'(.*)'$", r"\1", t["row_struct"] or "")
        first = next(iter(rws.values()), None)
        cols = list(first) if isinstance(first, dict) else []
        con.execute("insert into tables values (?,?,?,?,?,?)", (name, t["path"], t["kind"], st, len(rws), json.dumps(cols)))
        for r, v in rws.items():
            con.execute("insert into rows values (?,?,?)", (name, r, json.dumps(v, ensure_ascii=False)))
            con.execute("insert into fts values (?,?,?)", (name, r, " ".join(texts(v))))
            for k in v.get("Keys", []) if isinstance(v, dict) else []:
                con.execute("insert into curves values (?,?,?,?)", (name, r, k.get("Time"), k.get("Value")))
    con.execute("insert into meta values (?)", (stamp(),))
    con.commit()
    con.close()
    tmp.rename(DB)
    print(f"built {DB.name} in {time.time() - t0:.0f} s", file=sys.stderr)


def connect():
    if DB.exists():
        con = sqlite3.connect(DB)
        if con.execute("select stamp from meta").fetchone()[0] == stamp():
            return con
        con.close()
    build()
    return sqlite3.connect(DB)


def main(argv):
    con = connect()
    if argv[:1] == ["--sql"]:
        cur = con.execute(argv[1])
        print("\t".join(c[0] for c in cur.description))
        for r in cur:
            print("\t".join(map(str, r)))
    elif argv:
        q = " ".join(f'"{w}"*' for w in re.findall(r"\w+", " ".join(argv)))
        for r in con.execute("select tbl, row, snippet(fts, -1, '[', ']', '…', 10) from fts where fts match ? order by rank limit 300", (q,)):
            print("\t".join(r))
    else:
        for k in ("tables", "rows", "curves"):
            print(k, con.execute(f"select count(*) from {k}").fetchone()[0])
        print("by kind", dict(con.execute("select kind, count(*) from tables group by kind")))


if __name__ == "__main__":
    main(sys.argv[1:])
