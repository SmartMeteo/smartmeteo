#!/usr/bin/env python3
"""Build the city database from GeoNames (https://www.geonames.org, CC BY 4.0).

cities500: every populated place with at least 500 inhabitants (or a seat of administration), about
230 000 of them, each with coordinates, country, first-level division and IANA time zone — exactly what
the device needs to fetch a forecast and keep its clock. Built into a new file and renamed over the old
one, so the server never reads a half-written database. Stdlib only.

usage: build_db.py /var/lib/smartmeteo-geo/cities.sqlite
"""
import io
import os
import sqlite3
import sys
import tempfile
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import norm  # noqa: E402

BASE = "https://download.geonames.org/export/dump/"
UA = "SmartMeteo-geo/1.0 (+https://smartmeteo.org)"
MAX_ALT = 40           # alternate names longer than this are descriptions, not names
# Feature codes left out: sections of a place (city districts), and historical, abandoned, destroyed or
# religious places. Someone in a district picks the city; the forecast is the same.
SKIP_FEATURES = {"PPLX", "PPLH", "PPLQ", "PPLW", "PPLCH", "PPLR"}


def fetch(name: str) -> bytes:
    req = urllib.request.Request(BASE + name, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def main(out_path: str) -> None:
    admin1 = {}
    for line in fetch("admin1CodesASCII.txt").decode("utf-8").splitlines():
        f = line.split("\t")
        if len(f) >= 2:
            admin1[f[0]] = f[1]                       # "DE.05" -> "Hesse"

    z = zipfile.ZipFile(io.BytesIO(fetch("cities500.zip")))

    # Streamed row by row into SQLite: the box has 1.8 GB and the unit a 512 MB ceiling, and holding the
    # whole name index in a dict went past it (2026-09-17). A place's own names go in first, so the
    # INSERT OR IGNORE of its alternate names keeps alt = 0 where a key is both.
    fd, tmp = tempfile.mkstemp(prefix="cities.", suffix=".sqlite", dir=os.path.dirname(os.path.abspath(out_path)))
    os.close(fd)
    try:
        db = sqlite3.connect(tmp)
        db.executescript("""
            PRAGMA journal_mode = OFF;
            PRAGMA synchronous = OFF;
            PRAGMA cache_size = -65536;
            CREATE TABLE cities (id INTEGER PRIMARY KEY, name TEXT, country TEXT, state TEXT,
                                 lat REAL, lon REAL, tz TEXT, pop INTEGER, elev INTEGER);
            CREATE TABLE names (key TEXT, id INTEGER, alt INTEGER, PRIMARY KEY (key, id)) WITHOUT ROWID;
            CREATE TABLE meta (k TEXT PRIMARY KEY, v TEXT);
        """)
        n_cities = 0
        with z.open("cities500.txt") as raw:
            for line in io.TextIOWrapper(raw, encoding="utf-8"):
                f = line.rstrip("\n").split("\t")
                if len(f) < 19 or f[7] in SKIP_FEATURES:
                    continue
                gid, name, ascii_name, alts = int(f[0]), f[1], f[2], f[3]
                cc, a1, tz = f[8], f[10], f[17]
                try:
                    lat, lon, pop = float(f[4]), float(f[5]), int(f[14] or 0)
                except ValueError:
                    continue
                # Metres above sea level. GeoNames carries two: `elevation` (f[15], surveyed, often
                # blank) and `dem` (f[16], a 30"x30" average from SRTM3/GTOPO30, nearly always
                # filled, -9999 where it has none). The surveyed one wins where it exists.
                #
                # It is only ever a FALLBACK — for a device that will never receive a weather report
                # and so can never measure its own altitude factor. A city-centre average is enough
                # for that: the gate it feeds separates high pressure from low, a step of some
                # 15 hPa, and 25 m of error is 3.
                try:
                    elev = int(f[15]) if f[15].strip() else int(f[16] or -9999)
                except ValueError:
                    elev = -9999
                if elev < -500 or elev > 9000:
                    elev = -9999          # Dead Sea to Everest; outside that it is not a height
                db.execute("INSERT INTO cities VALUES (?,?,?,?,?,?,?,?,?)",
                           (gid, name, cc, admin1.get(f"{cc}.{a1}", ""), lat, lon, tz, pop, elev))
                n_cities += 1
                # alt 0: the place's own name; alt 1: one of its names in other languages and spellings.
                # The server ranks the first kind above the second, so "new yo" is New York before places
                # that merely list "New York" among their alternate names.
                own = {k for k in (norm(name), norm(ascii_name)) if k}
                db.executemany("INSERT OR IGNORE INTO names VALUES (?,?,0)", [(k, gid) for k in own])
                other = set()
                for alt in alts.split(","):
                    alt = alt.strip()
                    # Alternate names include airport codes, postal codes and links; a name has letters.
                    if 2 <= len(alt) <= MAX_ALT and any(ch.isalpha() for ch in alt) and "://" not in alt:
                        k = norm(alt)
                        if k and k not in own:
                            other.add(k)
                db.executemany("INSERT OR IGNORE INTO names VALUES (?,?,1)", [(k, gid) for k in other])
        if n_cities < 100000:
            raise SystemExit(f"only {n_cities} places — refusing to replace the database")
        n_names = db.execute("SELECT COUNT(*) FROM names").fetchone()[0]
        db.execute("INSERT INTO meta VALUES ('source', 'GeoNames cities500, CC BY 4.0')")
        db.execute("INSERT INTO meta VALUES ('cities', ?)", (str(n_cities),))
        db.execute("INSERT INTO meta VALUES ('names', ?)", (str(n_names),))
        db.commit()
        db.execute("VACUUM")
        db.close()
        os.chmod(tmp, 0o644)
        os.replace(tmp, out_path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    print(f"{n_cities} cities, {n_names} names -> {out_path} ({os.path.getsize(out_path) // 1024} KB)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "cities.sqlite")
