"""Shared by the builder and the server: how a place name becomes a search key."""
import re
import unicodedata

_SEP = re.compile(r"[\s\-‐-―'’`.,()/]+")


def norm(s: str) -> str:
    """'Frankfurt am Main' -> 'frankfurt am main', 'München' -> 'munchen', 'Санкт-Петербург' ->
    'санкт петербург'. Accents go, case goes, hyphens and apostrophes become spaces."""
    s = unicodedata.normalize("NFKD", s)
    s = "".join(ch for ch in s if not unicodedata.combining(ch))
    s = s.casefold().replace("ß", "ss")
    return _SEP.sub(" ", s).strip()
