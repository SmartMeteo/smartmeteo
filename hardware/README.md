# Hardware

Licensed under the CERN Open Hardware Licence v2 — Permissive (`../LICENSES/CERN-OHL-P-2.0.txt`).

## enclosure/

The enclosure for the v2 board: a front shell that holds the board and the e-ink panel, a rear
cover on four M2 screws into the board's inserts, a fold-out stand, a button key and two 10×3 mm
magnets for a fridge door.

- `release_v2.2/pla/stl/` — for FDM printing in PLA (fits tuned for that).
- `release_v2.2/sla/stl/`, `release_v2.2/sla/step/` — the same parts with fits for resin printing
  (JLC3DP 9600), plus four test keys with different clearances.
- `build_enclosure.py`, `build_jig.py` and the other scripts generate every part from scratch in
  Autodesk Fusion. They talk to Fusion through its local MCP server (`fmcp.py`); set the folder
  path at the top of `build_jig.py` and `export_print.py` to where this folder is on your machine.
- `build_jig.py` makes a jig that holds the panel and the board while the panel is glued on.

## board-v2/

The board itself (silkscreen "SmartMeteo v2.2"): schematic, bill of materials and Gerber
files. See `board-v2/README.md`.
