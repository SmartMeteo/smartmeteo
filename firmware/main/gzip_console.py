"""Compress the console deterministically.

The obvious one-liner, gzip.compress(data, 9), stamps the CURRENT TIME into the archive header —
and that header is embedded in the firmware image, so the same source produced a different binary
on every build for that reason alone. mtime=0 removes it; writing through a fileobj also keeps the
original filename out of the header. Nothing about the served bytes changes.
"""
import gzip, io, sys

src, dst = sys.argv[1], sys.argv[2]
buf = io.BytesIO()
with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
    f.write(open(src, "rb").read())
open(dst, "wb").write(buf.getvalue())
