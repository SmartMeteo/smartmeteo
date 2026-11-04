"""Writes every part that gets printed to STL, from the open Fusion design.

Run it through Fusion's MCP server:  ./fmcp.py --script export_print.py
Files go to print/<pla|sla>/ next to this script; which of the two is taken from the design
itself (build_enclosure.py records the fit set it built with), so a folder never holds parts
built with the other set. The folder is not kept in git: everything in it is rebuilt from the
scripts in a minute.

The STLs are turned to the side they print on, so the slicer only has to drop them on the bed:
  front shell, coupons cut from it   screen side down (model -z)
  rear cover, stand/magnet coupons   outer face down (model +z)
  stand flap                         flat, OUTER face down, so its visible face carries the same bed
                                     texture as the cover's (owner, 2026-10-11: inner face down, the
                                     flap looked like a different part; he flipped it by hand)
  key                                standing on its back, tabs on the bed (model -y down; owner,
                                     2026-10-13): the flange reaches the back, so it is one flat face,
                                     and the pill lies in the printer's XY plane;
                                     only with BUTTON_STYLE "key" - the wall tongue is part of the shell
  clip                               on its side (model +x down): the finger bends along the layers
  turnbutton                         flat on its underside, as built (LATCH_STYLE "screw")
  jig base                           floor down
  jig lid                            plate down (model +z)
Each file reports how much of it lies on the bed: a part balanced on an edge would show at once.

Print aids (components named "PRN ...") are written into the file of the part they serve, as a
second shell in the same coordinates: the slicer then keeps them exactly where they were drawn.
  PRN Pocket support -> rear_cover.stl (stands in the stand pocket)
  PRN Magnet support -> rear_cover.stl (one comb in each magnet pocket)
"""
import adsk.core, adsk.fusion, os, struct

HERE = os.path.expanduser("~/smartmeteo/hardware/enclosure")   # this folder on your machine
# rotations to the printing side, as row-major 3x3 matrices applied to model coordinates
KEEP = (1, 0, 0, 0, 1, 0, 0, 0, 1)
FLIP = (1, 0, 0, 0, -1, 0, 0, 0, -1)          # 180 deg about x: model +z goes down
SIDE = (0, 0, 1, 0, 1, 0, -1, 0, 0)           # 90 deg about y: model +x goes down
BACK_DOWN = (1, 0, 0, 0, 0, -1, 0, 1, 0)      # 90 deg about x: model -y goes down
TURN = {"rear_cover": FLIP, "coupon_stand": FLIP, "coupon_magnet": FLIP, "jig_lid": FLIP, "key": BACK_DOWN,
        "stand_flap": FLIP, "clip": SIDE}
AIDS = {"PRN Pocket support": "rear_cover", "PRN Magnet support": "rear_cover"}   # print aid -> the part file it is appended to
PARTS = {  # component -> file
    "SM Front": "front_shell", "SM Rear cover": "rear_cover", "SM Stand": "stand_flap",
    "SM Button": "key", "SM Clips": "clip", "SM Turnbuttons": "turnbutton",
    "JIG base": "jig_base", "JIG lid": "jig_lid",
    "CPN latch corner": "coupon_latch_corner", "CPN ledge corner": "coupon_ledge_corner",
    "CPN key wall": "coupon_key_wall", "CPN stand": "coupon_stand", "CPN magnet": "coupon_magnet",
}


def turn_stl(path, m):
    """Rotate a binary STL in place; returns triangle count, size and the area lying on the bed."""
    with open(path, "rb") as f:
        data = bytearray(f.read())
    n = struct.unpack_from("<I", data, 80)[0]
    rot = lambda x, y, z: (m[0] * x + m[1] * y + m[2] * z, m[3] * x + m[4] * y + m[5] * z,
                           m[6] * x + m[7] * y + m[8] * z)
    tris = []
    for i in range(n):
        v = struct.unpack_from("<12f", data, 84 + i * 50)
        tris.append([rot(*v[0:3]), rot(*v[3:6]), rot(*v[6:9]), rot(*v[9:12])])
    zmin = min(p[2] for t in tris for p in t[1:])
    lo, hi = [1e9] * 3, [-1e9] * 3
    bed = 0.0
    for i, (nrm, a, b, c) in enumerate(tris):
        struct.pack_into("<12f", data, 84 + i * 50, *nrm, *a, *b, *c)
        for p in (a, b, c):
            for k in range(3):
                lo[k] = min(lo[k], p[k]); hi[k] = max(hi[k], p[k])
        if max(a[2], b[2], c[2]) - zmin < 0.01:
            ux, uy = b[0] - a[0], b[1] - a[1]
            vx, vy = c[0] - a[0], c[1] - a[1]
            bed += abs(ux * vy - uy * vx) / 2
    with open(path, "wb") as f:
        f.write(data)
    return n, [h - l for l, h in zip(lo, hi)], bed


def append_stl(path, extra):
    """Append the triangles of the binary STL `extra` to `path` (both still in model coordinates)."""
    with open(path, "rb") as f:
        a = bytearray(f.read())
    with open(extra, "rb") as f:
        b = f.read()
    na, nb = struct.unpack_from("<I", a, 80)[0], struct.unpack_from("<I", b, 80)[0]
    struct.pack_into("<I", a, 80, na + nb)
    with open(path, "wb") as f:
        f.write(bytes(a) + b[84:84 + 50 * nb])
    return nb


def run(_context: str):
    design = adsk.fusion.Design.cast(adsk.core.Application.get().activeProduct)
    attr = design.attributes.itemByName("smartmeteo", "print")
    if attr is None:
        print("the design carries no fit set: run build_enclosure.py first")
        return
    out = os.path.join(HERE, "print", attr.value)
    os.makedirs(out, exist_ok=True)
    em = design.exportManager

    def export(body, path):
        opt = em.createSTLExportOptions(body, path)
        opt.isBinaryFormat = True
        opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        em.execute(opt)

    occs = list(design.rootComponent.occurrences)
    present = {o.component.name for o in occs}
    for comp, name in PARTS.items():     # a part the design no longer has (the key, with the wall tongue):
        stale = os.path.join(out, name + ".stl")   # its old file would still be sliced
        if comp not in present and os.path.exists(stale):
            os.remove(stale)
            print("%s is not in the design: removed the old %s.stl" % (comp, name))
    trials = {"key_c" + c[len("CPN key c"):].replace(".", "") for c in present if c.startswith("CPN key c")}
    for f in sorted(os.listdir(out)):    # trial keys come and go with KEY_TRIALS: drop the ones no longer built
        if f.startswith("key_c") and f.endswith(".stl") and f[:-4] not in trials:
            os.remove(os.path.join(out, f))
            print("trial key %s is not in the design: removed" % f[:-4])
    for occ in occs:
        cname = occ.component.name
        name = PARTS.get(cname)
        if name is None and cname.startswith("CPN key c"):       # trial keys: "CPN key c0.21" -> key_c021
            name = "key_c" + cname[len("CPN key c"):].replace(".", "")
        if name is None:
            continue
        if not occ.transform2.isEqualTo(adsk.core.Matrix3D.create()):
            print("%s is moved or turned in the design (a preview pose?): not exported" % occ.component.name)
            continue
        body = occ.bRepBodies.item(0)          # coupons carry the key or flap as extra bodies, and the two
                                               # clips (turnbuttons) are one part: the first body only
        path = os.path.join(out, name + ".stl")
        export(body, path)
        for aid in occs:
            if AIDS.get(aid.component.name) == name:
                for k in range(aid.bRepBodies.count):
                    tmp = path[:-4] + "_aid.stl"     # Fusion insists on the .stl ending
                    export(aid.bRepBodies.item(k), tmp)
                    print("  + %s / %s, %d triangles" % (aid.component.name, aid.bRepBodies.item(k).name,
                                                         append_stl(path, tmp)))
                    os.remove(tmp)
        n, size, bed = turn_stl(path, TURN.get(name, BACK_DOWN if name.startswith("key_c") else KEEP))
        print("%-26s %6d triangles  %6.2f x %6.2f x %5.2f mm  on the bed %6.0f mm2"
              % (name + ".stl", n, *size, bed))
    print("written to", out)
