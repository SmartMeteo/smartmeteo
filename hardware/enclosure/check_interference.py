"""Interference of the SM enclosure parts with every board part, the panel and the cell.
Run read-only:  ./fmcp.py --script check_interference.py --ro"""
import adsk.core, adsk.fusion


def run(_context: str):
    design = adsk.fusion.Design.cast(adsk.core.Application.get().activeProduct)
    root = design.rootComponent
    coll = adsk.core.ObjectCollection.create()
    # analyzeInterference only looks at the bodies that lie directly in the occurrences it is given,
    # not in their children - and the board model keeps 705 of its 706 bodies (every part on it) one
    # or more levels down. Until 2026-10-11 this script handed over the top level only, so the case was
    # compared with the bare PCB, the panel, the cell and the inserts, and with none of the parts on
    # the board (found when the key's preload on SW2 did not show up). Every occurrence that holds
    # bodies goes in now, at any depth.
    def add(o):
        if o.bRepBodies.count:
            coll.add(o)
        for ch in o.childOccurrences:
            add(ch)
    for o in root.occurrences:
        if o.component.name.startswith(("JIG ", "CPN ", "PRN ")):   # tools, test coupons and print aids
            continue                      # (the pocket support stands where the flap lies)
        if "Silkscreen" in o.component.name and o.component.name.startswith("Top"):
            continue                      # flat print, no volume
        add(o)
    attr = design.attributes.itemByName("smartmeteo", "cover_in")   # build_enclosure.py records it
    cover_in = float(attr.value) if attr else 8.89                     # (v2's, before it did)
    ii = design.createInterferenceInput(coll)
    ii.areCoincidentFacesIncluded = False
    res = design.analyzeInterference(ii)

    def owner(e):
        o = e.assemblyContext
        while o is not None and o.assemblyContext is not None:
            o = o.assemblyContext
        return (o.name if o else e.parentComponent.name).split("~")[0].split(":")[0]

    hits, meant = [], []
    for r in res:
        a, b = owner(r.entityOne), owner(r.entityTwo)
        if not (a.startswith("SM ") or b.startswith("SM ")):
            continue
        if "SM Inserts M2 (measured)" in (a, b) and "SMT INSERT" in a + b:
            continue                      # the measured stand-in and the board model's short insert are one part
        bb = r.interferenceBody.boundingBox
        if (bb.maxPoint.z - bb.minPoint.z) * 10 < 0.01:
            continue                      # zero thickness: parts touching face to face (insert on its pad)
        x0, x1, y0, y1, z0, z1 = (bb.minPoint.x * 10, bb.maxPoint.x * 10, bb.minPoint.y * 10,
                                  bb.maxPoint.y * 10, bb.minPoint.z * 10, bb.maxPoint.z * 10)
        line = "%s <-> %s: %.3f mm3 at x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]" % (
            a, b, r.interferenceBody.volume * 1000, x0, x1, y0, y1, z0, z1)
        why = expected(a, b, r.interferenceBody.volume * 1000, x0, x1, y0, y1, z0, z1, cover_in)
        (meant if why else hits).append(line + ("  (%s)" % why if why else ""))
    print("%d occurrences checked (nested ones included), %d interferences involving SM parts"
          % (coll.count, len(hits)))
    for h in hits:
        print("  " + h)
    print("%d by design:" % len(meant))
    for h in meant:
        print("  " + h)


def expected(a, b, vol, x0, x1, y0, y1, z0, z1, cover_in):
    """Overlaps the design asks for, each pinned to where it must be and how deep it may go, so an
    overlap of the same pair anywhere else, or deeper, still shows up as a hit."""
    pair = {a, b}
    # the v1 board model is ONE merged body ("Board"): its panel (drawn 1.8 thick, straight on the PCB) and
    # its short inserts are inside it, so the measured stand-ins overlap it there. Only stand-ins: a printed
    # part touching "Board" is still a hit
    if "Board" in pair and (pair & {"SM Panel GDEY042T81", "SM Inserts M2 (measured)"}):
        return "stand-in inside the merged v1 board model"
    # (the key no longer presses SW2: drawn out on its slot floors it stands BTN_GAP off the plunger)
    # crush ribs on the cover's edge reach 0.10 into the rebate wall (COVER_RIB_IN): eight of them,
    # 0.8 wide and ~0.9 tall, ~0.07 mm3 each; Fusion reports them as one body round the whole cover
    # the v1 model draws the inserts 4.12 tall (to z 4.21); the bosses end 0.05 over the measured 4.0 (z 4.14)
    if pair == {"Board", "SM Rear cover"} and z0 > 4.13 and z1 < 4.22 and vol < 4 * 1.6:
        return "boss ends over the v1 model's inserts, drawn 4.12 tall (measured 4.0)"
    if pair == {"SM Rear cover", "SM Front"} and z0 > cover_in and z1 < cover_in + 1.3 and vol < 8 * 0.1:
        return "cover crush ribs"
    # v0: the cover's four bosses (4.4, round the 2.7 holes) land 0.05 into the PCB, so the screws clamp it
    if pair == {"Board v0", "SM Rear cover"} and z0 > -0.06 and z1 < 0.01 and vol < 4 * 0.05 * 3.14 * 2.2 ** 2:
        return "cover bosses press the v0 board 0.05"
    return None
