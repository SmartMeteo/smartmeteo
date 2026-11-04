"""Gluing jig for the e-paper panel — generated into the open Fusion design.

Run it through Fusion's MCP server:  ./fmcp.py --script build_jig.py
Each run deletes the components it made before (names starting with "JIG ") and builds them again.
It reuses the board and panel numbers of build_enclosure.py, so the jig follows any change there;
the enclosure script never touches the jig and the other way round.

The jig is built where it works, round the board model: its floor is the panel's front face, so the
board and panel models sit in it exactly as they will on the bench. Hide the "SM " enclosure parts
to look at it; it overlaps them by nature.

  JIG base  a tray: the panel lies face down in the lower pocket, the PCB (parts up) drops into the
            upper pocket on top of the tape. Both pockets are concentric with the panel centred on the
            PCB, as the enclosure assumes. Push the panel and then the PCB into the corner marked with
            the triangle; the play in the pockets then does not show up as an offset.
  JIG lid   a plate with posts that land on bare PCB between the parts. It comes down on the rim of the
            base, and at that point the PCB sits exactly TAPE_PANEL above the panel, everywhere: the
            bond line is set by the jig, not by how hard one presses.

Bench order: panel face down, FPC out through the slot; tape on the panel's back, kept 5 mm off the
FPC edge (the driver chip is on the other side of the glass there, and nothing under it); PCB in;
lid on, press until it sits, hold half a minute; lift out by the finger notches; fold the FPC round
the PCB edge into P2. Glue the panel before the cell goes on: the posts land where the cell sits.

Print: the base as modelled, floor down (iron the pocket floor or lay paper on it, it is what the glass
rests on); the lid upside down, plate on the bed.
"""
import adsk.core, adsk.fusion, importlib, os, sys

SRC = os.path.expanduser("~/smartmeteo/hardware/enclosure")   # this folder on your machine
if SRC not in sys.path:
    sys.path.insert(0, SRC)
import build_enclosure as E
importlib.reload(E)

# ---------------------------------------------------------------- parameters --
FIT = 0.20                     # pocket to panel and to PCB, per side (PLA)
WALL = 4.0
FLOOR_T = 2.0
PANEL_STEP = E.PANEL_T         # the panel pocket is as deep as the glass; the PCB pocket starts there.
                               # Not deeper: with 0.33 mm tape the PCB would sit on the step, not on the tape
COG_RELIEF_DEPTH = 0.4         # the floor drops under the driver strip along the FPC edge
COG_RELIEF_W = E.COG_BAND + 0.5
FPC_X = (40.0, 86.0)           # FPC slot through the bottom wall; the flex fans out to ~37 mm near the glass
FPC_SHELF = 0.2                # the flex leaves the glass ~0.2..0.5 above the floor: a shelf to lie on
FPC_TONGUE = 20.0              # how far the shelf runs past the panel edge (the flex is 18.5 mm long)
NOTCH_Y, NOTCH_W, NOTCH_Z = 50.0, 16.0, 1.0   # finger notches in both side walls, down to FLOOR + NOTCH_Z
PART_CLEAR = 1.0               # lid plate above the tallest part (SCD41, 6.39 above the PCB)
LID_T = 3.0
POST_D = 4.0
# post spots: free of parts by 0.8 mm round a 4 mm post, picked from a 1 mm scan of the board model
POSTS = [(21, 29), (43, 29), (64, 29), (88, 22), (21, 46), (43, 46), (64, 46), (86, 49),
         (21, 63), (43, 63), (63, 62), (89, 63), (21, 82), (43, 80), (59, 78)]
PIN_D, PIN_H, PIN_CLEAR = 3.0, 3.0, 0.3

# ---------------------------------------------------------------- derived --
FLOOR = E.PANEL_FRONT                          # the glass rests here
PX0, PY0 = E.PANEL_X0 - FIT, E.PANEL_Y0 - FIT
PX1, PY1 = E.PANEL_X0 + E.PANEL_W + FIT, E.PANEL_Y0 + E.PANEL_H + FIT
BX0, BY0, BX1, BY1 = E.PCB_X0 - FIT, E.PCB_Y0 - FIT, E.PCB_X1 + FIT, E.PCB_Y1 + FIT
OX0, OY0, OX1, OY1 = BX0 - WALL, BY0 - WALL, BX1 + WALL, BY1 + WALL
RIM = E.PCB_TOP + 6.39 + PART_CLEAR            # lid plate underside = base rim
PINS = [(OX0 + WALL / 2, OY1 - WALL / 2), (OX1 - WALL / 2, OY0 + WALL / 2)]

NEW, CUT, JOIN = E.NEW, E.CUT, E.JOIN


def build_base(root):
    c = E.new_component(root, "JIG base")
    s = E.Plane(c, "z", FLOOR - FLOOR_T, "underside").sketch("outline")
    s.rrect(OX0, OY0, OX1, OY1, 3.0)
    body = E.extrude(s, s.profiles(), FLOOR - FLOOR_T, RIM, NEW, name="base").bodies.item(0)
    body.name = "Jig base"
    # tongue under the flex
    s = E.Plane(c, "z", FLOOR - FLOOR_T, "tongue").sketch("FPC tongue")
    s.rrect(FPC_X[0], E.PANEL_Y0 - FPC_TONGUE, FPC_X[1], OY0 + 1.0, 0)
    E.extrude(s, s.profiles(), FLOOR - FLOOR_T, FLOOR + FPC_SHELF, JOIN, body, "FPC tongue")
    # panel pocket, then PCB pocket above it
    s = E.Plane(c, "z", FLOOR, "floor").sketch("panel pocket")
    s.rrect(PX0, PY0, PX1, PY1, 0.3)
    E.extrude(s, s.profiles(), FLOOR, FLOOR + PANEL_STEP, CUT, body, "panel pocket")
    s = E.Plane(c, "z", FLOOR + PANEL_STEP, "step").sketch("PCB pocket")
    s.rrect(BX0, BY0, BX1, BY1, 0.3)
    E.extrude(s, s.profiles(), FLOOR + PANEL_STEP, RIM, CUT, body, "PCB pocket")
    # nothing may touch the driver chip on the glass ledge along the FPC edge
    s = E.Plane(c, "z", FLOOR, "floor (COG)").sketch("COG relief")
    s.rrect(PX0, PY0, PX1, E.PANEL_Y0 + COG_RELIEF_W, 0)
    E.extrude(s, s.profiles(), FLOOR, FLOOR - COG_RELIEF_DEPTH, CUT, body, "COG relief")
    # FPC slot through the bottom wall, down to the shelf
    s = E.Plane(c, "z", FLOOR + FPC_SHELF, "shelf").sketch("FPC slot")
    s.rrect(FPC_X[0], E.PANEL_Y0 - FPC_TONGUE - 1.0, FPC_X[1], PY0 + 0.01, 0)
    E.extrude(s, s.profiles(), FLOOR + FPC_SHELF, RIM + 1.0, CUT, body, "FPC slot")
    # finger notches to lift the glued pair out
    for x0, x1 in ((OX0 - 1.0, BX0 + 0.01), (BX1 - 0.01, OX1 + 1.0)):
        s = E.Plane(c, "z", FLOOR + NOTCH_Z, "notch").sketch("finger notch")
        s.rrect(x0, NOTCH_Y - NOTCH_W / 2, x1, NOTCH_Y + NOTCH_W / 2, 0)
        E.extrude(s, s.profiles(), FLOOR + NOTCH_Z, RIM + 1.0, CUT, body, "finger notch")
    # alignment pins for the lid
    s = E.Plane(c, "z", RIM, "rim").sketch("lid pins")
    for x, y in PINS:
        s.circle(x, y, PIN_D)
    E.extrude(s, s.profiles(), RIM, RIM + PIN_H, JOIN, body, "lid pins")
    # the reference corner: a triangle engraved in the rim, pointing at it
    k = 1.6                              # clear of the R3 outer corner
    s = E.Plane(c, "z", RIM, "rim (mark)").sketch("corner mark")
    leg = WALL - k - 0.2                 # stays inside the 4 mm wall, short of the pocket
    s.poly([(OX1 - k, OY1 - k), (OX1 - k - leg, OY1 - k), (OX1 - k, OY1 - k - leg)])
    E.extrude(s, s.profiles(), RIM, RIM - 0.6, CUT, body, "corner mark")
    return c, body


def build_lid(root):
    c = E.new_component(root, "JIG lid")
    s = E.Plane(c, "z", RIM, "lid underside").sketch("plate")
    s.rrect(OX0, OY0, OX1, OY1, 3.0)
    body = E.extrude(s, s.profiles(), RIM, RIM + LID_T, NEW, name="lid").bodies.item(0)
    body.name = "Jig lid"
    s = E.Plane(c, "z", RIM, "lid underside (posts)").sketch("posts")
    for x, y in POSTS:
        s.circle(x, y, POST_D)
    E.extrude(s, s.profiles(), RIM, E.PCB_TOP, JOIN, body, "posts")
    s = E.Plane(c, "z", RIM + LID_T, "lid top").sketch("pin holes")
    for x, y in PINS:
        s.circle(x, y, PIN_D + 2 * PIN_CLEAR)
    E.extrude(s, s.profiles(), RIM + LID_T, RIM - 0.1, CUT, body, "pin holes")
    return c, body


def run(_context: str):
    app = adsk.core.Application.get()
    root = adsk.fusion.Design.cast(app.activeProduct).rootComponent
    for occ in [o for o in root.occurrences if o.component.name.startswith("JIG ")]:
        occ.deleteMe()
    _, base = build_base(root)
    _, lid = build_lid(root)

    probes = [  # (body, x, y, z, expected inside, what)
        (base, 53.0, 50.0, FLOOR - 1.0, True, "floor"),
        (base, 53.0, 50.0, FLOOR + 0.5, False, "panel pocket"),
        (base, 53.0, E.PANEL_Y0 + 2.0, FLOOR - 0.2, False, "relief under the driver strip"),
        (base, 53.0, E.PANEL_Y0 + COG_RELIEF_W + 1.0, FLOOR - 0.2, True, "floor past the relief"),
        (base, (PX0 + BX0) / 2 - 0.02, 50.0, FLOOR + 1.0, True, "panel pocket wall"),
        (base, (PX0 + BX0) / 2, 50.0, FLOOR + PANEL_STEP + 0.5, False, "PCB pocket over the step"),
        (base, BX0 - 0.1, 30.0, 0.0, True, "PCB pocket wall"),
        (base, 60.0, E.PCB_Y0 - 2.0, FLOOR + 1.0, False, "FPC slot"),
        (base, 60.0, E.PANEL_Y0 - 10.0, FLOOR + FPC_SHELF - 0.1, True, "FPC shelf"),
        (base, 30.0, E.PCB_Y0 - 2.0, FLOOR + 1.0, True, "wall beside the slot"),
        (base, OX0 + 1.0, NOTCH_Y, 0.0, False, "finger notch"),
        (base, OX0 + 1.0, 30.0, RIM - 0.5, True, "rim"),
        (base, PINS[0][0], PINS[0][1], RIM + 1.5, True, "lid pin"),
        (base, OX1 - 2.2, OY1 - 2.2, RIM - 0.3, False, "corner mark"),
        (lid, 53.0, 50.0, RIM + 1.0, True, "lid plate"),
        (lid, POSTS[0][0], POSTS[0][1], 1.0, True, "post"),
        (lid, POSTS[0][0], POSTS[0][1], E.PCB_TOP - 0.05, False, "post ends at the PCB"),
        (lid, PINS[0][0], PINS[0][1], RIM + 1.5, False, "pin hole"),
    ]
    bad = []
    for b, x, y, z, want, what in probes:
        got = b.pointContainment(adsk.core.Point3D.create(x / 10, y / 10, z / 10)) != \
            adsk.fusion.PointContainment.PointOutsidePointContainment
        if got != want:
            bad.append(what)

    # overlap with the board and the panel models: Fusion's interference analysis (seconds), not a
    # boolean per board body (the first version did ~1200 of them and ran for over 15 minutes)
    design = adsk.fusion.Design.cast(app.activeProduct)
    coll = adsk.core.ObjectCollection.create()
    for o in root.occurrences:
        n = o.component.name
        if (n.startswith("SM ") and n not in ("SM Panel GDEY042T81", "SM Inserts M2 (measured)")) \
                or n.startswith("CPN ") \
                or (n.startswith("Top") and "Silkscreen" in n):
            continue
        coll.add(o)
    ii = design.createInterferenceInput(coll)
    ii.areCoincidentFacesIncluded = False
    hits, touches = [], []
    for r in design.analyzeInterference(ii):
        names = []
        for e in (r.entityOne, r.entityTwo):
            o = e.assemblyContext
            while o is not None and o.assemblyContext is not None:
                o = o.assemblyContext
            names.append((o.name if o else e.parentComponent.name).split("~")[0].split(":")[0])
        if not any(n.startswith("JIG ") for n in names):
            continue
        bb = r.interferenceBody.boundingBox
        if (bb.maxPoint.z - bb.minPoint.z) * 10 < 0.01:
            touches.append("%s on %s" % tuple(names))   # the posts landing on the PCB, as designed
            continue
        hits.append("%s x %s (%.4f mm3)" % (names[0], names[1], r.interferenceBody.volume * 1000))

    def bb(b):
        mn, mx = b.boundingBox.minPoint, b.boundingBox.maxPoint
        return "%.1f x %.1f x %.1f mm" % ((mx.x - mn.x) * 10, (mx.y - mn.y) * 10, (mx.z - mn.z) * 10)
    print("base: %s | lid: %s" % (bb(base), bb(lid)))
    print("floor z %.2f, step %.2f, rim %.2f, posts end at %.2f (PCB top); bond line %.2f"
          % (FLOOR, FLOOR + PANEL_STEP, RIM, E.PCB_TOP, E.PCB_BOT - E.PANEL_BACK))
    print("probes: %d of %d as designed%s" % (len(probes) - len(bad), len(probes),
                                               ("; WRONG: " + ", ".join(bad)) if bad else ""))
    print("overlap with the board and panel: " + ("; ".join(hits) if hits else "none")
          + ("  (face contact: %s)" % ", ".join(touches) if touches else ""))
