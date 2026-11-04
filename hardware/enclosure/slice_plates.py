#!/usr/bin/env python3
"""Slices the printed parts into three ready plates for the Kobra 3 V2, with Anycubic Slicer Next's
command line. Run export_print.py in Fusion first; this reads its STLs.

    ./slice_plates.py            # pla (the folder export_print.py wrote)
    ./slice_plates.py sla        # would read print/sla, but SLA is ordered, not sliced here

Plates, in the order to print them:
  1 coupons   the fit coupons with the real key and stand flap: prove the fits first
  1 stand     the stand coupon, with bed-only supports in its pocket
  1r retest   second round: only what changed after the first print
  1t tongue   two copies of the top-wall coupon with the wall tongue
  1s screw    the bottom corner with a turnbutton's boss and notch, and one turnbutton
  2 front     front shell, no supports
  2 cover     rear cover, supports from the bed only (the stand pocket)
  3, 4 jig    the gluing jig, base and lid, at 0.2 mm layers: no fine fits there, only a flat floor
  5 full      the whole case in one job
  6 front     the sixth print: front shell and key only (the cover and the stand are accepted)
  1p pull     two copies of the bottom corner with a drawn-back whisker, its hood and its clip's hole
  1k key      the top-wall coupon with the key's tab slots, and the key
  6 cover     the rear cover alone (only for the lowered LED turners)
  7 all       front, key and rear cover in one job
  8 key       two keys, nothing else (the 2026-10-12 shell takes the 2026-10-13 key)
  9 full      the whole case (front, cover, flap, key) and a spare key, in one job

The 3MFs land next to the STLs. Open one in Anycubic Slicer Next and send it to the printer from
there; the slicer asks which ACE slot to use, pick the white spool.

Settings: the printer is the stock "Anycubic Kobra 3 V2 0.4 nozzle". The filament is the owner's
own preset "SUNLU PLA pla @Anycubic Kobra 3 V2 0.4 nozzle" (flow 0.96, 210/220 C, bed 60 C), used
as it is except the colour. The process starts from the stock "0.12mm High Quality" and changes
only what the parts need (PROCESS below, reasons beside each line). Owner's printer and spool:
Anycubic Kobra 3 V2 Combo, SUNLU PLA+ 2.0, white (2026-10-10).
"""
import json, os, subprocess, sys, tempfile, zipfile, re

HERE = os.path.dirname(os.path.abspath(__file__))
APP = "/Applications/AnycubicSlicerNext.app/Contents/MacOS/AnycubicSlicerNext"
DATA = os.path.expanduser("~/Library/Application Support/AnycubicSlicerNext")
MACHINE = os.path.join(DATA, "system/Anycubic/machine/Anycubic Kobra 3 V2 0.4 nozzle.json")
FILAMENT_NAME = "SUNLU PLA pla @Anycubic Kobra 3 V2 0.4 nozzle"
WHITE = "#FFFFFF"

PROCESS = {
    "type": "process", "from": "User", "is_custom_defined": "0",
    "inherits": "0.12mm High Quality @Anycubic Kobra 3 V2 0.4 nozzle",
    "initial_layer_print_height": "0.2",
    "wall_loops": "4",                   # the 2 mm walls come out solid
    "wall_generator": "arachne",         # prints the 0.6 USB web and the 0.8 latches as real lines
    "top_shell_thickness": "0.8", "top_shell_layers": "7",
    "bottom_shell_thickness": "0.6", "bottom_shell_layers": "5",
    "sparse_infill_density": "25%", "sparse_infill_pattern": "gyroid",
    "elefant_foot_compensation": "0.15",  # keeps the bezel edge and the lowest fits true
    "xy_hole_compensation": "0",         # the PLA fit set already allows for small holes
    "xy_contour_compensation": "0",
    "seam_position": "back",
    "enable_support": "0",               # no part needs it (overhang check in Fusion, 2026-10-10)
    "bridge_speed": "30",
}
PLATES = [  # name, STL files, extra process settings
    ("plate1_coupons", ["coupon_latch_corner", "coupon_ledge_corner", "coupon_key_wall",
                        "coupon_magnet", "key", "stand_flap"],
     {"brim_type": "outer_only", "brim_width": "3"}),        # small footprints, tall pieces
    # the stand coupon carries the pocket, so it gets the cover's bed-only supports; the flap must
    # not (they would grow under its stubs), hence a plate of its own
    ("plate1_stand", ["coupon_stand"],
     {"brim_type": "outer_only", "brim_width": "3", "enable_support": "1", "support_type": "normal(auto)",
      "support_on_build_plate_only": "1", "support_threshold_angle": "30"}),
    # second round of coupons, 2026-10-10: only what changed after the first print (the rail latch,
    # the looser key hole, the taller key); print it with plate1_stand and reuse the printed flap
    ("plate1r_retest", ["coupon_latch_corner", "coupon_key_wall", "key"],
     {"brim_type": "outer_only", "brim_width": "3"}),
    # the bending-tab latch again, 2026-10-11 (owner: "try it, maybe it is better"): two copies of the
    # latch corner, one to fit the board edge, one to snap ten times and see whether it cracks
    ("plate1h_hook", ["coupon_latch_corner", "coupon_latch_corner"],
     {"brim_type": "outer_only", "brim_width": "3"}),
    # v4, 2026-10-11: the hanging clips in their wall slots, and the key resting on SW2 in its well
    ("plate1c_clip", ["coupon_latch_corner", "clip", "clip", "coupon_key_wall", "key"],
     {"brim_type": "outer_only", "brim_width": "3"}),
    # the wall tongue, 2026-10-12: two copies of the top-wall coupon, one to look at the slits, one to
    # press a hundred times. No supports on purpose: the tongue's first layer is tied across the end slit
    ("plate1t_tongue", ["coupon_key_wall", "coupon_key_wall"],
     {"brim_type": "outer_only", "brim_width": "3"}),
    # the turnbutton on an M2 screw, 2026-10-12: the corner with its boss, pilot and wall notch, and a turnbutton
    ("plate1s_screw", ["coupon_latch_corner", "turnbutton"],
     {"brim_type": "outer_only", "brim_width": "3"}),
    ("plate2_front", ["front_shell"], {"brim_type": "no_brim"}),
    # the stand pocket's floor is a 22 mm bridge when the cover prints outer face down; the first
    # print sagged and the flap would not go in (owner, 2026-10-10). Supports from the bed only: they
    # fill the pocket and the magnet pockets, nothing grows on the part itself.
    ("plate2_cover", ["rear_cover"],
     {"brim_type": "no_brim", "enable_support": "1", "support_type": "normal(auto)",
      "support_on_build_plate_only": "1", "support_threshold_angle": "30"}),
    # the jig has no fine fits, only a flat floor: 0.2 mm layers. Only the base is ironed (the
    # panel lies on its pocket floor); ironing the lid's plate would add hours for nothing.
    ("plate3_jig_base", ["jig_base"],
     {"brim_type": "no_brim", "ironing_type": "top",
      "layer_height": "0.2", "top_shell_layers": "4", "bottom_shell_layers": "3"}),
    ("plate4_jig_lid", ["jig_lid"],
     {"brim_type": "no_brim", "layer_height": "0.2", "top_shell_layers": "4", "bottom_shell_layers": "3"}),
    # the whole jig in one job (owner, 2026-10-13), the settings of the two plates above: the base ironed,
    # the lid not - per part, see PER_PART (the CLI has no per-object switch, the project is edited)
    ("plate16_jig", ["jig_base", "jig_lid"],
     {"brim_type": "no_brim", "ironing_type": "top",
      "layer_height": "0.2", "top_shell_layers": "4", "bottom_shell_layers": "3"}),
    # the whole case in one job (owner, 2026-10-10: printed while nobody is home, so nobody clears the
    # bed between jobs). Owner's call: automatic supports wherever the slicer wants them, the normal
    # (grid) kind, not tree — the stand pocket, the magnet pockets and the USB and key openings sagged
    # or would sag. Threshold 20 is the profile's own. The base is opened from the profile's 0.2 mm
    # gap to 2 mm: at 0.2 it prints almost solid, and nothing that solid comes out of the stand pocket
    # or the 0.6 mm web round the USB opening; the dense interface under the surface stays. A brim
    # tight against every part (gap 0) so the big flat pieces cannot lift. The flap prints WITHOUT
    # supports: they grow under its hinge stubs, and a stub that has to be snapped off a support no
    # longer snaps into its seat. Per-object settings have no command-line switch, so NO_SUPPORT is
    # applied by editing the project and slicing it a second time (see main()).
    ("plate5_full", ["front_shell", "rear_cover", "stand_flap", "key", "clip", "clip",
                     "turnbutton", "turnbutton"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_on_build_plate_only": "0", "support_threshold_angle": "20",
      "support_base_pattern_spacing": "2"}),
    # the sixth print, 2026-10-12: the front shell and the separate key, nothing else. The cover and the stand
    # were accepted as printed (owner: the cover is not reprinted; its four turnbutton fingers get snipped off,
    # the whiskers now stand where they were). Same settings as plate5_full. Built first with the long rails,
    # then (same day, before printing) with the drawn-back whiskers, LATCH_STYLE "pull".
    # Support style "snug" since the whiskers: the default grid closes the two prongs' supports into one block
    # that reaches under the bar (blockers stop overhangs, not that expansion), where it could only come out
    # sideways past the bar. Snug follows the prongs.
    ("plate6_front", ["front_shell", "key"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_style": "snug", "support_on_build_plate_only": "0",
      "support_threshold_angle": "20", "support_base_pattern_spacing": "2"}),
    # everything in one job (owner, 2026-10-12): the front with the key and the rear cover with the lowered
    # LED turners. Settings of plate6_front (snug supports, for the whiskers' prongs); the cover keeps its
    # own combs in the stand and magnet pockets, and the blockers of both plates.
    ("plate7_all", ["front_shell", "key", "rear_cover"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_style": "snug", "support_on_build_plate_only": "0",
      "support_threshold_angle": "20", "support_base_pattern_spacing": "2"}),
    # the whole new case in one job (owner, 2026-10-13): front, rear cover, stand flap and key, plus a second
    # key for the shell printed on 2026-10-12. Settings of plate7_all; the flap and the keys print without
    # supports (as on plate5_full: support under the flap's stubs spoils their snap)
    ("plate9_full", ["front_shell", "rear_cover", "stand_flap", "key", "key"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_style": "snug", "support_on_build_plate_only": "0",
      "support_threshold_angle": "20", "support_base_pattern_spacing": "2"}),
    # the key alone, two copies (2026-10-13): the shell printed on 2026-10-12 fits the new key unchanged,
    # so only the key is printed again. On the flat side of its pill, no supports
    ("plate8_key", ["key", "key"], {"brim_type": "outer_only", "brim_width": "3"}),
    # the rear cover alone, as on plate5_full: printed again only for the lowered LED turners (2026-10-12)
    ("plate6_cover", ["rear_cover"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_on_build_plate_only": "0", "support_threshold_angle": "20",
      "support_base_pattern_spacing": "2"}),
    # the nail catch at the stand's foot (2026-10-13): the ramp is in the cover, the hollow in the flap, so
    # both are printed again. Settings of plate6_cover; the flap without supports, as on plate9_full
    ("plate10_stand", ["rear_cover", "stand_flap"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_on_build_plate_only": "0", "support_threshold_angle": "20",
      "support_base_pattern_spacing": "2"}),
    # one job to try it all (owner, 2026-10-13): the cover with the nail ramp, the flap with the nail hollow and
    # two keys with the plunger channel, for the front shell printed on 2026-10-13. Settings of plate10_stand;
    # the flap and the keys without supports
    ("plate11_back", ["rear_cover", "stand_flap", "key", "key"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_on_build_plate_only": "0", "support_threshold_angle": "20",
      "support_base_pattern_spacing": "2"}),
    # two flat-backed keys again (2026-10-13, the channel kept them pressed), now standing on their back with the
    # tabs on the bed (owner): one for the final case, one for the second. Brim, no supports
    ("plate12_keys", ["key", "key"], {"brim_type": "outer_only", "brim_width": "3"}),
    # one key, standing on its tabs. 2026-10-13: 0.24 in the hole, then the four dotted trial keys
    # (plate14_key_trials, 0.21 / 0.18 / 0.15 / 0.12); the three-dot 0.15 was "perfect" and is the key now
    ("plate13_key", ["key"], {"brim_type": "outer_only", "brim_width": "3"}),
    # the released case (enclosure-v2.2): front, rear cover, stand flap and ONE key - plate9_full carried a
    # second key for the shell of 2026-10-12, and the release is one case (owner, 2026-10-13). Same settings,
    # blockers and unsupported parts as plate9_full; a spare key is plate13_key
    ("plate15_case", ["front_shell", "rear_cover", "stand_flap", "key"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_style": "snug", "support_on_build_plate_only": "0",
      "support_threshold_angle": "20", "support_base_pattern_spacing": "2"}),
    # the case for the first board (BOARD = "v1" in build_enclosure.py, 2026-10-13): front, rear cover, stand flap and
    # key, as plate15_case. Its STLs are in print/v1_pla:  ./slice_plates.py v1_pla plate17_v1_case plate13_key
    ("plate17_v1_case", ["front_shell", "rear_cover", "stand_flap", "key"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_style": "snug", "support_on_build_plate_only": "0",
      "support_threshold_angle": "20", "support_base_pattern_spacing": "2"}),
    # the case for the oldest board (BOARD = "v0" in build_enclosure.py, 2026-10-14): front, rear cover and stand flap,
    # as plate17 but no key (its switch's own stem is the button). STLs in print/v0_pla:
    #   ./slice_plates.py v0_pla plate18_v0_case
    ("plate18_v0_case", ["front_shell", "rear_cover", "stand_flap"],
     {"brim_type": "outer_only", "brim_width": "4", "brim_object_gap": "0", "enable_support": "1",
      "support_type": "normal(auto)", "support_style": "snug", "support_on_build_plate_only": "0",
      "support_threshold_angle": "20", "support_base_pattern_spacing": "2"}),
    # the drawn-back whisker, 2026-10-12: two copies of the bottom corner, one to try against the board with a
    # paperclip, one to draw back fifty times. Supports on: the prongs' flat undersides over the board's place
    # need them (they stand on the bezel and come out from the open back); blockers keep them from under the
    # bar, whose first layer is tied across the end gap and prints as a bridge, and from under the hood.
    ("plate1p_pull", ["coupon_latch_corner", "coupon_latch_corner"],
     {"brim_type": "outer_only", "brim_width": "3", "enable_support": "1", "support_type": "normal(auto)",
      "support_style": "snug", "support_on_build_plate_only": "0", "support_threshold_angle": "20",
      "support_base_pattern_spacing": "2"}),
    # the key in tab slots, 2026-10-12: the top-wall coupon and the key, to try the key in its slots in
    # a few minutes before the whole front. Supports as on plate6_front: in the key hole, not in the LED window
    ("plate1k_key", ["coupon_key_wall", "key"],
     {"brim_type": "outer_only", "brim_width": "3", "enable_support": "1", "support_type": "normal(auto)",
      "support_style": "snug", "support_on_build_plate_only": "0", "support_threshold_angle": "20",
      "support_base_pattern_spacing": "2"}),
]
OPTIONAL = {"key", "clip", "turnbutton"}   # parts a build may not have (no key with the wall tongue; clips or
                                           # turnbuttons, by LATCH_STYLE): left out, not an error
NO_SUPPORT = {"plate5_full": ["stand_flap", "clip", "turnbutton"], "plate6_front": ["key"], "plate1k_key": ["key"],
              "plate7_all": ["key"], "plate9_full": ["stand_flap", "key"],
              "plate10_stand": ["stand_flap"], "plate11_back": ["stand_flap", "key"],
              "plate15_case": ["stand_flap", "key"],
              "plate17_v1_case": ["stand_flap", "key"],
              "plate18_v0_case": ["stand_flap"]}   # parts that keep supports off on a plate that has them on
PER_PART = {"plate16_jig": {"jig_lid": {"ironing_type": "no ironing"}}}   # any other per-part process setting
# Support blockers, as boxes in the part's own (model, mm) coordinates: x0, x1, y0, y1, z0, z1.
# Automatic supports put columns under the two upper stops (the hooks over the PCB's top edge, on the
# insert axes x 12.57 and 93.98), standing exactly where the board-and-screen edge slides in and
# fused to the stop's underside, which is a 0.2 mm fit. The ledge coupon printed those stops with
# no support and the owner accepted them (2026-10-10), so they stay unsupported. The boxes reach
# from the case face to just above the stops and stop short of the key opening (x from 21.5).
# The stand pocket in the cover has its own support, drawn in Fusion (build_pocket_support) and carried
# in rear_cover.stl as a second shell: the slicer's support there printed as one solid block that only
# came out by prying, and prying bent the pocket's rim (owner, 2026-10-11). A blocker over the whole
# pocket keeps the slicer from filling the drawn support's gaps. The cover's STL is turned outer face
# down (model y and z negated), and blockers are written in the STL's coordinates, hence the signs.
# The magnet pockets got the same treatment after they printed stringy (owner, 2026-10-11).
# The clip slots in the bottom wall (2026-10-11): the slicer stood 12 mm support towers in the FPC room
# to hold their ceilings, and pushed support into the slots themselves, where it could never be picked
# out and the clip's tab would not go in. A slot's ceiling is a 1.1 mm overhang held on three sides:
# it prints without help.
# The turnbutton notches (2026-10-12) sit where the clip slots were, so the same two boxes cover them: the
# notch ceiling rises 45 deg and prints unsupported, and support in a 1 mm notch would never come out.
# The wall tongue (2026-10-12): its slits are 0.6, and support in them could never come out. Their
# ceilings print as bridges, the tongue's first layer is tied across the end slit, the nub's underside rises
# at 45 deg: nothing there wants the slicer's help.
# 2026-10-12, owner's call: supports UNDER the wall tongue after all ("it will sag"). The tongue's blocker is
# gone and the left stop's box ends at x 16.3 (it reached over the tongue's root). Known cost, told to him
# before: the slicer puts three dense interface layers in the 0.6 bottom slit, one layer under the tongue,
# along its whole length; if that cannot be cleared out, the tongue is glued to the wall below.
BLOCKERS = {"plate5_full": {"front_shell": [(5.0, 16.3, 86.0, 93.0, -5.1, 1.6),
                                            (87.0, 101.6, 86.0, 93.0, -5.1, 1.6),
                                            (6.9, 18.0, 7.0, 10.5, -5.1, 8.6),      # clip slots / turnbutton notches
                                            (88.5, 99.6, 7.0, 10.5, -5.1, 8.6)],
                            "rear_cover": [(65.8, 88.2, -70.8, -9.6, -11.1, -5.4),      # stand pocket
                                           (47.8, 58.8, -89.7, -78.7, -11.1, -7.6),     # magnet pockets
                                           (47.8, 58.8, -21.6, -10.6, -11.1, -7.6)]},
            # The drawn-back whiskers (LATCH_STYLE "pull", 2026-10-12). Each bar's first layer is tied across
            # the end gap into the side wall, so its underside prints as a bridge 4 mm over the FPC room's floor
            # and 0.5 over its clip's boss: support there would stand on the boss and fill the clip's hole.
            # The hood's underside rises 45 deg out of the side wall and is flat only over the bar's end (0.7):
            # it prints without, and support would wedge between hood and bar. The prongs' flat undersides
            # over the board's place DO get support (y from 12.42 on is left open): they reach up to 1.4 over
            # the board and would sag. Boxes: bar from just past its free end to its root block, y from behind
            # the root fillet to the bar's face; hood from inside the side wall to past its flat.
            # (The long rails of earlier the same day had one box each over bar and tooth.)
            "plate6_front": {"front_shell": [(5.0, 20.0, 86.0, 93.0, -5.1, 1.6),
                                             (87.0, 101.6, 86.0, 93.0, -5.1, 1.6),
                                             (7.38, 20.18, 10.89, 12.39, -5.1, 0.2),     # left bar
                                             (6.88, 8.58, 9.39, 12.42, -5.1, 3.05),       # left hood
                                             (86.37, 99.17, 10.89, 12.39, -5.1, 0.2),    # right bar
                                             (97.97, 99.67, 9.39, 12.42, -5.1, 3.05),     # right hood
                                             # The owner wants supports in the key hole and the USB opening,
                                             # the stand pocket and the magnet pockets (2026-10-12; the cover
                                             # carries its own printed combs there). The two LED windows are
                                             # left to bridge: a 2 mm round hole needs none, and support could
                                             # not be picked out of the tube the light pipe goes into. The key's
                                             # tab slots have 45 deg ceilings and need none either.
                                             (39.0, 41.5, 90.2, 92.8, -5.1, 3.2),         # LED1 window
                                             (99.2, 101.7, 34.0, 36.4, -5.1, 3.2)]},      # CHRG window
            "plate7_all": {},   # filled below from plate6_front and plate6_cover
            "plate1k_key": {"coupon_key_wall": [(169.0, 171.5, 90.2, 92.8, -5.1, 3.2)]},   # LED1 window, +130 in x
            "plate6_cover": {"rear_cover": [(65.8, 88.2, -70.8, -9.6, -11.1, -5.4),      # as on plate5_full
                                            (47.8, 58.8, -89.7, -78.7, -11.1, -7.6),
                                            (47.8, 58.8, -21.6, -10.6, -11.1, -7.6)]},
            # the same two boxes on the coupon, which is the left corner moved 130 mm along x
            "plate1p_pull": {"coupon_latch_corner": [(137.38, 150.18, 10.89, 12.39, -5.1, 0.2),
                                                     (136.88, 138.58, 9.39, 12.42, -5.1, 3.05)]}}
BLOCKERS["plate7_all"] = {"front_shell": BLOCKERS["plate6_front"]["front_shell"],
                          "rear_cover": BLOCKERS["plate6_cover"]["rear_cover"]}
BLOCKERS["plate9_full"] = BLOCKERS["plate7_all"]
BLOCKERS["plate10_stand"] = {"rear_cover": BLOCKERS["plate6_cover"]["rear_cover"]}
BLOCKERS["plate11_back"] = BLOCKERS["plate10_stand"]
BLOCKERS["plate15_case"] = BLOCKERS["plate9_full"]
# v1 (2026-10-13): plate6_front's whisker and LED boxes on v1's walls, which stand 0.14 further in on the left,
# 0.17 on the right, and 0.18 further up at the bottom edge (its board is smaller); since 2026-10-14 v1 has v2's
# ledges again, and v2's two boxes under them cover v1's (x 9.7..15.7 and 91.0..97.0, y from 89.24). The cover
# is 2 mm deeper (cell 10 mm), so its boxes go 2 mm further down in the STL's negated z, and the stand pocket
# 0.18 along y with the bottom edge.
BLOCKERS["plate17_v1_case"] = {
    "front_shell": [(5.0, 20.0, 86.0, 93.0, -5.1, 1.6),         # under the ledges
                    (87.0, 101.6, 86.0, 93.0, -5.1, 1.6),
                    (7.52, 20.32, 11.07, 12.57, -5.1, 0.2),     # left bar
                    (7.02, 8.72, 9.57, 12.60, -5.1, 3.05),      # left hood
                    (86.20, 99.00, 11.07, 12.57, -5.1, 0.2),    # right bar
                    (97.80, 99.50, 9.57, 12.60, -5.1, 3.05),    # right hood
                    (39.0, 41.5, 90.2, 92.8, -5.1, 3.2),        # LED1 window
                    (99.2, 101.7, 34.0, 36.4, -5.1, 3.2)],      # CHRG window
    "rear_cover": [(65.8, 88.2, -70.97, -9.77, -13.1, -7.4),    # stand pocket
                   (47.8, 58.8, -89.7, -78.7, -13.1, -9.6),     # magnet pockets
                   (47.8, 58.8, -21.6, -10.6, -13.1, -9.6)]}

# v0 (2026-10-14): no ledges and no whiskers. The two LED windows bridge as on v1, and so does the microSD slot
# in the right wall (14 long, 2.2 high: support in it could only be picked out through the slot), and the dish
# round it: its top is a 1.2 overhang the slicer filled with support from the dish's floor up, on the outer face.
# The SCD41's vents too (1.2 wide). The stem's hole and the switch's notch are open to the rim, no ceiling. The
# cover's boxes are v1's moved to v0's stand pocket (y 12.59..73.19) and magnets (y 90.9 and 22.8), its back at 12.80.
BLOCKERS["plate18_v0_case"] = {
    "front_shell": [(35.5, 38.0, 101.2, 103.8, -5.1, 3.2),     # LED1 window
                    (98.9, 101.4, 47.4, 49.9, -5.1, 3.2),      # CHRG window
                    (98.9, 101.6, 56.1, 75.0, -5.1, 5.0),      # microSD slot and its dish
                    (5.0, 7.6, 73.4, 87.6, -5.1, 8.4)],        # SCD41 vents
    "rear_cover": [(65.8, 88.2, -73.39, -12.39, -13.0, -7.3),  # stand pocket
                   (47.8, 58.8, -96.4, -85.4, -13.0, -9.5),    # magnet pockets
                   (47.8, 58.8, -28.3, -17.3, -13.0, -9.5)]}


def slice_info(path):
    """Print time and filament the slicer predicts, read from the 3MF it wrote."""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        cfg = next((n for n in names if n.endswith("slice_info.config")), None)
        if not cfg:
            return "not sliced"
        txt = z.read(cfg).decode("utf-8", "replace")
    t = re.search(r'key="prediction" value="(\d+)"', txt)
    w = re.search(r'key="weight" value="([\d.]+)"', txt)
    out = []
    if t:
        s = int(t.group(1))
        out.append("%d h %02d min" % (s // 3600, s % 3600 // 60))
    if w:
        out.append("%s g" % w.group(1))
    return ", ".join(out) or "sliced (no prediction found)"


def main():
    kind = sys.argv[1] if len(sys.argv) > 1 else "pla"
    folder = os.path.join(HERE, "print", kind)
    fil_src = os.path.join(DATA, "user")
    fil_path = None
    for root, _, files in os.walk(fil_src):
        if FILAMENT_NAME + ".json" in files and "backup" not in root:
            fil_path = os.path.join(root, FILAMENT_NAME + ".json")
    if not fil_path:
        sys.exit("filament preset %r not found under %s" % (FILAMENT_NAME, fil_src))
    work = tempfile.mkdtemp(prefix="sm_slice_")
    fil = json.load(open(fil_path))
    fil["type"] = "filament"             # the user preset file leaves it out; the CLI insists
    fil.setdefault("name", FILAMENT_NAME)
    fil["filament_colour"] = [WHITE]
    fil["default_filament_colour"] = [WHITE]
    fil_out = os.path.join(work, "filament.json")
    json.dump(fil, open(fil_out, "w"), indent=1)
    only = sys.argv[2:]                  # optional plate names: slice just those
    for name, parts, extra in PLATES:
        if only and name not in only:
            continue
        dropped = [p for p in parts if p in OPTIONAL and not os.path.exists(os.path.join(folder, p + ".stl"))]
        parts = [p for p in parts if p not in dropped]
        if dropped:
            print("%s: no %s in this build, left out" % (name, ", ".join(sorted(set(dropped)))))
        stls = [os.path.join(folder, p + ".stl") for p in parts]
        missing = [s for s in stls if not os.path.exists(s)]
        if missing:
            sys.exit("missing %s: run export_print.py in Fusion first" % ", ".join(missing))
        proc = dict(PROCESS, **extra)
        proc["name"] = proc["print_settings_id"] = "SmartMeteo PLA 0.12 " + name
        proc_out = os.path.join(work, name + "_process.json")
        json.dump(proc, open(proc_out, "w"), indent=1)
        out = os.path.join(folder, name + ".3mf")
        if os.path.exists(out):
            os.remove(out)
        cmd = [APP, "--load-settings", MACHINE + ";" + proc_out, "--load-filaments", fil_out,
               "--arrange", "1", "--orient", "0", "--ensure-on-bed", "--slice", "0",
               "--outputdir", folder, "--export-3mf", name + ".3mf"] + stls
        run_slicer(cmd, name, out)
        per = {p: {"enable_support": "0"} for p in NO_SUPPORT.get(name, []) if p in parts}   # an optional
        for p, kv in PER_PART.get(name, {}).items():                                         # part left out
            if p in parts:                                                                   # has nothing
                per.setdefault(p, {}).update(kv)                                             # to edit
        blk = {p: v for p, v in BLOCKERS.get(name, {}).items() if p in parts}
        if per or blk:
            # second pass: the same project with per-object settings (supports off, ironing off) and
            # support blockers added, sliced again in place (no --arrange, so nothing moves)
            src = os.path.join(work, name + "_in.3mf")
            edit_project(out, src, per, blk)
            os.remove(out)
            run_slicer([APP, "--load-settings", MACHINE + ";" + proc_out, "--load-filaments", fil_out,
                        "--slice", "0", "--outputdir", folder, "--export-3mf", name + ".3mf", src], name, out)
        # the same plate as plain G-code, for printing from the phone app
        with zipfile.ZipFile(out) as z:
            open(os.path.join(folder, name + ".gcode"), "wb").write(z.read("Metadata/plate_1.gcode"))
        print("%-16s %-48s %s" % (name, ", ".join(parts)[:48], slice_info(out)))
    print("3MFs in", folder)


def run_slicer(cmd, name, out):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if r.returncode != 0 or not os.path.exists(out):
        tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-15:])
        sys.exit("%s: slicer failed (exit %d)\n%s" % (name, r.returncode, tail))


def box_mesh(oid, x0, x1, y0, y1, z0, z1):
    """A closed box as a 3MF mesh object, triangles wound outwards."""
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    tri = [(0, 2, 1), (1, 2, 3), (4, 5, 6), (5, 7, 6), (0, 1, 4), (1, 5, 4),
           (2, 6, 3), (3, 6, 7), (0, 4, 2), (2, 4, 6), (1, 3, 5), (3, 7, 5)]
    return ('  <object id="%d" type="model">\n   <mesh>\n    <vertices>\n' % oid
            + "".join('     <vertex x="%.4f" y="%.4f" z="%.4f"/>\n' % p for p in v)
            + "    </vertices>\n    <triangles>\n"
            + "".join('     <triangle v1="%d" v2="%d" v3="%d"/>\n' % t for t in tri)
            + "    </triangles>\n   </mesh>\n  </object>\n")


def edit_project(src, dst, per_part, blockers):
    """Copy a sliced project with per-object process settings from `per_part` ({part: {key: value}},
    e.g. supports off) and support blockers added to the parts in `blockers`. The slicer keeps per-object options as metadata on
    the object in model_settings.config, and a blocker is one more part of the object (a mesh in the
    object's file, a component in 3dmodel.model, subtype support_blocker in model_settings.config).
    The object's mesh is stored centred: object coordinates are the STL's minus source_offset, read
    from the project. The old G-code is dropped so nothing stale survives the second pass."""
    with zipfile.ZipFile(src) as zin:
        files = {it.filename: zin.read(it.filename) for it in zin.infolist()}
        infos = {it.filename: it for it in zin.infolist()}
    settings = files["Metadata/model_settings.config"].decode()
    model = files["3D/3dmodel.model"].decode()
    for p, kv in per_part.items():
        meta = "".join('\n    <metadata key="%s" value="%s"/>' % (k, v) for k, v in kv.items())
        settings, n = re.subn(r'(<object id="\d+">\s*<metadata key="name" value="%s\.stl"/>)' % re.escape(p),
                              lambda m: m.group(1) + meta, settings)
        if n < 1:                        # every copy (two clips on one plate are two objects)
            sys.exit("%s: object %s not found in the project" % (src, p))
    next_id = 1000
    for p, boxes in blockers.items():
        ids = re.findall(r'<object id="(\d+)">\s*<metadata key="name" value="%s\.stl"/>' % re.escape(p), settings)
        if not ids:
            sys.exit("%s: object %s not found in the project" % (src, p))
        for oid_ in ids:                 # every copy on the plate (two coupons are two objects)
            settings, model, next_id = add_blockers(src, files, settings, model, oid_, p, boxes, next_id)
    files["Metadata/model_settings.config"] = settings.encode()
    files["3D/3dmodel.model"] = model.encode()
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for name, data in files.items():
            if not name.startswith("Metadata/plate_1.gcode"):
                zout.writestr(infos[name], data)


def add_blockers(src, files, settings, model, oid_, p, boxes, next_id):
    """Blockers for one object (one copy of part p, object id oid_). Each copy has its own mesh file, which
    gets the blocker meshes; returns the edited settings and model texts and the next free id."""
    m = re.search(r'<object id="%s"[^>]*>\s*<components>\s*<component p:path="(/3D/Objects/%s\.stl_\d+\.model)" '
                  r'objectid="\d+"[^>]*/>' % (oid_, re.escape(p)), model)
    if not m:
        sys.exit("%s: object %s (id %s) not found in 3dmodel.model" % (src, p, oid_))
    path = m.group(1)
    obj = re.search(r'(<object id="%s">\s*<metadata key="name" value="%s\.stl"/>.*?)(\s*</object>)'
                    % (oid_, re.escape(p)), settings, re.S)
    # Where things land on reload, found by reading the re-exported project: the part comes back in
    # the STL's own coordinates (its centred mesh shifted by its matrix, a translation by
    # source_offset), while a blocker lands exactly where its mesh is written, whatever matrix it
    # carries. So the blocker mesh is written in the STL's coordinates. Written centred, it landed
    # 53 mm off, in the middle of the bed. The part must be a plain translation for this to hold.
    matrix = re.search(r'<part id="\d+" subtype="normal_part">.*?key="matrix" value="([^"]+)"', obj.group(1), re.S).group(1)
    mv = [float(t) for t in matrix.split()]
    if mv[0:3] != [1, 0, 0] or mv[4:7] != [0, 1, 0] or mv[8:11] != [0, 0, 1]:
        sys.exit("%s: part %s is rotated or scaled; blockers expect a plain translation" % (src, p))
    meshes, comps, parts = "", "", ""
    for i, box in enumerate(boxes):
        oid = next_id; next_id += 1
        meshes += box_mesh(oid, *box)
        comps += '\n    <component p:path="%s" objectid="%d" transform="1 0 0 0 1 0 0 0 1 0 0 0"/>' % (path, oid)
        parts += ('\n    <part id="%d" subtype="support_blocker">\n      <metadata key="name" value="blocker %d"/>'
                  '\n      <metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>\n    </part>' % (oid, i + 1))
    objfile = path.lstrip("/")
    files[objfile] = files[objfile].decode().replace(" </resources>", meshes + " </resources>", 1).encode()
    model = model.replace(m.group(0), m.group(0) + comps, 1)
    settings = settings.replace(obj.group(0), obj.group(1) + parts + obj.group(2), 1)
    return settings, model, next_id


if __name__ == "__main__":
    main()
