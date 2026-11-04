#!/usr/bin/env python3
"""City search for Smart Meteo devices — GET /v1/geo/search?q=<prefix>[&cc=DE][&n=6]

Answers {"results": [{"name", "country", "state", "lat", "lon", "tz", "pop", "elev"?}], "source": ...}, the shape
the firmware already reads from its fallback geocoder. Read-only over a SQLite file built by build_db.py;
listens on localhost behind nginx, which terminates TLS and limits the request rate. Stdlib only.
"""
import json
import os
import sqlite3
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import norm  # noqa: E402

DB = os.environ.get("GEO_DB", "/var/lib/smartmeteo-geo/cities.sqlite")
PORT = int(os.environ.get("GEO_PORT", "8081"))
SOURCE = "GeoNames (CC BY 4.0)"

# An exact name first, then a match on the place's own name before one on its names in other languages,
# then the bigger place: "Paris" is the French capital, "new yo" is New York.
QUERY = """
    SELECT c.name, c.country, c.state, c.lat, c.lon, c.tz, c.pop, c.elev,
           MAX(n.key = :q) AS exact, MIN(n.alt) AS alt
    FROM names n JOIN cities c ON c.id = n.id
    WHERE n.key >= :q AND n.key < :hi {cc}
    GROUP BY c.id
    ORDER BY exact DESC, alt ASC, c.pop DESC
    LIMIT :n
"""


def search(q: str, cc: str, n: int):
    key = norm(q)
    if len(key) < 2:
        return []
    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    try:
        sql = QUERY.format(cc="AND c.country = :cc" if cc else "")
        rows = con.execute(sql, {"q": key, "hi": key + "\U0010ffff", "cc": cc, "n": n}).fetchall()
    finally:
        con.close()
    # `elev` is omitted rather than sent as -9999: a consumer that forgets to check a sentinel
    # would place the device 9999 m below the sea, and the device cannot tell that from a height.
    out = []
    for r in rows:
        item = {"name": r[0], "country": r[1], "state": r[2], "lat": round(r[3], 5),
                "lon": round(r[4], 5), "tz": r[5], "pop": r[6]}
        if r[7] is not None and r[7] > -500:
            item["elev"] = r[7]
        out.append(item)
    return out


class Handler(BaseHTTPRequestHandler):
    server_version = "smartmeteo-geo/1.0"
    sys_version = ""

    def _send(self, code: int, body: dict, cache: str = "no-store") -> None:
        data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):  # noqa: N802
        url = urlsplit(self.path)
        if url.path != "/v1/geo/search":
            return self._send(404, {"error": "not found"})
        # max_num_fields raises ValueError rather than truncating, and this line used to sit outside
        # any try: a request with nine query parameters — which anybody can send — escaped do_GET,
        # dropped the connection (nginx turned it into 502) and wrote a full traceback to the journal.
        # An unauthenticated way to fill a disk on a box with no journal size limit. The cap itself is
        # worth keeping; it just has to answer instead of exploding.
        try:
            p = parse_qs(url.query, max_num_fields=8)
        except ValueError:
            return self._send(400, {"error": "too many parameters"})
        q = (p.get("q") or [""])[0][:64]
        cc = (p.get("cc") or [""])[0].upper()
        cc = cc if len(cc) == 2 and cc.isalpha() else ""
        try:
            n = max(1, min(10, int((p.get("n") or ["6"])[0])))
        except ValueError:
            n = 6
        try:
            results = search(q, cc, n)
        except sqlite3.Error as e:
            self.log_error("database: %s", e)
            return self._send(503, {"error": "unavailable"})
        # A day: the database changes once a month, and the same prefixes are typed over and over.
        self._send(200, {"results": results, "source": SOURCE}, "public, max-age=86400")

    # What people type is not logged: one line per request with the path and the status only.
    def log_request(self, code="-", size="-"):
        sys.stderr.write("%s %s %s\n" % (self.command, urlsplit(self.path).path, code))

    def log_message(self, fmt, *args):
        sys.stderr.write((fmt % args if args else fmt) + "\n")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
