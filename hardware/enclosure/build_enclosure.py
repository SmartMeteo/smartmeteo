"""Smart Meteo enclosure, v1 — generated into the open Fusion design.

Run it through Fusion's MCP server:  ./fmcp.py --script build_enclosure.py
Each run deletes the components it made before (names starting with "SM ") and builds them
again, so a parameter below is changed here and the script re-run, not edited in Fusion.

Target document: "SmartMeteo enclosure" (Default Project), a Save As of the board model turned
into a DIRECT design (no timeline). In the parametric board file every feature this script adds
cost ~5 s and got slower with each run (10.5 min a build); in a direct design it is ~0.01 s, and
the whole build takes ~30 s with the full 522-part board still there. The board file itself
(3D_PCB_ESP32-C6-SmartMeteo_v2) is left as it was. Measured 2026-10-09.

Coordinates are the board model's own (3D_PCB_ESP32-C6-SmartMeteo_v2), in millimetres:
  z = 0      component side of the PCB; the parts and the cell face +z (the BACK of the device)
  z < 0      the e-paper panel is taped to the bare underside and faces -z (the FRONT)
Looking at the screen from the front, the board model's +x is on the viewer's LEFT.

Stack, front to back:  bezel lip | gap | panel 1.2 | tape 0.8 | PCB | parts, cell 8.0 | cover.
The board is held in the front shell by itself: two rigid ledges over its top edge at the inserts,
two whiskers over its bottom edge (LATCH_STYLE below). With "pull" the whiskers are drawn back with
a paperclip first, the board goes in top edge first and is laid down without touching them, and the
clips are pulled out. The middle of the bottom edge stays free for the FPC.
The rear cover only screws into the four M2 inserts, so with the cover off the board, the
panel and the cell (on CN1) stay where they are.

The rear cover carries two 10 x 3 magnet pockets and a fold-out stand in the area right of
the cell (ESP32 and USB side), hinged low and folded upwards, like a phone-case kickstand.
The button is a separate key in a hole in the top wall (SM Button, BUTTON_STYLE below); the tongue cut
out of the wall itself (v5, printed 2026-10-12) is kept as the alternative.
The two status LEDs emit towards the cover; a sloped block on the cover turns their light
into a small window in the side wall (plug it with a piece of clear 1.75 filament).

Board facts below are measured from the model (2026-10-09); panel facts are from the Good
Display GDEY042T81 mechanical drawing, p. 7.

BOARD (below) picks the board: "v2" is PCB v2.2 and the case released as enclosure-v2.2; "v1" is the same
case fitted to the first board (owner, 2026-10-13), PLA only, built into its own document from the v1 board
model. On v1 the board goes in flat: no top ledges, grooves let its USB receptacle and button plunger
pass the walls (see BOARD). "v0" is the oldest board (owner, 2026-10-14), drawn by build_board_v0.py: the cover's
screws go through its corner holes into the shell, its switch's own stem is the button, and the microSD card
is reached through the right wall (see BOARD).
"""
import adsk.core, adsk.fusion, math

# ---------------------------------------------------------------- print process --
# The fits differ by process. FDM (PLA on the owner's printer) prints holes 0.1..0.2 mm small, rounds
# heights to the layer, lets the underside of an overhang sag, and is weakest bending across its layers;
# its column is what the prints of 2026-10-10..13 settled. The SLA column is for JLC3DP's 9600 resin
# (owner, 2026-10-13), set from JLC3DP's published rules, not from a print: sizes +-0.2 mm within 100 mm,
# holes +-0.3 mm and usually small, at least 0.2 mm between mating parts, walls from 0.8 mm. So a fit PLA
# could take tight because FDM holes shrink gets 0.2 on resin, and a hole something goes into is not drawn
# smaller than PLA's. Only the fits that differ are here; the rest (PCB to wall, USB) are loose enough for
# both. Chosen by the owner 2026-10-10: a switch, PLA for the first prototypes.
PRINT = "pla"                          # "pla" or "sla"
_FITS = {
    #                      sla    pla
    "CATCH_PLAY":        (0.15, 0.20),   # ledge underside is an overhang: it sags. Resin does not sag, but
                                         # 0.05 is inside its +-0.2, and a ledge that lands low stops the board
    "HOOK_T":            (0.70, 0.80),   # two whole extrusion lines, not one and a gap fill
    "HOOK_OVER":         (0.60, 0.40),   # tooth over the PCB; 0.30 held, but felt small and gave (owner, 2026-10-11)
    "BOSS_GAP":          (0.05, 0.05),   # boss end to insert top; at 0.20 the screws pulled the board 0.2 back
                                         # and the USB port with it (owner, 2026-10-11): it must not land first
    "BTN_CLEAR":         (0.20, 0.15),   # key in its hole; 0.25 printed tight (2026-10-10), 0.40 loose,
                                         # 0.35, 0.30, 0.27 and 0.24 (standing) still a little loose; of four
                                         # trial keys 0.15 is "perfect" (owner, 2026-10-13). Since 0.40
                                         # the hole stays 8.8 x 4.0 as printed on 2026-10-12 and the key grows
                                         # instead (BTN_CAP_W/H), so only the key is printed again. Resin: 0.20
                                         # (JLC3DP's minimum between mating parts), the hole grows to 8.9 x 4.1
    "TONGUE_SLIT":       (0.40, 0.60),   # slits round the wall tongue: 0.60 = 5 layers of 0.12; a bridged
                                         # ceiling sags ~0.2, so less could close up
    "TONGUE_GAP":        (0.15, 0.20),   # tongue nub to the SW2 plunger, board in the middle of its float
    "BTN_GAP":           (0.40, 0.40),   # key's back to the SW2 plunger with the board pushed up to the top
                                         # wall and the key out on its slot floors: room for print, part and
                                         # board tolerance, so the plunger never holds the key out. 0.15 was
                                         # too close on the 2026-10-12 print (owner, 2026-10-13); a 0.10
                                         # preload kept the board from going down at all (2026-10-12)
    "COVER_FIT":         (0.20, 0.10),   # cover to rebate, per side; 0.25 rattled ~0.5 mm (owner, 2026-10-11)
    "COVER_RIB_IN":      (0.05, 0.10),   # crush ribs into the rebate wall: PLA gives where they touch, resin
                                         # (81 Shore D) hardly does, and its +-0.2 already makes the press
    "FIT_TOP":           (0.15, 0.10),   # PCB's top edge to the top wall
    "HOOK_GAP":          (0.10, 0.05),   # PCB's bottom edge to the latch blocks: with FIT_TOP the board's float
                                         # in Y. PLA's 0.15 clamps it (that print ran tighter than drawn); resin
                                         # prints true, 0.25 is JLC3DP's minimum plus a PCB outline tolerance
    "LED_HOLE_D":        (2.00, 2.00),   # a 1.75 filament stub has to go in (resin holes come out small too)
    "MAG_POCKET_D":      (10.4, 10.5),   # 10 mm magnet; 10.5 was "right in size" on PLA
    "STAND_PIN_HOLE":    (1.80, 1.90),   # pin variant: the pin hole
    "STAND_SEAT_D":      (2.10, 2.20),   # snap stub seats for the 1.8 stub; 1.9 for 1.6 printed too small
    "STAND_FLAP_HOLE":   (1.95, 2.05),   # pin variant only
    "STAND_FLOOR_GAP":   (0.20, 0.30),   # flap over the pocket floor
    "TB_PILOT_D":        (1.70, 1.80),   # the M2 screw cuts its own thread here; FDM prints it ~0.15 small
    "TB_HOLE_D":         (2.20, 2.40),   # M2 clearance through the turnbutton
    "TB_PLAY":           (0.05, 0.10),   # turnbutton underside over the PCB top (as CLIP_PLAY)
}
assert PRINT in ("sla", "pla")
FIT_FOR = {k: v[0 if PRINT == "sla" else 1] for k, v in _FITS.items()}
FDM = PRINT == "pla"   # the print ties and the cover's own printed supports exist only for FDM

# ---------------------------------------------------------------- board (measured) --
# Which board the case is for. "v2": PCB v2.2, the case released 2026-10-13 as enclosure-v2.2. "v1": the first
# board (model 3D_PCB_ESP32-C6-SmartMeteo_2026-06-26), a PLA case built on the v2.2 design (owner, 2026-10-13).
# What differs on v1, sliced from its model 2026-10-13:
#   - the outline is ~0.15 smaller each side, the PCB 1.59 thick (v2 1.67) - the glued stack is taken as
#     the same 3.2 measured on v2: same panel, same tape;
#   - the USB-C receptacle stands 1.33 past the board's right edge, so 1.03 into the wall (USB_GROOVE);
#   - the button is the upright TS-1102US: its plunger is centred 4.49 above the board and its tip stands
#     0.26 past the top edge (SW2_GROOVE, BTN_*);
#   - the cell is the 103450, 34 wide and 10 tall (owner: "it stands 10 mm, a little fatter"): the case is 2 mm
#     deeper and the cover screws are M2 x 10;
#   - the inserts sit ~0.1 off v2's; the BME690 sits where v2's does (owner, 2026-10-14: the 2026-10-13 build put
#     its vents on the left wall after a part read off the merged board model, which was not the BME690).
# The top edge goes under v2's two ledges the same way (owner, 2026-10-14: "the tabs that hold the top are
# missing"): bottom end lifted clear of the whiskers, board slid up 1 mm under the ledges, bottom end lowered. On
# v1 two parts ride along: the receptacle slides up its groove, which is therefore 1.2 longer towards the bottom,
# and the tall plunger leans out with the board and pushes the key out onto its slot floors (it has BTN_GAP for
# that, and SW2's travel after it; the report below gives the tilt). The first v1 build (2026-10-13) went in flat
# with no ledges and only the cover's bosses on the top inserts.
# "v0": the oldest board, "epaper c6 NEW" (silkscreen "Smart Meteo v1.0"; owner, 2026-10-14: "it needs a case
# too"). No Fusion model of it exists: build_board_v0.py draws one from the owner's OBJ and photos, and says
# what is measured and what is guessed. It differs from v1/v2 in kind, not by tenths:
#   - no M2 inserts: four plain Ø2.9 holes, two in the top corners and two in EARS below the bottom edge (the
#     panel's FPC wraps round the edge between the ears, so the case needs no FPC room below them). The cover's
#     four screws (M2 x 16 countersunk) go through the cover, through those holes, into bosses of the front shell
#     under the board, where the glass is not: it starts 5.3 above the ears' bottom and ends 6.3 below the top
#     edge. The screws hold board, panel and cover at once, so there are no ledges and no whiskers
#     (LATCH_STYLE "none"); with the cover off the board lies loose on the bosses, as it did in the old case;
#   - the button is SW2's own stem, a right-angle switch: it leaves the top wall through a round hole
#     (BUTTON_STYLE "stem"), slotted up to the rim so the board goes in; a tongue on the cover fills the slot;
#   - the microSD socket opens towards the USB side: a slot in that wall, with a dish round it for a nail
#     (owner: "access to the card from outside"); the slide switch beside it sits in the old case's notch,
#     whose top a second tongue on the cover closes down to the switch (owner, 2026-10-14: "a hole there");
#   - no BME690 (no vents for it); the SCD41 sits on a module by the left wall; the cell is the 103450 again,
#     standing 34 x 50 on the board's left half.
BOARD = "v2"                           # "v2", "v1" or "v0"
assert BOARD in ("v0", "v1", "v2")
V1 = BOARD == "v1"
V0 = BOARD == "v0"
if V0:   # outline from the OBJ, placed with build_board_v0.py's X_OFF / Y_TOP
    PCB_X0, PCB_X1 = 7.70, 98.84
    PCB_Y0, PCB_Y1 = 12.59, 101.18
    PCB_TOP, PCB_BOT = 0.0, -1.60      # the OBJ's PCB is drawn 2.54 thick; 1.6 assumed
    PCB_MAIN_Y0 = PCB_Y0 + 4.95        # the bottom edge between the ears (PCB_Y0 is the ears' bottom)
    # no inserts: these are the screw axes, through the board's Ø2.9 holes (the OBJ's, as build_board_v0.py):
    # bottom-left and -right in the ears, then top-left and -right
    INSERTS = [(10.215, 15.353), (96.575, 15.353), (10.469, 98.284), (96.495, 98.220)]
elif V1:
    PCB_X0, PCB_X1 = 7.52, 99.00
    PCB_Y0, PCB_Y1 = 12.77, 90.24
    PCB_TOP, PCB_BOT = 0.09, -1.50
    INSERTS = [(12.695, 17.78), (93.975, 17.78), (12.695, 84.963), (93.975, 84.836)]
else:
    PCB_X0, PCB_X1 = 7.38, 99.17
    PCB_Y0, PCB_Y1 = 12.59, 90.41          # Y0 is the bottom edge: the panel FPC wraps around it
    PCB_TOP, PCB_BOT = 0.09, -1.58
    INSERTS = [(12.57, 17.78), (93.98, 17.78), (12.57, 85.22), (93.98, 85.22)]
# M2 inserts: "M2 4MM SMT INSERT" in the BOM, soldered on a pad with no hole under it (checked in the
# board model). The board model's 3D part is wrong (2.3 mm above the PCB, running through it); the
# owner measured 4.0 mm above the PCB with calipers, 2026-10-10. So the screw must stop above the PCB.
INSERT_H, INSERT_D = (0.0, 0.0) if V0 else (4.0, 6.2)   # measured height; diameter of the model's part, as an
                                       # envelope. v0 has none: the cover's bosses end on the PCB itself
INSERT_TOP = PCB_TOP + INSERT_H
SCREW_L = 16.0 if V0 else 10.0 if V1 else 8.0   # M2 countersunk: on v2 x 10 would bottom on the PCB before the cover
                                       # seats; v1's cover sits 2 mm higher, and x 8 would reach 0.2 into the insert;
                                       # v0's runs on through the board into the shell's boss
if V0:   # right-angle KH-6X6X6H-ZJ-A: the stem is the button (build_board_v0.py)
    SW2_X, SW2_Z = PCB_X0 + 21.44, 4.0                # stem axis
    SW2_TIP_Y = PCB_Y1 - 0.5           # only so key_geometry() stays defined; v0 has no key
    STEM_D, STEM_CLEAR = 3.5, 0.30     # stem; hole round it (the board floats 0.3, FIT_TOP below)
    STEM_FILL_CLEAR = 0.15             # the cover's tongue in the slot above the stem, each side
    SW2_BODY_OUT = 0.07                # the switch's body stands this far past the board's top edge (OBJ)
elif V1:   # upright TS-1102US: plunger 3.35 square, tip 0.26 past the board's top edge
    SW2_TIP_Y, SW2_X, SW2_Z = 90.50, 30.735, 4.49
else:
    SW2_TIP_Y, SW2_X, SW2_Z = 89.80, 31.12, 1.17      # side-push SMD button, plunger tip
USB_Y, USB_Z = 44.196, 1.72            # USB-C receptacle shell at its face, sliced from the model (the same on
USB_W, USB_H = 8.94, 3.16              # both boards); the part's own bbox is wider and lower because of its tabs
USB_FACE = 100.33 if V1 else 98.80     # x of the receptacle's face: past the board's edge (99.00) on v1
if V0:   # the same 8.94 x 3.16 shell on the PCB (the OBJ, past the board's edge: z 0.05..3.21), its face 1.41 out
    USB_Y, USB_Z, USB_W, USB_H, USB_FACE = PCB_Y1 - 60.43, 1.63, 8.94, 3.16, PCB_X0 + 92.55

# ---------------------------------------------------------------- panel (datasheet) --
PANEL_W, PANEL_H, PANEL_T = 91.0, 77.0, 1.2
VA_MARGIN, VA_MARGIN_FPC = 2.6, 9.9    # viewing area 85.8 x 64.5; the FPC side is wider
COG_BAND = 4.7                         # driver IC strip along the FPC edge: nothing may press it
# panel-to-PCB tape: the glued PCB + tape + panel measures 3.2 mm (owner, calipers, 2026-10-10), so with
# the PCB model's 1.67 and the 1.2 glass the tape is 0.33 (it was 0.8 from memory)
STACK = 3.2
if V0:   # not measured on v0: its 1.6 PCB with v2's tape and glass
    STACK = 1.6 + 0.33 + PANEL_T
TAPE_PANEL = round(STACK - (PCB_TOP - PCB_BOT) - PANEL_T, 2)
# v0's glass is not centred on its board: the old case's window (85 x 64, the viewing area is 85.8 x 64.5) puts
# its FPC edge 5.3 above the board's bottom edge, and so its top edge 6.3 below the board's top one
PANEL_Y_OFF = 5.3 if V0 else None

# ---------------------------------------------------------------- cell --
if V0:   # 103450 standing on its end: 34 along x, 50 (53 with its folded tape) along y, on photos e2bcbfbf/77322c51
    CELL_NAME, CELL_L, CELL_W, CELL_T, CELL_C = "103450", 35.0, 53.0, 10.0, (PCB_X0 + 18.0, PCB_Y0 + 35.5)
elif V1:   # 103450 on v1 (owner, 2026-10-06), centred in its silkscreen box x 9.72..63.97, y 23.11..58.37
    CELL_NAME, CELL_L, CELL_W, CELL_T, CELL_C = "103450", 50.0, 34.0, 10.0, (36.845, 40.74)
else:    # 804050 on every v2 board, centred in its silkscreen box
    CELL_NAME, CELL_L, CELL_W, CELL_T, CELL_C = "804050", 50.0, 40.0, 8.0, (10.03 + 51.27 / 2, 22.47 + 40.27 / 2)
CELL_TAPE, CELL_CLEAR = 0.20, 0.60

# ---------------------------------------------------------------- case --
WALL = 2.0
FIT = 0.30                             # PCB edge to the side walls
# Top and bottom hold the board in Y: the top wall 0.1 off its top edge, the latch rails 0.05 off its
# bottom edge (HOOK_GAP). With 0.3 and 0.39 the board slid ~0.5 mm up and down and the key sat on SW2
# (owner, 2026-10-11; that print came out ~0.2 tighter than the 0.69 nominal, so 0.15 should clamp).
FIT_TOP = 0.30 if V0 else FIT_FOR["FIT_TOP"]   # v0: the screws through its holes place the board, not the walls
FPC_ROOM = 0.3 if V0 else 3.0          # bottom: the FPC loop (panel 2.4 mm below the PCB) needs room; on v0 it
                                       # wraps between the board's ears, and the ears stand FIT off the wall
R_OUT = 4.0
R_IN = 0.4                             # the PCB corners are sharp: a larger inner radius bites them; with
                                       # the board 0.1 off the top wall, 0.6 would hold its corner off it
PCB_R = 2.4                            # v0's corners are rounded (the OBJ: 2.4 at the top, ~2.4 at the ears)
if V0:   # the cavity follows them, concentric, FIT off all round (owner, 2026-10-14)
    R_IN = PCB_R + FIT
BEZEL_T = 1.6
FRONT_GAP = 0.30                       # lip to panel face: the glass never bears on the lip
COG_RELIEF = 0.6
COVER_T = 2.0
REBATE = 1.0                           # the cover sits in a step this wide in the wall
R_REBATE = R_OUT - (WALL - REBATE)     # concentric with the outer corner, so the rim stays WALL - REBATE
                                       # thick round the corners too; R_IN + REBATE left 0.42 mm there and
                                       # the back-edge fillet thinned it to a 0.06 mm knife edge
COVER_FIT = FIT_FOR["COVER_FIT"]
# Crush ribs on the cover's edge (2026-10-11). The fit did not change, but with BOSS_GAP 0.05 the
# screws now pull the cover onto the inserts instead of onto the rebate shoulder, so nothing clamps
# the shell any more and it moves on the cover by the fit (owner: "the cover wanders in the shell").
# Eight narrow ribs stand COVER_RIB_IN into the rebate wall; PLA gives where they touch, and the cover
# sits without play whatever the screws do. A 45 deg lead-in where they meet the rebate first.
COVER_RIB_IN, COVER_RIB_W = FIT_FOR["COVER_RIB_IN"], 0.8
COVER_RIBS_X = [25.0, 60.0]            # on the bottom and top edges; clear of the stand pocket
                                       # (x 66..88), whose thin rim at the foot end would just bend
COVER_RIBS_Y = [25.0, 70.0]            # on the left and right edges
BOSS_D, BOSS_GAP = (4.4 if V0 else 5.6), FIT_FOR["BOSS_GAP"]   # v0: its holes are 2.75 off the walls
if V0:   # the bosses land on the board 0.05 before the cover reaches its shoulder, so the screws clamp the board
    BOSS_GAP = -0.05                   # between them and the shell's bosses: else a press on SW2's stem would
                                       # slide it round the screws, which pass its holes with 0.35 to spare
# v0: the shell's bosses under the board's holes, from the bezel up to the PCB, and the screw's pilot in them
SHELL_BOSS_D = 3.6                     # 0.9 of wall round the M2's pilot; 1.05 off the glass's bottom edge, whose
                                       # place is read off the old case's window to ~0.4
HOLE_D, CBORE_D, CBORE_DEPTH, CSINK_D = 2.4, 4.2, 1.0, 3.9   # M2 x SCREW_L countersunk ONLY

# board retention in the front shell
CATCH_PLAY = FIT_FOR["CATCH_PLAY"]     # catch faces sit this far above the PCB top
# all four at the corners, on the insert axes (owner, 2026-10-09): the PCB strip between the
# insert flange and the board edge is free of parts there (2.1 mm at the sides, 2.1 at top/bottom)
# none on v1: its board goes in flat (BOARD above), and a ledge needs it tilted in under it
LEDGE_X, LEDGE_W, LEDGE_OVER, LEDGE_Z1 = ([] if V0 else [INSERTS[2][0], INSERTS[3][0]]), 6.0, 1.0, 1.6
HOOK_X, HOOK_W, HOOK_T = [INSERTS[0][0], INSERTS[1][0]], 8.0, FIT_FOR["HOOK_T"]
HOOK_GAP, HOOK_OVER = FIT_FOR["HOOK_GAP"], FIT_FOR["HOOK_OVER"]   # beam to PCB edge (0.39 let the board slide); tooth over the PCB top face
# The bottom-edge latches. "hook" was a cantilever standing in Z, rooted in the lip: the first PLA print
# broke it along a layer the first time it was bent (owner, 2026-10-10) - a beam standing in Z bends
# across the layer bonds, the weakest direction a print has. "rail" is a bar lying along the board
# edge, held at both ends (the side wall and a block tied to the outer wall) with the tooth in the
# middle: it prints as a bridge and flexes sideways, so the bend runs along the extrusion lines.
# Back to "hook" for a test on 2026-10-11 (owner: "try the bending tab again, maybe it is better").
# It is the same tab, made to survive the snap: tapered (thick root, thin top, so the bend spreads
# instead of piling up at the root), rounded where it meets the floor (the 10-10 crack started at that
# sharp corner) and 8 mm wide (width adds strength without adding strain). A thicker tab would not
# help: at the same deflection the strain grows with the thickness.
# Back to "rail" after the fifth print (owner, 2026-10-12): the screws held best of all, but M2 screws are
# fiddly to drive, and the rails never broke - they only let go when the screen was pressed. So the rails
# return longer and heavier (RAIL_* below), and with the bar standing mostly ABOVE the tooth.
# Then "pull" the same day (owner: "I don't want to press the board in; draw the whiskers back, lay it in
# without the screen touching them, let them return and hold it with a big overlap"). PULL_* below.
LATCH_STYLE = "none" if V0 else "pull"   # "pull", "rail", "hook", "clip" or "screw"; "none" on v0 (BOARD above)
HOOK_T_ROOT, HOOK_T_TOP, HOOK_ROOT_R = 0.9, 0.5, 0.4
# "clip" (owner, 2026-10-11, after the tapered hook broke on assembly too): a separate part, printed
# lying on its side so it bends along the layers. It HANGS from a tab in a slot in the bottom wall;
# its finger reaches down past the board edge to a nose over the board's top face. Hanging from
# above is the point: when the screen is pressed, the board pushes the finger lengthwise, and the
# nose's overhang bends it towards the board, so the harder the push, the tighter it holds. The
# standing hook was pulled lengthwise instead, and the same overhang pried it open. Fitted before
# the board; the board's edge then stands CLIP_GAP off the finger, so the tab cannot leave its slot.
# The nose stays at 0.4: the panel's glass edge is only 0.41 inside the board edge, and the glass
# passes the nose first on the way in.
CLIP_W = 6.0
CLIP_T_ROOT, CLIP_T_NOSE = 1.0, 0.7    # the finger tapers from its root at the arm to the nose
CLIP_OVER, CLIP_GAP, CLIP_PLAY = 0.40, 0.05, 0.10   # nose over the PCB; finger to PCB edge; nose above the PCB top
CLIP_ARM_H = 1.2                       # 1.6 at first: a thinner arm gives the finger 0.4 more length,
                                       # 1.52 -> 1.33 % to snap in, and the arm stays 4x as stiff
CLIP_TAB_H, CLIP_TAB_IN = 1.0, 1.0     # tab height; how far it goes into the 2 mm wall
CLIP_SLOT_CLEAR, CLIP_SLOT_X_CLEAR = 0.05, 0.15
# "screw" (owner, 2026-10-12, after the clips): a turnbutton on an M2 screw in each bottom corner, on the
# insert axes. A boss rises from the FPC room's floor to just under the turnbutton, whose arm reaches
# TB_OVER over the board's top face. A quarter turn (arm along the bottom wall) clears the board, which then
# lifts out; the screw is loosened half a turn for that, not taken out. Nothing flexes: a screen press is
# taken by the screw in the boss. The strip under the board's edge is too narrow for an M2 head, so a
# notch in the bottom wall takes the head and the arm's open swing (0.9 of wall left behind it, as behind
# the clip slots). Two fingers on the cover stand either side of each hub: with the cover on, a
# turnbutton cannot turn more than ~17 deg, and with one left open the cover does not go on.
# Why not a cover finger alone, or a printed pin with a friction fit: the cover is screwed to the
# board's inserts, so it moves with the board and holds nothing against a screen press; and a PLA
# friction fit creeps and wears with every opening.
TB_HEAD_D, TB_HEAD_K, TB_SCREW_L = 3.8, 2.0, 5.0   # M2 x 5, cylinder head (DIN 912 socket / ISO 1207 cheese)
TB_EDGE_CLEAR = 0.14                   # hub and head to the PCB edge: open, the board lifts past them
TB_T, TB_ARM_W, TB_OVER = 1.2, 3.2, 1.5  # turnbutton thickness; arm width; arm over the PCB's top face
TB_BOSS_W, TB_BOSS_GAP = 3.2, 0.10     # boss width; boss face to the PCB edge (FIT_TOP's 0.10 printed true)
TB_PILOT_DEPTH = 4.0                   # 3.8 of thread for an M2 x 5 through the 1.2 turnbutton
TB_NOTCH_CLEAR = 0.15                  # notch round the head and the swinging arm
TB_FINGER_W, TB_FINGER_GAP, TB_FINGER_DOWN = 1.2, 0.2, 0.5   # cover finger: width (x); gap to the head;
                                       # how far below the turnbutton's top face it reaches
TB_RIB = (0.4, 0.4, 0.3)               # nail rib across the arm: past the PCB edge, wide, high
# The rail, third version (owner, 2026-10-12: "make the whisker longer and stronger, so a press on the
# screen cannot snap it open"). Each rail is a bar from its side wall to an anchor block, the tooth in the
# middle. It snaps open by bending sideways (towards the bottom wall), along the extrusion lines:
#   strain 12 d s / L^2 and force 16 E h d^3 s / L^3, for depth d, height h, span L and tooth overlap s.
# A longer span lets the bar be much deeper for the same strain, and depth is what stiffens it (d^3). The
# left rail runs 26 mm, twice the old 13, at 1.5 deep instead of 0.8. The right one cannot grow: it already
# ends 2 mm short of the FPC, which wraps the board edge at x ~67..83; it gets the height instead.
# The screen press itself loads the tooth upwards, and the tooth sits on the bar's inner face, so the bar
# twists. With the bar's middle level with the tooth (the 2026-10-11 rail) that tilts the tooth's underside
# into a ramp the board can push it off; with the middle ABOVE the tooth the same twist swings the tooth in
# over the board. So the bar now rises RAIL_RISE above the tooth, and still drops RAIL_DROP below it to stay
# the board's stop in Y (HOOK_GAP). Its top reaches where the cover's turnbutton fingers are: the printed
# v5 cover stays, with those four fingers snipped off.
RAIL_SPAN = (26.0, 13.0)               # side wall to the anchor's inner face: left (model -x, SCD41 side), right
RAIL_DEPTH = (1.5, 0.8)                # bar depth in Y, the way it flexes; the right one keeps 0.8 (2.3 % at 13 mm)
RAIL_TOOTH_W = (6.0, 4.0)              # tooth width (x)
RAIL_ANCHOR = 1.6                      # anchor block length (x), floor to the bar's top, into the bottom wall
RAIL_DROP, RAIL_RISE = 1.3, 2.3        # the bar below / above the tooth's underside: 3.6 tall (it was 1.8),
                                       # its middle 0.5 above the tooth
# "pull" (owner, 2026-10-12): the board is not snapped in at all. Each whisker is drawn back with a paperclip
# before the board goes in, held back by that clip standing in a hole, and let go once the board lies in
# place. Nothing passes the glass edge on the way in any more, so the nose may reach as far over the board
# as the strip allows: 0.7..1.4 instead of 0.4.
#  - A bar held at both ends cannot be drawn back 2 mm (13 % at the right side's 13 mm), so each whisker is a
#    cantilever, rooted in a block towards the middle and free at the corner, where a hood on the side wall
#    takes a screen press once the bar has lifted PULL_HOOD_GAP.
#  - The board's bottom stop in Y moves to the root blocks. A whisker the board leans on would be pushed back
#    by the board's own weight when the device stands on its foot, so the bar stands PULL_SET behind the edge.
#  - The nose is two prongs with a gap (a closed ring for a screwdriver does not fit: its wall towards the
#    board would be 0.2). The clip goes into the gap, pushes the bar to the wall and drops into its hole in a
#    boss under the bar; let go, the bar rests on it. The far prong reaches further over the board than the
#    near one, because the bar's free end moves further than its middle; each prong's tip line follows the
#    bar's deflection, so drawn back every point of it stands PULL_CLEAR off the board edge.
#  - Printing: the bar's first layer is tied across the end gap into the side wall (as the wall tongue's), so
#    its underside prints as a bridge; the prongs' flat undersides get the slicer's support; the hood's
#    underside rises 45 deg from the wall and is flat only over the bar's end.
PULL_ROOT = 13.0                       # side wall to the root block's face; the right side cannot have more
                                       # (the FPC wraps the board edge at x ~67..83)
PULL_END_GAP = 0.6                     # bar end to the side wall; 0.3 would print fused (under one line)
PULL_D = 0.8                           # bar depth in Y, the way it bends: two whole lines
PULL_SET = 0.15                        # bar face behind the board edge (the root block's face is HOOK_GAP)
PULL_PRONG, PULL_FORK = 1.0, 1.6       # prong width (x); the gap between them, for a 1.0 clip
PULL_CLEAR = 0.30                      # drawn back, the prongs' tips stand this far off the board edge
PULL_PIN_D, PULL_PIN_HOLE = 1.0, 1.3   # paperclip or SIM tool; its hole in the boss
PULL_PARK = 1.38                       # how far the clip holds the bar back, at the fork
PULL_HOOD_GAP, PULL_HOOD_T, PULL_HOOD_OVER = 0.40, 0.8, 0.7   # hood over the bar's end: gap, thickness,
                                       # how far along the bar it reaches
PULL_R_BACK = 0.6                      # fillet where the bar leaves its root block, on the back
PULL_TIE_W = 0.5                       # print tie across the end gap: depth (y), one layer thick
E_MOD = 3500.0 if FDM else 2600.0     # MPa, for the latch force estimates: PLA; 9600 resin's flexural modulus

# status LEDs: plain top emitters, light turned 90 deg by a 45 deg block on the cover
LED1_X, LED1_Y = (40.26, 87.25) if V1 else (40.25, 87.25)   # near the top edge (SW2 side)
CHRG_X, CHRG_Y = (95.40, 35.11) if V1 else (96.39, 35.18)   # near the right edge (USB side)
if V0:   # the OBJ's two 0805s: LED1 by SW2, CHRG by the USB receptacle
    LED1_X, LED1_Y, CHRG_X, CHRG_Y = PCB_X0 + 29.06, PCB_Y1 - 3.15, PCB_X0 + 88.37, PCB_Y1 - 52.56
# The 45 deg turner sends the LED's light straight to the window at LED_HOLE_Z, so the two move together.
# 2.6 until 2026-10-12: the turner stood 1.7 mm above the LED (owner: "there is still a millimetre"); at 2.0
# it is 1.1 mm above, and over the LED's back end, where the slope would reach it, it is cut flat
# LED_REFL_CLEAR above the LED's top. A turner left at 2.6 on an older cover still lands inside the window.
LED_HOLE_D, LED_HOLE_Z = FIT_FOR["LED_HOLE_D"], 2.0   # a 1.75 clear filament stub glued in works as a lens
LED_REFL_W, LED_REFL_BACK = 4.0, 1.4   # block width; slope starts this far behind the LED centre
LED_TOP, LED_REFL_CLEAR = 0.90, 0.3    # both LEDs are 0805 top emitters, 0.90 tall in the board model
LED_REFL_GAP = 0.4                     # block end to the wall: at 0.1 the block, not the cover edge, was the tightest fit

# two magnets 10 x 3 glued into pockets in the outer face of the cover, on the board's middle line,
# one above the cell and one below it, symmetric about the middle of the case (owner, 2026-10-09)
MAGNETS = [((PCB_X0 + PCB_X1) / 2, 84.2), ((PCB_X0 + PCB_X1) / 2, 16.1)]
if V0:   # the same 34 either side of the case's middle (y 55.5): clear of the cell and over BOOT/RESET's tops
    MAGNETS = [((PCB_X0 + PCB_X1) / 2, 90.9), ((PCB_X0 + PCB_X1) / 2, 22.8)]
MAG_POCKET_D, MAG_POCKET_DEPTH, MAG_FLOOR, MAG_WALL = FIT_FOR["MAG_POCKET_D"], 3.1, 0.8, 0.8

# Fold-out stand: an easel (owner, 2026-10-11). The flap hangs DOWN from its hinge and opens
# bottom-first; the device leans back on it and its weight turns the flap further OPEN, onto a stop.
# The first version hinged low and folded upwards: there the weight turns the flap shut, and on the
# desk it simply folded (owner, 2026-10-11). A heel past the hinge is both the stop and the opener:
# it swings down into a recess and lands on its floor at STAND_STOP, and pressing it with a finger
# swings the foot out - the old flap needed a screwdriver to lift (owner, 2026-10-11).
STAND_X0, STAND_X1 = 66.0, 88.0        # pocket, right of the cell (cell ends at x 60.7)
STAND_L, STAND_T = 55.0, 2.5           # hinge axis to the foot end; the owner chose 55 of 38.5/55/70
STAND_FOOT_Y = PCB_Y0 - FPC_ROOM + 0.6 # folded foot end, as low as the shell allows: the pocket floor
                                       # stops 0.3 short of the cavity wall, the flap 0.3 short of that.
                                       # The tilt needs the foot close to the bottom edge
STAND_HINGE_Y = STAND_FOOT_Y + STAND_L
STAND_HEEL = 5.0                       # heel past the axis; longer = a stop less sensitive to print error
STAND_STOP = 35.0                      # degrees the flap opens; 35 -> screen ~22 deg off vertical
STAND_SIDE, STAND_FLOOR_GAP = 0.3, FIT_FOR["STAND_FLOOR_GAP"]
STAND_WALL, STAND_FLOOR = 2.0, 1.2     # pocket walls 2.0: they carry 1.4 mm deep stub seats
STAND_HINGE = "snap"                   # how the flap is held, see below; "snap" or "pin"
# snap (owner's choice, 2026-10-10): flex tongues along the flap's edges carry stubs that sit in blind
# holes in the pocket walls. The tongues run from the heel end: squeeze it, drop the flap in, let go.
# The first stubs (1.6, 0.7 in the wall) let the flap drop out (owner, 2026-10-11): now 1.8 and
# 1.2 in, and slits wide enough for that squeeze - the free end moves ~2.0 for 1.2 at the stub.
# The stub can't grow much past 1.8: the axis sits mid-way through the 2.5 flap, and a 2.5 seat
# reached the cover's back face and broke the body (2026-10-11).
# v2 (owner, 2026-10-11): the 1.2 x 18 tongues bent easily, and pushing the open flap past its stop
# squeezed them together until the stubs left their seats. Now 1.5 wide and rooted 11 mm from the
# axis instead of 13: ~3x stiffer, a hard one-time squeeze (~1.9 % at the root) - and ribs in the heel
# recess fill the slits once the flap is open, so the tongues cannot close up there at all.
STAND_STUB_D, STAND_STUB_IN = 1.8, 1.1          # stub diameter; how deep it sits in the wall
STAND_STUB_HOLE_DEPTH = 1.3                     # blind hole in the 2.0 mm wall
STAND_TONGUE_W, STAND_TONGUE_L, STAND_SLIT = 1.5, 16.0, 2.4   # the free end moves ~2.2 for 1.2 at the stub
# blocking ribs: in each slit's band, in the heel recess, below the closed flap (so the squeeze at
# assembly is free) and beside the tongue's heel end once the flap opens. They leave the tongue this
# much room to move in - enough for the open detent, too little for a stub to leave its seat.
STAND_BLOCK_CLEAR, STAND_BLOCK_BODY = 0.4, 0.3  # rib to the tongue; rib to the flap body
# detents (flap coordinates from the axis: dy along the flap towards the heel, dz towards its outer
# face): a 1.2 bump on each pocket wall sits in a 1.4 dimple in the tongue's outer face. Open
# (owner, 2026-10-11: "let it lock open too"; held well in print, owner 2026-10-11): near the
# tongue's free end, rubbing from ~17 deg and dropping into its dimple at the stop. It is soft - a
# detent this close to the axis holds little torque - but ~10x what gravity needs to fold the flap.
STAND_DET_OPEN, STAND_DET_OPEN_IN = (4.2, -0.55), 0.30
STAND_DET_BUMP_D, STAND_DET_DIMPLE_D, STAND_DET_DIMPLE_DEPTH = 1.2, 1.4, 0.35
# Closed: the detent moved from 5 mm off the axis to the foot (2026-10-11). Near the axis it set the
# closed angle, and every 0.1 mm of print error there turned into ~1 mm at the foot, 11x as far
# out: the closed flap stood proud (owner, 2026-10-11). At the foot an error stays its own size. Short
# tongues along the foot's edges carry the dimples; their lower outer edges are chamfered so the bump
# pushes them aside as the foot comes down. Soft on purpose: the heel opens the flap against it with
# 11x the leverage disadvantage.
STAND_DET_FOOT, STAND_DET_FOOT_IN, STAND_DET_FOOT_DIMPLE_D = (-(STAND_L - 3.0), 0.0), 0.15, 1.5
STAND_FOOT_TONGUE_W, STAND_FOOT_TONGUE_L, STAND_FOOT_SLIT, STAND_FOOT_LEADIN = 1.2, 12.0, 1.0, 0.4
# pin (kept as the alternative):
STAND_PIN_HOLE, STAND_FLAP_HOLE = FIT_FOR["STAND_PIN_HOLE"], FIT_FOR["STAND_FLAP_HOLE"]
STAND_SEAT_D = FIT_FOR["STAND_SEAT_D"]   # pin: 35 mm of 1.75 filament
# The pin goes in from the cover's right edge (model +x), through a rib under the plate, the right
# pocket wall and the flap, into a blind end in the left wall. Once the cover is in, the shell's
# rim sits 0.25 mm off the pin's end, so it cannot walk out. The hinge axis is only 1.25 mm under
# the back face, inside the plate, so there was no other way in: the first version had both ends
# blind and the pin could not be fitted at all (found 2026-10-10).
STAND_PIN_RIB_W = 3.2                  # rib round the pin between the pocket and the cover edge

# The button. "tongue" (owner, 2026-10-12: variant A of four, "and make it beautiful"): a tongue cut
# out of the top wall itself, rooted near the corner and running along x, so it bends ALONG the layers
# (the front shell prints screen side down). A nub on its inner face reaches over the PCB to the SW2
# plunger; a shallow dish on its face says where to press. Nothing to fit, nothing to rattle.
# Seen from outside it is one even 0.6 line: two slits joined by a rounded end, each slit ending in a
# half round at the hinge. The slits go straight through the wall, so the tongue slides along them
# and they never pinch. Their ceilings print as bridges, held at both ends. The tongue's own underside
# would be held at the hinge only: its first layer would sag off the free end onto the slit floor. So
# that one layer runs on across the end slit into the wall - a tie 0.12 thick, set back from the face
# - and prints as a bridge held at both ends. The first press snaps it (~0.4 N). A pillar under the
# free end did the same job but stood right behind the slit, in view, and would have left a stub
# there (2026-10-12). "key" is the separate key of 2026-10-09..11, kept as the alternative.
# Back to "key" after the fifth print (owner, 2026-10-12): the tongue looks crude, the separate key neat.
BUTTON_STYLE = "stem" if V0 else "key"   # "tongue" or "key"; "stem" on v0, whose switch is right-angle (BOARD)
TONGUE_L, TONGUE_T = 18.0, 1.0         # hinge to free end; thickness (the wall behind it is cut away)
TONGUE_END = SW2_X + 3.6               # free end; the LED1 window starts 4.5 further on
TONGUE_HALF_H = 2.82                   # half its height, about the LED window's line (z 2.6)
TONGUE_SLIT, TONGUE_GAP = FIT_FOR["TONGUE_SLIT"], FIT_FOR["TONGUE_GAP"]
TONGUE_R = 0.6                         # free-end corners; the slits' outer corners are concentric
TONGUE_ROOT_R = 0.8                    # fillet where the tongue leaves the full wall, inside
TONGUE_NUB_W, TONGUE_NUB_TOP, TONGUE_NUB_FLAT = 2.8, 2.2, 0.4   # the underside rises 45 deg from the
                                       # tongue and is flat for its last 0.4: it clears the PCB edge as it
                                       # moves, and still meets the plunger low enough
TONGUE_DISH_D, TONGUE_DISH_DEPTH = 4.4, 0.30
TONGUE_TIE_W, TONGUE_TIE_BACK = 0.5, 0.5   # print tie across the end slit: depth (y); how far back from the face
TONGUE_OVERTRAVEL = 0.15               # pressed past the click before the switch bottoms
LAYER0, LAYER = 0.2, 0.12              # slice_plates.py: first layer, then 0.12; the tongue's edges sit on them

# button key (owner chose a separate key over the flexure tongue, 2026-10-09)
BTN_CAP_W, BTN_CAP_H, BTN_CAP_Z = 8.50, 3.70, (SW2_Z if V1 else 2.0)   # visible pill; v2's plunger tip spans
                                       # x 30.19..32.04, z 0.74..1.59; on v1 the key is centred on the plunger
# 8.0 x 3.2 until 2026-10-13, then 8.1 x 3.3, 8.2 x 3.4, 8.26 x 3.46, 8.32 x 3.52, now 8.50 x 3.70 (the three-dot
# trial key): the key grows as BTN_CLEAR shrinks (0.40, 0.35, 0.30, 0.27, 0.24, 0.15), so the shell printed on
# 2026-10-12, its hole and its tab slots fit every one unchanged
BTN_CLEAR, BTN_GAP = FIT_FOR["BTN_CLEAR"], FIT_FOR["BTN_GAP"]   # hole clearance; key back to plunger (FIT_FOR)
# The key is loose in y. Its two end tabs sit in slots cut into the wall's inner face, and it rests on the
# SW2 plunger by its own weight: the top wall is up whether the case stands or hangs. From 2026-10-11 the
# key was pressed onto the wall by a 0.10 preload on the plunger; printed (2026-10-12) the plunger met the
# key's back while the board was laid in, the board would not go down and the key stood flush with the
# face. Now, with the board up against the top wall and the key out on its slot floors, the plunger stays
# BTN_GAP short of the key, and the slots put the key further out (owner: tabs into slots in the wall, the
# key itself higher).
# BTN_OUT is the top above the wall face with the key out on its slot floors - where it stood on the
# 2026-10-12 print, which the owner liked ("the height is excellent", 2026-10-13). Resting on the plunger of a
# board on its bottom stops it stands key_drop() lower. Widening the gap to the plunger (BTN_GAP) takes
# material off the key's back and leaves its top where it is. (Before 2026-10-13 the resting height was the
# fixed one: 1.0, owner 2026-10-12 "at least a millimetre"; where SW2 clicks is not a constraint.)
BTN_OUT = 1.30
SW2_TRAVEL = 0.25                      # to the click (TS-1102 side-push datasheet)
# BTN_CHANNEL: tried and dropped the same day (owner, 2026-10-13: printed, the key stayed pressed either way
# round - "make it flat again"). Kept switchable, off.
BTN_CHANNEL = False
# A U-channel on the key's back holds SW2's plunger, so the key sits on the plunger instead of rattling in
# its hole (owner's idea, 2026-10-13). Open towards the cover and flared there: the board is laid in from
# that side and the plunger has to slide in. The bar on the front side stops BTN_BAR_GAP short of the
# plunger: the plunger never lands on it while the board goes down (the case is face down then, the key
# lies on the hole's front side, 0.3 further away), and in use it takes the key's play towards the cover.
# A bar on the cover side could not be passed at all. Depth: the rails reach 0.5 past the key's back;
# pressed to SW2's full travel (~0.35) their ends stay 0.07 over the two bumps on SW2's face (y 88.835,
# beside the plunger) and 0.27 over the face itself.
BTN_RAIL_DEPTH, BTN_RAIL_W, BTN_RAIL_CLEAR = 0.5, 0.6, 0.15   # past the back; rail width; to the plunger's sides
BTN_RAIL_FUNNEL, BTN_RAIL_LEAD = 0.5, 0.2   # flare at the open end; lead-in on the rails' tips
BTN_BAR_GAP = 0.10                          # bar to the plunger's front side, key centred in its hole
BTN_WELL_D, BTN_WELL_MARGIN = 0.0, 1.5   # well round the key 2026-10-11..12; the owner wants the face flat
# The flange runs back to the key's back (2026-10-13): the key stands on one flat face, tabs on the bed (owner:
# "print them standing on the tabs", the pill then lies in the printer's XY plane). It reaches 0.21 past the
# wall's inner face into the cavity, where nothing is - the drop test below checks.
# Tabs only at the key's two ends, along x (owner, 2026-10-12). The flange ran round the top and the ends
# too, and below the key it was cut off at the PCB edge: one-sided, so the key hung off-centre and bent.
BTN_FLANGE = 1.0                       # tabs past the hole at the two ends
BTN_FLANGE_Z0 = BTN_CAP_Z - 1.51 if V1 else PCB_TOP + 0.40   # v2: no tab below the key, the PCB edge is there;
                                       # as tall above its middle. v1: the same tabs, centred on the key
# v1's plunger tip stands 0.26 past the board edge, 0.87 nearer the outside than v2's: with the key's back BTN_GAP
# off it, a 1.0 tab in a 1.0 slot would end behind the key's back. So on v1 the slots go 1.26 deep (0.74 of wall
# behind) and the tabs are 0.6, the key's back flush with theirs as on v2 (it prints standing on that face)
BTN_SLOT_D = 1.26 if V1 else 1.0       # tab slots into the wall's inner face; on v2 1.0 of the 2.0 wall stays behind
BTN_TAB_T = 0.6 if V1 else 1.0         # v2: the tabs fill the slot: 1 mm of wall and 1 mm of tab (owner, 2026-10-12),
                                       # so out on its slot floors the key's tabs are flush with the wall's inside
BTN_SEAT_CLEAR, BTN_SEAT_Z0 = 0.3, (BTN_FLANGE_Z0 - 0.25 if V1 else PCB_TOP + 0.15)   # round the tabs; the slot
                                       # floor stays above the PCB
# v1: the TS-1102US has four bosses round its plunger, their tips 0.33 behind the plunger's, and the key's back
# corners overlap them by ~0.15 in z: at full travel they would meet. Two shallow steps across the back clear them
KEY_EAR_X = [(28.01, 29.05), (32.41, 33.45)] if V1 else []   # the bosses' x spans; z: below / above KEY_EAR_Z
KEY_EAR_Z = (2.81, 6.17)
KEY_EAR_CLEAR, KEY_EAR_DEPTH = 0.2, 0.4
BTN_BACK_CHAMFER = 0.15                # lead-in round the key's back; less than the back's reach past the tabs
# Trial keys (owner, 2026-10-13: 0.24 still had a little play - "four sizes up, mark them"). Each is the same
# key with the hole clearance below, built off to the side as "CPN key cX.XX" and marked by dots on its back:
# one dot for the first, four for the last. Empty list: none built. Printed 2026-10-13: three dots (0.15) was
# "perfect" and became the key above, so none are built now.
# Resin (owner, 2026-10-13): its 0.20 is set from JLC3DP's rules, not from a print, and +-0.2 can make the key
# tight or loose, so four trial keys go with the first order: two tighter, two looser. The dots are JLC3DP's
# smallest engraving (0.8 deep and wide): FDM's two-layer 0.32 would not show in resin.
KEY_TRIALS = [] if FDM else [0.10, 0.15, 0.25, 0.30]
KEY_TRIAL_DX0, KEY_TRIAL_STEP = -60.0, -16.0     # where they stand, along x from the real key
KEY_MARK_D, KEY_MARK_DEPTH, KEY_MARK_PITCH = (0.9, 0.32, 1.4) if FDM else (1.0, 0.8, 1.8)
KEY_MARK_DZ = 0.5                      # dots above the plunger (z <= 1.59); FDM: 0.32 = two layers
SW2_TIP = (29.06, 32.41, 2.81, 6.17) if V1 else (30.19, 32.04, 0.74, 1.59)   # plunger tip x0, x1, z0, z1
                                       # (board model); v2: its body starts ~1 mm back

# vents: the SCD41 sits 3.5 mm from the left wall (model -x), the BME690 4.2 mm from the right
VENT_SCD_Y = [67.5, 70.5, 73.5, 76.5, 79.5]
VENT_SCD_Z = (1.0, 6.4)
VENT_BME_Y = [61.5, 64.5, 67.5, 70.5]   # the same on v1 (owner, 2026-10-14)
VENT_BME_Z = (0.6, 4.0)
VENT_W = 1.2
USB_CLEAR = 0.25                       # around the shell; 0.15 is enough for SLA
USB_WEB = 0.6                          # wall left around the receptacle; the rest is the plug relief
USB_PLUG_W, USB_PLUG_H = 12.8, 7.0     # relief for the plug's overmould: the receptacle sits 2.7 mm inside
COVER_VENT_X = (9.0, 21.0)             # the same five rows continue across the cover over the SCD41
if V0:   # the SCD41 module lies flat by the left wall, its sensor (10.1 square, 6.5 tall) at the wall end; no BME690
    VENT_SCD_Y, VENT_SCD_Z, VENT_BME_Y = [74.5, 77.5, 80.5, 83.5, 86.5], (1.6, 8.0), []
    COVER_VENT_X = (12.0, 24.0)
COVER_VENT_Y = VENT_SCD_Y
# v0 only: the microSD socket opens towards the right wall (photos: the card goes in towards -x). Where the 11 mm
# card runs inside the 14.9 wide socket is not known, so the slot spans the whole socket (y_up 45.5..60.4, from
# photo e2bcbfbf anchored on the socket's two peg holes in the OBJ). Round it, a dish in the outer face leaves
# SD_DISH[2] less wall for a nail; the socket's mouth stands 2 inside the board's edge
SD_Y, SD_W, SD_Z = PCB_Y0 + 52.95, 15.4, (PCB_TOP + 0.15, PCB_TOP + 2.35)
SD_DISH = (18.4, 7.0, 1.2)             # dish round the slot: length (y), height (z), depth
# v0 only: the slide switch sits in the right wall in the old case's notch (Epaper-c6: y_up 70..75.5, from 0.75
# over the PCB; photos: its body stands in the notch, its black lever out past the wall), open to the rim so the
# board goes in. A tongue on the cover fills the notch down to SLIDE_TOP, over the switch (owner, 2026-10-14: the
# notch open to the rim was "a hole"), so from outside only a window round the switch shows. SS12D00 lying on the
# board: ~3.5 tall, so SLIDE_TOP is a guess to be measured; "ON" is towards the top (the old case's engraving)
SLIDE_Y, SLIDE_Z0, SLIDE_TOP = (PCB_Y0 + 70.0, PCB_Y0 + 75.5), PCB_TOP + 0.75, PCB_TOP + 4.0
SLIDE_FILL_CLEAR = 0.15                # the cover's tongue in the notch, each side
SLIDE_TEXT_H, SLIDE_TEXT_D = 2.4, 0.4  # ON / OFF engraved beside it: letter height, depth

# ---------------------------------------------------------------- derived --
IN_X0, IN_X1 = PCB_X0 - FIT, PCB_X1 + FIT
RAIL_SIDES = [(IN_X0, 1), (IN_X1, -1)] # (side wall face, direction into the case)
RAIL_TOOTH_X = [xw + sg * span / 2 for (xw, sg), span in zip(RAIL_SIDES, RAIL_SPAN)]
RAIL_Z0, RAIL_Z1 = PCB_TOP + CATCH_PLAY - RAIL_DROP, PCB_TOP + CATCH_PLAY + RAIL_RISE   # the bar's bottom and top
LATCH_X = RAIL_TOOTH_X if LATCH_STYLE == "rail" else \
    [xw + sg * (PULL_END_GAP + PULL_PRONG / 2) for xw, sg in RAIL_SIDES] if LATCH_STYLE == "pull" else HOOK_X
IN_Y0, IN_Y1 = PCB_Y0 - FPC_ROOM, PCB_Y1 + FIT_TOP
# v1: what of the board reaches into a wall, and how deep its groove in the wall's inner face goes (0 on v2)
USB_GROOVE = max(0.0, USB_FACE + 0.15 - IN_X1)              # receptacle past the right wall's inner face
USB_SLIDE = 0.4 if V0 else LEDGE_OVER + 0.2                 # the board's slide up under the ledges, and a bit;
                                                            # v0 goes in flat and only floats
# The plug relief stops USB_WEB short of the groove: the groove stays inside the wall and the opening is the
# receptacle's stadium from outside (owner, 2026-10-14). On v2 (no groove) it is the 1.4 it always was
USB_RELIEF = WALL - USB_GROOVE - USB_WEB
# Slid up under the ledges with its bottom end lifted (USB_SLIDE above; tilt in the report, 5.3 deg to clear the
# whiskers' hoods), the board leans: the receptacle moves up its groove (USB_LEAN) and v1's tall switch leans into
# the top wall. Both are cut for SLIDE_TILT, 0.7 deg over what is needed, with the board pushed up (FIT_TOP)
SLIDE_TILT = 6.0
USB_LEAN = 0.4 if V0 else 0.5          # past the opening's width towards the top: 0.34 at SLIDE_TILT, +FIT_TOP
# v1's TS-1102US body, read off the model 2026-10-14: x span, its lowest part that leans into the wall (the lower
# bosses round the plunger) and its top; the bosses' tips stand 0.33 behind the plunger's
SW2_BODY = (27.18, 34.28, 1.74, 7.63) if V1 else None
SW2_BOSS_Y = SW2_TIP_Y - 0.33
# v1: a recess in the top wall's inner face over the switch, from below its lower bosses up to the rim, deep
# enough for the bosses at its top leaning out at SLIDE_TILT. It also takes the plunger past the wall's face
# (board pushed up), which was the plunger groove of 2026-10-13. 0 on v2
SW2_GROOVE = 0.0 if not V1 else \
    SW2_BOSS_Y + (SW2_BODY[3] - PCB_TOP) * math.sin(math.radians(SLIDE_TILT)) + FIT_TOP + 0.1 - IN_Y1
OUT_X0, OUT_X1, OUT_Y0, OUT_Y1 = IN_X0 - WALL, IN_X1 + WALL, IN_Y0 - WALL, IN_Y1 + WALL
PANEL_X0 = (PCB_X0 + PCB_X1 - PANEL_W) / 2
PANEL_Y0 = PCB_Y0 + PANEL_Y_OFF if V0 else (PCB_Y0 + PCB_Y1 - PANEL_H) / 2
PANEL_BACK = PCB_BOT - TAPE_PANEL
PANEL_FRONT = PANEL_BACK - PANEL_T
LIP_IN = PANEL_FRONT - FRONT_GAP
FRONT = LIP_IN - BEZEL_T
CELL_TOP = PCB_TOP + CELL_TAPE + CELL_T
STAND_AXIS_Z = None                    # set below, needs BACK
COVER_IN = CELL_TOP + CELL_CLEAR
BACK = COVER_IN + COVER_T
STAND_AXIS_Z = BACK - STAND_T / 2
STAND_FLOOR_Z = BACK - STAND_T - STAND_FLOOR_GAP
STAND_HEEL_END = STAND_HINGE_Y + STAND_HEEL
STAND_POCKET_Y0 = STAND_FOOT_Y - 0.3
STAND_POCKET_Y1 = STAND_HEEL_END + 0.3     # the heel's top corner swings out ~0.15 on its way down
_SB = math.radians(STAND_STOP)
# the heel's lower corner, (STAND_HEEL, -T/2) from the axis, lands on the recess floor at the stop
STAND_RECESS_Z = STAND_AXIS_Z - STAND_HEEL * math.sin(_SB) - STAND_T / 2 * math.cos(_SB)
STAND_RECESS_Y0 = STAND_HINGE_Y - STAND_T / 2 * math.sin(_SB) - 0.5   # where the heel's underside dips


def stand_world(d, deg):
    """A point (dy, dz) of the flap, in model (y, z) with the flap opened by deg."""
    a = math.radians(deg)
    return (STAND_HINGE_Y + d[0] * math.cos(a) + d[1] * math.sin(a),
            STAND_AXIS_Z - d[0] * math.sin(a) + d[1] * math.cos(a))


def on_layer(z):
    """The nearest layer boundary of the PLA print (front shell, screen side on the bed)."""
    return FRONT + LAYER0 + LAYER * round((z - FRONT - LAYER0) / LAYER)


def on_layer_up(z):
    """The first layer boundary at or above z."""
    return FRONT + LAYER0 + LAYER * math.ceil((z - FRONT - LAYER0) / LAYER - 1e-9)


PULL_Z0, PULL_Z1 = on_layer(RAIL_Z0), RAIL_Z1          # the bar's bottom (on a layer: the tie) and top
PULL_ZC = on_layer_up(PCB_TOP + CATCH_PLAY)            # the prongs' flat underside
PULL_H = PULL_Z1 - PULL_Z0
PULL_FACE = PCB_Y0 - PULL_SET                          # the bar's face towards the board ...
PULL_BACK = PULL_FACE - PULL_D                         # ... and its back
PULL_ROOM = PULL_BACK - IN_Y0                          # how far the free end can go before the wall stops it
PULL_L = PULL_ROOT - PULL_END_GAP                      # root face to the free end
PULL_PRONGS = [(PULL_L - 2 * PULL_PRONG - PULL_FORK, PULL_L - PULL_PRONG - PULL_FORK),   # near, far: (u0, u1)
               (PULL_L - PULL_PRONG, PULL_L)]          # from the root face
PULL_FORK_U = (PULL_L - PULL_PRONG - PULL_FORK, PULL_L - PULL_PRONG)
PULL_A = sum(PULL_FORK_U) / 2                          # where the clip pushes, from the root face
PULL_HOLE_Y = PULL_FACE - PULL_PARK + PULL_PIN_D - PULL_PIN_HOLE / 2   # the bar rests on the clip, the clip
                                                       # on the hole's board side
PULL_BOSS_TOP = on_layer(PULL_Z0 - 0.5)                # under the bar, with 4 layers of air
PULL_HOLE_BOTTOM = LIP_IN - COG_RELIEF - 0.4           # 0.6 of the bezel left under it


def pull_defl(u, da):
    """How far back the bar is at u from its root when the clip holds the fork da back: a cantilever with one
    load at PULL_A (curved up to it, straight beyond)."""
    a = PULL_A
    return da * u * u * (3 * a - u) / (2 * a ** 3) if u <= a else da * (3 * u - a) / (2 * a)


def pull_tip(u):
    """A prong's tip line, chosen so that held back by the clip it stands PULL_CLEAR off the board edge."""
    return PCB_Y0 - PULL_CLEAR + pull_defl(u, PULL_PARK)


def pull_x(xw, sg, u):
    """Model x of the point u from a whisker's root face, towards its side wall."""
    return xw + sg * (PULL_ROOT - u)


TONGUE_X0 = TONGUE_END - TONGUE_L
TB_Z = on_layer(PCB_TOP + FIT_FOR["TB_PLAY"])          # boss top = the turnbutton's underside
TB_TOP = TB_Z + TB_T
TB_HUB_R = TB_HEAD_D / 2
TB_YS = PCB_Y0 - TB_EDGE_CLEAR - TB_HUB_R             # screw axis, in the strip under the board's edge
TB_ARM_TIP = PCB_Y0 + TB_OVER
TB_REACH = TB_ARM_TIP - TB_YS                          # axis to the arm's tip
TB_NOTCH_Y = TB_YS - TB_HUB_R - TB_NOTCH_CLEAR         # the notch's back face, in the bottom wall
TB_NOTCH_TOP = TB_TOP + TB_HEAD_K + 0.25               # its ceiling at the back; it rises 45 deg to the cavity
TB_FLOOR = LIP_IN - COG_RELIEF                         # the FPC room's floor at the corners
TB_FINGER_X = (TB_HUB_R + TB_FINGER_GAP, TB_HUB_R + TB_FINGER_GAP + TB_FINGER_W)   # |x - screw axis|
TB_FINGER_Y = (IN_Y0 + 0.3, PCB_Y0 - 0.15)
TB_FINGER_Z0 = TB_TOP - TB_FINGER_DOWN
TONGUE_Z0, TONGUE_Z1 = on_layer(LED_HOLE_Z - TONGUE_HALF_H), on_layer(LED_HOLE_Z + TONGUE_HALF_H)
TONGUE_YT = OUT_Y1 - TONGUE_T          # the tongue's inner face
TONGUE_NUB_Y = SW2_TIP_Y + TONGUE_GAP  # the nub's tip
TONGUE_NUB_Z = TONGUE_Z0 + LAYER       # the nub's underside where it leaves the tongue
TONGUE_NUB_TIP_Z = TONGUE_NUB_Z + (TONGUE_YT - TONGUE_NUB_Y - TONGUE_NUB_FLAT)   # ... and at its tip

STAND_BLOCK_X = [(STAND_X0 + STAND_SIDE + STAND_TONGUE_W + STAND_BLOCK_CLEAR,
                  STAND_X0 + STAND_SIDE + STAND_TONGUE_W + STAND_SLIT - STAND_BLOCK_BODY),
                 (STAND_X1 - STAND_SIDE - STAND_TONGUE_W - STAND_SLIT + STAND_BLOCK_BODY,
                  STAND_X1 - STAND_SIDE - STAND_TONGUE_W - STAND_BLOCK_CLEAR)]
STAND_DISH = (4.0, 1.5, 0.4)           # half-length, half-width, depth: a finger dish on the heel
# Nail catch at the foot (owner, 2026-10-13: "a notch on the leg to hook a fingernail in and pull it out";
# changing the cover is fine). The foot end sits 0.3 off the pocket's end wall, too tight for a nail, and
# a groove across the flap's face would not help: the foot moves straight out, so a nail needs something
# to lift UNDER. So the catch is two parts. A ramp in the cover's 1.2 mm strip at the pocket's foot end
# leads the nail down below the flap's face; a hollow under the foot's middle, open to the flap's end and
# underside, roofed by its outer skin, gives it a lip to lift by. From behind only the ramp shows - the
# flap's face stays whole. Both print without help: the flap prints face down, so the lip is its first
# layers on the bed and the hollow a plain pocket from the top; the cover prints face down too, and the
# ramp is a ceiling ~50 deg off the bed.
STAND_NAIL_W = 10.0                    # across x, the ramp and the hollow
STAND_NAIL_RAMP = 1.4                  # ramp depth at the pocket's end wall; 0 at the cover's edge
STAND_NAIL_LIP = LAYER0 + 4 * LAYER if FDM else 0.80   # the lip: the flap's outer skin over the hollow,
                                                        # 5 print layers; resin: JLC3DP's 0.8 minimum wall
STAND_NAIL_HOLLOW = 2.0                # how far the hollow runs up the flap from its foot end
COVER_EDGE_Y0 = IN_Y0 - REBATE + COVER_FIT    # the cover's bottom edge
MAG_BOTTOM = BACK - MAG_POCKET_DEPTH - MAG_FLOOR
COVER_EDGE_X = IN_X1 + REBATE - COVER_FIT    # the cover's right edge
PIN_X0 = STAND_X0 - STAND_WALL + 0.2     # blind end of the pin hole, inside the left pocket wall
PILOT_BOTTOM = FRONT + 0.6               # v0: the cover screws' pilots end 0.6 short of the front face


# ======================================================================= helpers ==
def cm(v):
    return v / 10.0


def V(v):
    return adsk.core.ValueInput.createByReal(cm(v))


class Plane:
    """An offset construction plane. world(u, v) maps the plane's 2-D coordinates:
    'z' -> (u, v, c),  'x' -> (c, u, v),  'y' -> (u, c, v)."""

    def __init__(self, comp, axis, c, name):
        self.comp, self.axis, self.c = comp, axis, c
        base = {"z": comp.xYConstructionPlane, "x": comp.yZConstructionPlane,
                "y": comp.xZConstructionPlane}[axis]
        idx = "xyz".index(axis)
        for sign in (1, -1):
            pi = comp.constructionPlanes.createInput()
            pi.setByOffset(base, V(sign * c))
            p = comp.constructionPlanes.add(pi)
            o = p.geometry.origin
            if abs((o.x, o.y, o.z)[idx] * 10 - c) < 1e-4:
                break
            p.deleteMe()
        p.name = name
        p.isLightBulbOn = False
        self.plane = p
        n = p.geometry.normal
        self.normal_sign = 1 if (n.x, n.y, n.z)[idx] > 0 else -1

    def world(self, u, v):
        return {"z": (u, v, self.c), "x": (self.c, u, v), "y": (u, self.c, v)}[self.axis]

    def sketch(self, name):
        sk = self.comp.sketches.add(self.plane)
        sk.name = name
        sk.isLightBulbOn = False
        return Sketch(sk, self)


class Sketch:
    def __init__(self, sk, plane):
        self.sk, self.pl = sk, plane
        n = sk.xDirection.crossProduct(sk.yDirection)
        self.normal_sign = 1 if (n.x, n.y, n.z)["xyz".index(plane.axis)] > 0 else -1

    def pt(self, u, v):
        x, y, z = self.pl.world(u, v)
        p = self.sk.modelToSketchSpace(adsk.core.Point3D.create(cm(x), cm(y), cm(z)))
        return adsk.core.Point3D.create(p.x, p.y, 0)

    def poly(self, pts):
        lines = self.sk.sketchCurves.sketchLines
        ps = [self.pt(u, v) for u, v in pts]
        for a, b in zip(ps, ps[1:] + ps[:1]):
            lines.addByTwoPoints(a, b)

    def rrect(self, u0, v0, u1, v1, r):
        if r <= 0:
            return self.poly([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
        L = self.sk.sketchCurves.sketchLines
        A = self.sk.sketchCurves.sketchArcs
        k = r * (1 - math.sqrt(0.5))
        L.addByTwoPoints(self.pt(u0 + r, v0), self.pt(u1 - r, v0))
        L.addByTwoPoints(self.pt(u1, v0 + r), self.pt(u1, v1 - r))
        L.addByTwoPoints(self.pt(u1 - r, v1), self.pt(u0 + r, v1))
        L.addByTwoPoints(self.pt(u0, v1 - r), self.pt(u0, v0 + r))
        A.addByThreePoints(self.pt(u1 - r, v0), self.pt(u1 - k, v0 + k), self.pt(u1, v0 + r))
        A.addByThreePoints(self.pt(u1, v1 - r), self.pt(u1 - k, v1 - k), self.pt(u1 - r, v1))
        A.addByThreePoints(self.pt(u0 + r, v1), self.pt(u0 + k, v1 - k), self.pt(u0, v1 - r))
        A.addByThreePoints(self.pt(u0, v0 + r), self.pt(u0 + k, v0 + k), self.pt(u0 + r, v0))

    def path(self, start, segs):
        """A closed outline from `start`: each seg is (end,) for a line or (mid, end) for an arc
        through mid. The last end must be `start`."""
        L, A = self.sk.sketchCurves.sketchLines, self.sk.sketchCurves.sketchArcs
        p = start
        for seg in segs:
            if len(seg) == 1:
                L.addByTwoPoints(self.pt(*p), self.pt(*seg[0]))
            else:
                A.addByThreePoints(self.pt(*p), self.pt(*seg[0]), self.pt(*seg[1]))
            p = seg[-1]

    def slot(self, uc, vc, w, h):
        self.rrect(uc - w / 2, vc - h / 2, uc + w / 2, vc + h / 2, min(w, h) / 2 - 1e-3)

    def circle(self, u, v, d):
        self.sk.sketchCurves.sketchCircles.addByCenterRadius(self.pt(u, v), cm(d / 2))

    def profiles(self, pick=None):
        oc = adsk.core.ObjectCollection.create()
        for p in self.sk.profiles:
            if pick is None or pick(p):
                oc.add(p)
        if oc.count == 0:
            raise RuntimeError("sketch %s has no profiles" % self.sk.name)
        return oc


NEW = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
CUT = adsk.fusion.FeatureOperations.CutFeatureOperation
JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation


def extrude(s, profiles, start, end, op, body=None, name=None):
    """Extrude from the sketch plane (at `start`, which must equal the plane offset) to `end`."""
    ext = s.pl.comp.features.extrudeFeatures
    ei = ext.createInput(profiles, op)
    d = end - start
    direction = adsk.fusion.ExtentDirections.PositiveExtentDirection if d * s.normal_sign > 0 \
        else adsk.fusion.ExtentDirections.NegativeExtentDirection
    ei.setOneSideExtent(adsk.fusion.DistanceExtentDefinition.create(V(abs(d))), direction)
    if body is not None:
        ei.participantBodies = [body]
    f = ext.add(ei)
    if name:
        f.name = name
    return f


def extrude_sym(s, profiles, full, op, body=None, name=None):
    ext = s.pl.comp.features.extrudeFeatures
    ei = ext.createInput(profiles, op)
    ei.setSymmetricExtent(V(full), True)
    if body is not None:
        ei.participantBodies = [body]
    f = ext.add(ei)
    if name:
        f.name = name
    return f


def planar_face(body, axis, value):
    idx = "xyz".index(axis)
    for f in body.faces:
        g = f.geometry
        if g.objectType != adsk.core.Plane.classType():
            continue
        n = (g.normal.x, g.normal.y, g.normal.z)
        if abs(abs(n[idx]) - 1) > 1e-6:
            continue
        b = f.boundingBox
        if abs((b.minPoint.x, b.minPoint.y, b.minPoint.z)[idx] * 10 - value) < 1e-3:
            return f
    raise RuntimeError("no planar face at %s = %.3f" % (axis, value))


def loop_edges(face, outer):
    oc = adsk.core.ObjectCollection.create()
    for lp in face.loops:
        if lp.isOuter == outer:
            for e in lp.edges:
                oc.add(e)
    return oc


def fillet(comp, edges, r, name):
    fl = comp.features.filletFeatures
    fi = fl.createInput()
    fi.edgeSetInputs.addConstantRadiusEdgeSet(edges, V(r), True)
    f = fl.add(fi)
    f.name = name


def chamfer(comp, edges, d, name):
    ch = comp.features.chamferFeatures
    ci = ch.createInput2()
    ci.chamferEdgeSets.addEqualDistanceChamferEdgeSet(edges, V(d), True)
    f = ch.add(ci)
    f.name = name


def new_component(root, name):
    occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    occ.component.name = name
    return occ.component


def box(comp, name, x0, y0, z0, x1, y1, z1):
    s = Plane(comp, "z", z0, name + " base").sketch(name)
    s.rrect(x0, y0, x1, y1, 0)
    b = extrude(s, s.profiles(), z0, z1, NEW).bodies.item(0)
    b.name = name
    return b


# ======================================================================= parts ==
def build_panel(root):
    c = new_component(root, "SM Panel GDEY042T81")
    b = box(c, "Panel", PANEL_X0, PANEL_Y0, PANEL_FRONT, PANEL_X0 + PANEL_W, PANEL_Y0 + PANEL_H, PANEL_BACK)
    return c, b


def build_inserts(root):
    """Stand-in for the real 4 mm inserts: the board model's are 1.7 mm short, and the clearance
    checks have to see the real ones."""
    c = new_component(root, "SM Inserts M2 (measured)")
    s = Plane(c, "z", PCB_TOP, "PCB top").sketch("inserts")
    for x, y in INSERTS:
        s.circle(x, y, INSERT_D)
    extrude(s, s.profiles(), PCB_TOP, INSERT_TOP, NEW, name="inserts")
    return c


def build_cell(root):
    c = new_component(root, "SM Cell " + CELL_NAME)
    x0, y0 = CELL_C[0] - CELL_L / 2, CELL_C[1] - CELL_W / 2
    box(c, "Cell", x0, y0, PCB_TOP + CELL_TAPE, x0 + CELL_L, y0 + CELL_W, CELL_TOP)
    return c


def build_front(root):
    c = new_component(root, "SM Front")
    s = Plane(c, "z", FRONT, "front face").sketch("outline")
    s.rrect(OUT_X0, OUT_Y0, OUT_X1, OUT_Y1, R_OUT)
    body = extrude(s, s.profiles(), FRONT, BACK, NEW, name="shell").bodies.item(0)
    body.name = "Front"

    s = Plane(c, "z", LIP_IN, "lip inner").sketch("cavity")
    s.rrect(IN_X0, IN_Y0, IN_X1, IN_Y1, R_IN)
    extrude(s, s.profiles(), LIP_IN, BACK, CUT, body, "cavity")

    s = Plane(c, "z", COVER_IN, "cover seat").sketch("rebate")
    s.rrect(IN_X0 - REBATE, IN_Y0 - REBATE, IN_X1 + REBATE, IN_Y1 + REBATE, R_REBATE)
    extrude(s, s.profiles(), COVER_IN, BACK, CUT, body, "cover rebate")

    # window = the panel's viewing area
    s = Plane(c, "z", FRONT, "front face (window)").sketch("window")
    wx0, wx1 = PANEL_X0 + VA_MARGIN, PANEL_X0 + PANEL_W - VA_MARGIN
    wy0, wy1 = PANEL_Y0 + VA_MARGIN_FPC, PANEL_Y0 + PANEL_H - VA_MARGIN
    s.rrect(wx0, wy0, wx1, wy1, 0.6)
    extrude(s, s.profiles(), FRONT, LIP_IN, CUT, body, "window")

    # the lip must not touch the driver IC along the FPC edge
    s = Plane(c, "z", LIP_IN, "lip inner (COG)").sketch("COG relief")
    s.rrect(IN_X0 + 2, IN_Y0, IN_X1 - 2, PANEL_Y0 + COG_BAND + 0.5, 0)
    extrude(s, s.profiles(), LIP_IN, LIP_IN - COG_RELIEF, CUT, body, "COG relief")

    # edges: front perimeter, back perimeter, window
    fillet(c, loop_edges(planar_face(body, "z", FRONT), True), 1.5, "front edge")
    fillet(c, loop_edges(planar_face(body, "z", BACK), True), 0.6, "back edge")
    chamfer(c, loop_edges(planar_face(body, "z", FRONT), False), 0.8, "window chamfer")

    # USB-C: a stadium hugging the receptacle shell, cut through a thin web; outside it a concentric
    # relief so the plug's overmould can come within (FIT + 0.37 + USB_WEB) of the receptacle face
    xr = OUT_X1 - WALL / 2
    s = Plane(c, "x", xr, "right wall").sketch("USB")
    s.slot(USB_Y, USB_Z, USB_W + 2 * USB_CLEAR, USB_H + 2 * USB_CLEAR)
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "USB opening")
    s = Plane(c, "x", OUT_X1, "right wall outer").sketch("USB plug relief")
    s.slot(USB_Y, USB_Z, USB_PLUG_W, USB_PLUG_H)
    extrude_sym(s, s.profiles(), 2 * USB_RELIEF, CUT, body, "USB plug relief")
    if USB_GROOVE > 0:
        # v1: the receptacle stands 1.03 into the wall, so the board could not go down past it. A groove in the
        # wall's inner face runs from the opening up to the rim, USB_SLIDE longer towards the bottom: the board
        # slides up under the ledges with it. The plug relief above is shallow enough to leave USB_WEB outside the
        # groove, so from outside only the opening shows
        s = Plane(c, "x", IN_X1 - 0.1, "right wall inner (USB groove)").sketch("USB groove")
        hw = USB_W / 2 + USB_CLEAR
        s.rrect(USB_Y - hw - USB_SLIDE, USB_Z - USB_H / 2 - USB_CLEAR, USB_Y + hw + USB_LEAN, BACK + 0.5, 0)
        extrude(s, s.profiles(), IN_X1 - 0.1, IN_X1 + USB_GROOVE, CUT, body, "USB groove")

    # vents
    s = Plane(c, "x", OUT_X0 + WALL / 2, "left wall").sketch("SCD41 vents")
    for y in VENT_SCD_Y:
        s.slot(y, sum(VENT_SCD_Z) / 2, VENT_W, VENT_SCD_Z[1] - VENT_SCD_Z[0])
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "SCD41 vents")
    if VENT_BME_Y:   # none on v0
        s = Plane(c, "x", xr, "BME wall").sketch("BME690 vents")
        for y in VENT_BME_Y:
            s.slot(y, sum(VENT_BME_Z) / 2, VENT_W, VENT_BME_Z[1] - VENT_BME_Z[0])
        extrude_sym(s, s.profiles(), 4.0, CUT, body, "BME690 vents")

    if BUTTON_STYLE == "tongue":
        build_tongue(c, body)
    elif BUTTON_STYLE == "stem":   # v0: SW2's stem, the shell's bosses, the microSD slot, the slide switch
        build_v0_walls(c, body)
    else:
        # button: a pill-shaped hole for the separate key (a well round it only if BTN_WELL_D > 0)
        s = Plane(c, "y", OUT_Y1 - WALL / 2, "top wall").sketch("button hole")
        s.slot(SW2_X, BTN_CAP_Z, BTN_CAP_W + 2 * BTN_CLEAR, BTN_CAP_H + 2 * BTN_CLEAR)
        extrude_sym(s, s.profiles(), 4.0, CUT, body, "button hole")
        if BTN_WELL_D > 0:
            m = BTN_CLEAR + BTN_WELL_MARGIN
            s = Plane(c, "y", OUT_Y1, "top wall outer (key well)").sketch("key well")
            s.slot(SW2_X, BTN_CAP_Z, BTN_CAP_W + 2 * m, BTN_CAP_H + 2 * m)
            extrude(s, s.profiles(), OUT_Y1, OUT_Y1 - BTN_WELL_D, CUT, body, "key well")
        # slots for the key's end tabs in the wall's inner face. Their ceiling rises 45 deg towards the
        # inside, so the shell (printed face down) needs no support in them; across the middle the same cut
        # only bevels the hole's inner edge
        y_in, y_seat, (fx0, fx1, fz0, fz1) = key_geometry()
        k = BTN_SEAT_CLEAR
        ya, zt = IN_Y1 - 0.1, fz1 + k
        s = Plane(c, "x", (fx0 + fx1) / 2, "key slots").sketch("key slots")
        s.poly([(ya, BTN_SEAT_Z0), (y_seat, BTN_SEAT_Z0), (y_seat, zt), (ya, zt + (y_seat - ya))])
        extrude_sym(s, s.profiles(), fx1 - fx0 + 2 * k, CUT, body, "key slots")
        if SW2_GROOVE > 0:
            # v1: the switch recess (SW2_GROOVE above); open at the rim, so it prints with no ceiling, and behind
            # the cover's edge there
            s = Plane(c, "y", IN_Y1 - 0.1, "top wall inner (switch recess)").sketch("switch recess")
            s.rrect(SW2_BODY[0] - 0.3, SW2_BODY[2] - 0.3, SW2_BODY[1] + 0.3, COVER_IN + 0.5, 0)
            extrude(s, s.profiles(), IN_Y1 - 0.1, IN_Y1 + SW2_GROOVE, CUT, body, "switch recess")

    # board retention: rigid ledges over the top edge ...
    zc = PCB_TOP + CATCH_PLAY
    for x in LEDGE_X:
        s = Plane(c, "x", x, "ledge %.0f" % x).sketch("ledge %.0f" % x)
        tip = PCB_Y1 - LEDGE_OVER
        s.poly([(tip, zc), (IN_Y1 + 0.2, zc), (IN_Y1 + 0.2, LEDGE_Z1), (tip + 0.4, LEDGE_Z1),
                (tip, LEDGE_Z1 - 0.4)])
        extrude_sym(s, s.profiles(), LEDGE_W, JOIN, body, "ledge %.0f" % x)
    # ... and latches over the bottom edge, flexing into the FPC room (LATCH_STYLE above)
    yi = PCB_Y0 - HOOK_GAP
    yo = yi - HOOK_T
    ytip = PCB_Y0 + HOOK_OVER
    ztip = zc + 0.45
    zr = ztip + (ytip - yi)
    if LATCH_STYLE == "rail":
        floor = LIP_IN - COG_RELIEF - 0.1          # just into the deepest floor of the FPC room
        for (xw, sg), span, d, tw, xt in zip(RAIL_SIDES, RAIL_SPAN, RAIL_DEPTH, RAIL_TOOTH_W, RAIL_TOOTH_X):
            x_face = xw + sg * span                # the anchor's inner face
            x_end = x_face + sg * RAIL_ANCHOR
            s = Plane(c, "z", RAIL_Z0, "rail %.0f" % xw).sketch("rail %.0f" % xw)
            s.rrect(min(xw - sg * 0.5, x_end), yi - d, max(xw - sg * 0.5, x_end), yi, 0)
            extrude(s, s.profiles(), RAIL_Z0, RAIL_Z1, JOIN, body, "latch rail %.0f" % xw)
            xa0, xa1 = sorted((x_face, x_end))
            s = Plane(c, "z", floor, "rail anchor %.0f" % xw).sketch("rail anchor %.0f" % xw)
            s.rrect(xa0, IN_Y0 - 0.5, xa1, yi, 0)
            extrude(s, s.profiles(), floor, RAIL_Z1, JOIN, body, "latch anchor %.0f" % xw)
            s = Plane(c, "x", xt, "rail tooth %.0f" % xw).sketch("rail tooth %.0f" % xw)
            s.poly([(yi - 0.1, zc), (ytip, zc), (ytip, ztip), (yi - 0.1, zr)])
            extrude_sym(s, s.profiles(), tw, JOIN, body, "latch tooth %.0f" % xw)
    elif LATCH_STYLE == "pull":    # whiskers drawn back with a clip (PULL_* above)
        build_pull(c, body)
    elif LATCH_STYLE == "clip":    # the clips are separate parts (build_clips): here only their slots
        tz0, tz1 = clip_tab_z()
        k = CLIP_SLOT_CLEAR
        s = Plane(c, "y", IN_Y0 + 0.1, "clip slots").sketch("clip slots")
        for x in HOOK_X:
            s.rrect(x - CLIP_W / 2 - CLIP_SLOT_X_CLEAR, tz0 - k, x + CLIP_W / 2 + CLIP_SLOT_X_CLEAR, tz1 + k, 0)
        extrude(s, s.profiles(), IN_Y0 + 0.1, IN_Y0 - CLIP_TAB_IN - 0.1, CUT, body, "clip slots")
    elif LATCH_STYLE == "screw":   # the turnbuttons are separate parts (build_turnbuttons): here their seats
        build_turnbutton_seats(c, body)
    elif LATCH_STYLE == "hook":
        floor = LIP_IN - COG_RELIEF                # the FPC room floor the hook stands on
        root_z = floor - 0.3
        r = HOOK_ROOT_R
        yo0, yo1 = yi - HOOK_T_ROOT, yi - HOOK_T_TOP

        def quarter(cy, cz, a0, a1, n=6):
            return [(cy + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)),
                     cz + r * math.sin(math.radians(a0 + (a1 - a0) * k / n))) for k in range(n + 1)]
        # board side first: the snap puts this root corner in tension, and that is where it cracked
        prof = ([(yo0 - r, root_z), (yi + r, root_z)] + quarter(yi + r, floor + r, 270, 180)
                + [(yi, zc), (ytip, zc), (ytip, ztip), (yi, zr), (yo1, zr)]
                + quarter(yo0 - r, floor + r, 0, -90))
        for x in HOOK_X:
            s = Plane(c, "x", x, "latch %.0f" % x).sketch("latch %.0f" % x)
            s.poly(prof)
            extrude_sym(s, s.profiles(), HOOK_W, JOIN, body, "latch %.0f" % x)

    # LED windows
    s = Plane(c, "y", OUT_Y1 - WALL / 2, "top wall (LED1)").sketch("LED1 window")
    s.circle(LED1_X, LED_HOLE_Z, LED_HOLE_D)
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "LED1 window")
    s = Plane(c, "x", OUT_X1 - WALL / 2, "right wall (CHRG)").sketch("CHRG window")
    s.circle(CHRG_Y, LED_HOLE_Z, LED_HOLE_D)
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "CHRG window")
    if STAND_HINGE == "pin":
        # the pin's end crosses the cover shoulder and dips 0.125 mm below the cover: a notch for it
        s = Plane(c, "z", COVER_IN, "shoulder (pin)").sketch("pin notch")
        s.rrect(IN_X1 - 0.2, STAND_HINGE_Y - 1.3, IN_X1 + REBATE, STAND_HINGE_Y + 1.3, 0)
        extrude(s, s.profiles(), COVER_IN, COVER_IN - 0.4, CUT, body, "pin notch")
    return c, body


def build_rear(root):
    c = new_component(root, "SM Rear cover")
    s = Plane(c, "z", COVER_IN, "cover inner").sketch("plate")
    g = COVER_FIT
    s.rrect(IN_X0 - REBATE + g, IN_Y0 - REBATE + g, IN_X1 + REBATE - g, IN_Y1 + REBATE - g,
            R_REBATE - g)
    body = extrude(s, s.profiles(), COVER_IN, BACK, NEW, name="plate").bodies.item(0)
    body.name = "Rear"
    # crush ribs on the edge (COVER_RIB_IN above): a strip with a 45 deg lead-in at its lower end
    p, z0, z1 = g + COVER_RIB_IN, COVER_IN + 0.15, BACK - 0.8
    for ye, sg in ((IN_Y0 - REBATE + g, -1), (IN_Y1 + REBATE - g, 1)):
        for x in COVER_RIBS_X:
            s = Plane(c, "x", x, "cover rib x%.0f y%.0f" % (x, ye)).sketch("cover rib")
            s.poly([(ye - sg * 0.2, z0), (ye + sg * p, z0 + p), (ye + sg * p, z1), (ye - sg * 0.2, z1)])
            extrude_sym(s, s.profiles(), COVER_RIB_W, JOIN, body, "cover rib")
    for xe, sg in ((IN_X0 - REBATE + g, -1), (IN_X1 + REBATE - g, 1)):
        for y in COVER_RIBS_Y:
            s = Plane(c, "y", y, "cover rib x%.0f y%.0f" % (xe, y)).sketch("cover rib")
            s.poly([(xe - sg * 0.2, z0), (xe + sg * p, z0 + p), (xe + sg * p, z1), (xe - sg * 0.2, z1)])
            extrude_sym(s, s.profiles(), COVER_RIB_W, JOIN, body, "cover rib")

    boss_end = INSERT_TOP + BOSS_GAP
    s = Plane(c, "z", boss_end, "boss ends").sketch("bosses")
    for x, y in INSERTS:
        s.circle(x, y, BOSS_D)
    extrude(s, s.profiles(), boss_end, COVER_IN + 0.5, JOIN, body, "bosses")
    if LATCH_STYLE == "screw":
        # fingers either side of each turnbutton's hub (LATCH_STYLE above): they stop it turning, and
        # one left open is under a finger, so the cover will not go on
        s = Plane(c, "z", TB_FINGER_Z0, "turnbutton fingers").sketch("turnbutton fingers")
        for x in HOOK_X:
            for sg in (-1, 1):
                u0, u1 = sorted((x + sg * TB_FINGER_X[0], x + sg * TB_FINGER_X[1]))
                s.rrect(u0, TB_FINGER_Y[0], u1, TB_FINGER_Y[1], 0)
        extrude(s, s.profiles(), TB_FINGER_Z0, COVER_IN + 0.5, JOIN, body, "turnbutton fingers")

    # material the magnet pockets and the stand pocket go into: the plate alone is 2 mm
    s = Plane(c, "z", MAG_BOTTOM, "magnet bosses").sketch("magnet bosses")
    for x, y in MAGNETS:
        s.circle(x, y, MAG_POCKET_D + 2 * MAG_WALL)
    extrude(s, s.profiles(), MAG_BOTTOM, COVER_IN + 0.5, JOIN, body, "magnet bosses")
    # the stand's tub: no wall at the foot end, which runs to 0.3 off the cavity wall so the foot can
    # sit low; deeper under the heel, where the recess floor is the stop
    bz = STAND_FLOOR_Z - STAND_FLOOR
    s = Plane(c, "z", bz, "stand tub").sketch("stand tub")
    s.rrect(STAND_X0 - STAND_WALL, STAND_POCKET_Y0, STAND_X1 + STAND_WALL, STAND_POCKET_Y1 + STAND_WALL, 1.0)
    extrude(s, s.profiles(), bz, COVER_IN + 0.5, JOIN, body, "stand tub")
    hz = STAND_RECESS_Z - STAND_FLOOR
    s = Plane(c, "z", hz, "heel tub").sketch("heel tub")
    s.rrect(STAND_X0 - STAND_WALL, STAND_RECESS_Y0 - STAND_FLOOR, STAND_X1 + STAND_WALL,
            STAND_POCKET_Y1 + STAND_WALL, 1.0)
    extrude(s, s.profiles(), hz, bz + 0.5, JOIN, body, "heel tub")
    if STAND_HINGE == "pin":
        rib_z = STAND_AXIS_Z - STAND_PIN_HOLE / 2 - 1.0
        s = Plane(c, "z", rib_z, "pin rib").sketch("pin rib")
        s.rrect(STAND_X1 + STAND_WALL - 0.5, STAND_HINGE_Y - STAND_PIN_RIB_W / 2, IN_X1 - LED_REFL_GAP,
                STAND_HINGE_Y + STAND_PIN_RIB_W / 2, 0)
        extrude(s, s.profiles(), rib_z, COVER_IN + 0.5, JOIN, body, "pin rib")

    # light turners above the LEDs: a 45 deg underside sends the LED's light to the wall window
    zt = COVER_IN + 0.5
    for name, plane, c0, u1 in (("LED1", ("x", LED1_X), LED1_Y, IN_Y1 - LED_REFL_GAP),
                                ("CHRG", ("y", CHRG_Y), CHRG_X, IN_X1 - LED_REFL_GAP)):
        s = Plane(c, plane[0], plane[1], name + " turner").sketch(name + " turner")
        s.poly(turner_profile(c0, u1, zt))
        extrude_sym(s, s.profiles(), LED_REFL_W, JOIN, body, name + " turner")

    if BUTTON_STYLE == "stem":
        # v0: a tongue fills the top wall's slot above SW2's stem, its end the upper half of the stem's hole
        r = STEM_D / 2 + STEM_CLEAR
        hw = r - STEM_FILL_CLEAR
        zs = SW2_Z + math.sqrt(r * r - hw * hw)
        y0 = IN_Y1 + 0.05                      # the switch's body stands just inside the wall
        s = Plane(c, "y", y0, "stem filler").sketch("stem filler")
        s.path((SW2_X - hw, BACK), [((SW2_X - hw, zs),), ((SW2_X, SW2_Z + r), (SW2_X + hw, zs)),
                                    ((SW2_X + hw, BACK),), ((SW2_X - hw, BACK),)])
        extrude(s, s.profiles(), y0, OUT_Y1, JOIN, body, "stem filler")
        # and one over the slide switch, in the right wall's notch, its end SLIDE_TOP over the PCB
        k = SLIDE_FILL_CLEAR
        x0 = IN_X1 + 0.05
        s = Plane(c, "x", x0, "switch filler").sketch("switch filler")
        s.rrect(SLIDE_Y[0] + k, SLIDE_TOP, SLIDE_Y[1] - k, BACK, 0)
        extrude(s, s.profiles(), x0, OUT_X1, JOIN, body, "switch filler")

    fillet(c, loop_edges(planar_face(body, "z", BACK), True), 0.6, "back edge")

    # magnet pockets, open to the outside
    s = Plane(c, "z", BACK, "back face").sketch("magnet pockets")
    for x, y in MAGNETS:
        s.circle(x, y, MAG_POCKET_D)
    extrude(s, s.profiles(), BACK, BACK - MAG_POCKET_DEPTH, CUT, body, "magnet pockets")

    # vents over the SCD41 (it sits 2.5 mm under the cover)
    s = Plane(c, "z", BACK, "back face (SCD41)").sketch("SCD41 cover vents")
    for y in COVER_VENT_Y:
        s.slot(sum(COVER_VENT_X) / 2, y, COVER_VENT_X[1] - COVER_VENT_X[0], VENT_W)
    extrude(s, s.profiles(), BACK, COVER_IN - 0.5, CUT, body, "SCD41 cover vents")

    # stand pocket, the heel's recess (its floor is the stop), hinge seats, closed-position detents
    s = Plane(c, "z", BACK, "back face (stand)").sketch("stand pocket")
    s.rrect(STAND_X0, STAND_POCKET_Y0, STAND_X1, STAND_POCKET_Y1, 0)
    extrude(s, s.profiles(), BACK, STAND_FLOOR_Z, CUT, body, "stand pocket")
    # the nail ramp: from the cover's edge down to STAND_NAIL_RAMP at the pocket's end wall
    xm = (STAND_X0 + STAND_X1) / 2
    s = Plane(c, "x", xm - STAND_NAIL_W / 2, "nail ramp").sketch("nail ramp")
    s.poly([(COVER_EDGE_Y0 - 0.01, BACK + 0.01), (STAND_POCKET_Y0 + 0.01, BACK + 0.01),
            (STAND_POCKET_Y0 + 0.01, BACK - STAND_NAIL_RAMP)])
    extrude(s, s.profiles(), xm - STAND_NAIL_W / 2, xm + STAND_NAIL_W / 2, CUT, body, "nail ramp")
    s = Plane(c, "z", STAND_FLOOR_Z, "heel recess").sketch("heel recess")
    s.rrect(STAND_X0, STAND_RECESS_Y0, STAND_X1, STAND_POCKET_Y1, 0)
    extrude(s, s.profiles(), STAND_FLOOR_Z, STAND_RECESS_Z, CUT, body, "heel recess")
    if STAND_HINGE == "pin":
        s = Plane(c, "x", PIN_X0, "pin blind end").sketch("stand pin")
        s.circle(STAND_HINGE_Y, STAND_AXIS_Z, STAND_PIN_HOLE)
        extrude(s, s.profiles(), PIN_X0, COVER_EDGE_X + 0.5, CUT, body, "stand pin")
    else:
        for xw, sgn in [(STAND_X0, -1), (STAND_X1, 1)]:
            s = Plane(c, "x", xw, "stub seat %.0f" % xw).sketch("stub seat %.0f" % xw)
            s.circle(STAND_HINGE_Y, STAND_AXIS_Z, STAND_SEAT_D)
            # teardrop: the cover prints outer face down (model -z up), a round hole lying on its side
            # sags at the top; a 45 deg roof pointing up prints true
            k = STAND_SEAT_D / 2 / math.sqrt(2)
            s.poly([(STAND_HINGE_Y - k, STAND_AXIS_Z - k), (STAND_HINGE_Y + k, STAND_AXIS_Z - k),
                    (STAND_HINGE_Y, STAND_AXIS_Z - STAND_SEAT_D / 2 * math.sqrt(2))])
            extrude(s, s.profiles(), xw, xw + sgn * STAND_STUB_HOLE_DEPTH, CUT, body, "stub seat %.0f" % xw)
    for xw, sgn in [(STAND_X0, 1), (STAND_X1, -1)]:
        for tag, d, deg, inter in (("foot", STAND_DET_FOOT, 0.0, STAND_DET_FOOT_IN),
                                   ("open", STAND_DET_OPEN, STAND_STOP, STAND_DET_OPEN_IN)):
            s = Plane(c, "x", xw, "stand wall %s %.0f" % (tag, xw)).sketch("%s detent %.0f" % (tag, xw))
            s.circle(*stand_world(d, deg), STAND_DET_BUMP_D)
            extrude(s, s.profiles(), xw, xw + sgn * (STAND_SIDE + inter), JOIN, body,
                    "%s detent %.0f" % (tag, xw))
    if STAND_HINGE == "snap":
        # ribs that fill the slits beside the tongues' heel ends once the flap is open
        for xa, xb in STAND_BLOCK_X:
            s = Plane(c, "x", xa, "block rib %.0f" % xa).sketch("block rib %.0f" % xa)
            s.rrect(STAND_RECESS_Y0, STAND_RECESS_Z - 0.2, STAND_POCKET_Y1 + 0.2, STAND_FLOOR_Z, 0)
            extrude(s, s.profiles(), xa, xb, JOIN, body, "block rib %.0f" % xa)

    # screw holes: counterbore + 90 deg countersink + clearance, revolved about each insert axis
    for k, (x, y) in enumerate(INSERTS):
        s = Plane(c, "y", y, "screw %d" % k).sketch("screw %d" % k)
        top = BACK + 0.5
        cs_top = BACK - CBORE_DEPTH
        cs_bot = cs_top - (CSINK_D - HOLE_D) / 2
        s.poly([(x, top), (x + CBORE_D / 2, top), (x + CBORE_D / 2, cs_top), (x + CSINK_D / 2, cs_top),
                (x + HOLE_D / 2, cs_bot), (x + HOLE_D / 2, boss_end - 0.5), (x, boss_end - 0.5)])
        axis = s.sk.sketchCurves.sketchLines.addByTwoPoints(s.pt(x, boss_end - 1.0), s.pt(x, top + 0.5))
        axis.isConstruction = True
        rv = c.features.revolveFeatures
        ri = rv.createInput(s.profiles(), axis, CUT)
        ri.setAngleExtent(False, adsk.core.ValueInput.createByString("360 deg"))
        ri.participantBodies = [body]
        rv.add(ri).name = "screw hole %d" % k
    return c, body


def build_v0_walls(c, body):
    """v0 (BOARD above): the shell's bosses under the board's holes and their pilots, SW2's stem hole slotted up to
    the rim, the microSD slot and its dish, the slide switch's notch and its I / O marks."""
    # bosses from the COG relief's floor up to the PCB's underside, where the board rests on them; the cover's
    # screws come through the board and cut their thread in the boss and the bezel, 0.6 of it left in front
    floor = LIP_IN - COG_RELIEF - 0.1
    s = Plane(c, "z", floor, "shell bosses").sketch("shell bosses")
    for x, y in INSERTS:
        s.circle(x, y, SHELL_BOSS_D)
    extrude(s, s.profiles(), floor, PCB_BOT, JOIN, body, "shell bosses")
    s = Plane(c, "z", PCB_BOT, "shell boss tops").sketch("screw pilots")
    for x, y in INSERTS:
        s.circle(x, y, FIT_FOR["TB_PILOT_D"])
    extrude(s, s.profiles(), PCB_BOT, PILOT_BOTTOM, CUT, body, "screw pilots")
    # SW2's stem: a round hole through the top wall, slotted up to the rim so the board goes in with it; the
    # cover's tongue fills the slot above the stem (build_rear)
    r = STEM_D / 2 + STEM_CLEAR
    s = Plane(c, "y", OUT_Y1 - WALL / 2, "top wall (stem)").sketch("stem slot")
    s.path((SW2_X - r, BACK + 1.0), [((SW2_X - r, SW2_Z),), ((SW2_X, SW2_Z - r), (SW2_X + r, SW2_Z)),
                                      ((SW2_X + r, BACK + 1.0),), ((SW2_X - r, BACK + 1.0),)])
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "stem slot")
    # the microSD slot, and the dish round it
    xr = OUT_X1 - WALL / 2
    s = Plane(c, "x", xr, "right wall (microSD)").sketch("microSD slot")
    s.rrect(SD_Y - SD_W / 2, SD_Z[0], SD_Y + SD_W / 2, SD_Z[1], 0.6)
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "microSD slot")
    s = Plane(c, "x", OUT_X1, "right wall outer (microSD dish)").sketch("microSD dish")
    s.slot(SD_Y, sum(SD_Z) / 2, SD_DISH[0], SD_DISH[1])
    extrude(s, s.profiles(), OUT_X1, OUT_X1 - SD_DISH[2], CUT, body, "microSD dish")
    # the slide switch: the old case's notch, open to the rim, and the IEC 60417 marks beside it, "I" (on)
    # towards the top, "O" (off) towards the bottom, level with the lever
    y0, y1 = SLIDE_Y
    s = Plane(c, "x", xr, "right wall (switch)").sketch("switch notch")
    s.rrect(y0, SLIDE_Z0, y1, BACK + 1.0, 0.5)
    extrude_sym(s, s.profiles(), 4.0, CUT, body, "switch notch")
    h, w, zm = SLIDE_TEXT_H, 0.5, SLIDE_Z0 + 1.0
    pl = Plane(c, "x", OUT_X1, "right wall outer (switch marks)")
    s = pl.sketch("switch mark I")
    s.rrect(y1 + 2.2 - h / 2, zm - w / 2, y1 + 2.2 + h / 2, zm + w / 2, w / 2 - 1e-3)   # "I" stands along y
    extrude(s, s.profiles(), OUT_X1, OUT_X1 - SLIDE_TEXT_D, CUT, body, "switch mark I")
    s = pl.sketch("switch mark O")
    s.circle(y0 - 2.2, zm, h)
    s.circle(y0 - 2.2, zm, h - 2 * w)
    extrude(s, s.profiles(lambda p: p.profileLoops.count == 2), OUT_X1, OUT_X1 - SLIDE_TEXT_D, CUT, body,
            "switch mark O")                                                     # the ring, not its middle


def build_tongue(c, body):
    """The wall tongue (BUTTON_STYLE above): slits, the pocket behind it, the nub, the dish, the print tie."""
    x0, te, s, rt = TONGUE_X0, TONGUE_END, TONGUE_SLIT, TONGUE_R
    z0, z1, yt = TONGUE_Z0, TONGUE_Z1, TONGUE_YT
    rc, q = rt + s, math.sqrt(0.5)
    ct, cb = (te - rt, z1 - rt), (te - rt, z0 + rt)          # centres of the free-end corners
    # the slits as one closed outline in (x, z): top slit out to the rounded end, down, bottom slit
    # back to the hinge, then back out along the tongue's own edge
    s1 = Plane(c, "y", OUT_Y1 + 0.1, "top wall outer (tongue)").sketch("tongue slits")
    s1.path((x0, z1), [
        ((x0 - s / 2, z1 + s / 2), (x0, z1 + s)),                                  # half round at the hinge
        ((te - rt, z1 + s),),
        ((ct[0] + rc * q, ct[1] + rc * q), (te + s, z1 - rt)),
        ((te + s, z0 + rt),),
        ((cb[0] + rc * q, cb[1] - rc * q), (te - rt, z0 - s)),
        ((x0, z0 - s),),
        ((x0 - s / 2, z0 - s / 2), (x0, z0)),                                      # half round at the hinge
        ((te - rt, z0),),
        ((cb[0] + rt * q, cb[1] - rt * q), (te, z0 + rt)),
        ((te, z1 - rt),),
        ((ct[0] + rt * q, ct[1] + rt * q), (te - rt, z1)),
        ((x0, z1),),
    ])
    extrude(s1, s1.profiles(), OUT_Y1 + 0.1, IN_Y1 - 0.1, CUT, body, "tongue slits")
    # the wall behind the tongue, rounded in plan where the tongue leaves the full wall
    r = TONGUE_ROOT_R
    s2 = Plane(c, "z", z0, "tongue underside").sketch("tongue pocket")
    yi = IN_Y1 - 0.1
    s2.path((x0, yi), [
        ((x0, yt - r),),
        ((x0 + r - r * q, yt - r + r * q), (x0 + r, yt)),
        ((te + s / 2, yt),),
        ((te + s / 2, yi),),
        ((x0, yi),),
    ])
    extrude(s2, s2.profiles(), z0, z1, CUT, body, "tongue pocket")
    # the nub: from the tongue's inner face over the PCB to the plunger; its underside rises at 45 deg
    # from the tongue (it prints) and is flat for the last TONGUE_NUB_FLAT
    yc, y1 = TONGUE_NUB_Y, TONGUE_NUB_Y + TONGUE_NUB_FLAT
    s3 = Plane(c, "x", SW2_X, "tongue nub").sketch("tongue nub")
    s3.poly([(yt + 0.1, TONGUE_NUB_Z), (yt, TONGUE_NUB_Z), (y1, TONGUE_NUB_TIP_Z), (yc, TONGUE_NUB_TIP_Z),
             (yc, TONGUE_NUB_TOP), (yt + 0.1, TONGUE_NUB_TOP)])
    extrude_sym(s3, s3.profiles(), TONGUE_NUB_W, JOIN, body, "tongue nub")
    # the dish: a spherical cap TONGUE_DISH_D across and TONGUE_DISH_DEPTH deep, over the nub
    d, h = TONGUE_DISH_D, TONGUE_DISH_DEPTH
    R = (d * d / 4 + h * h) / (2 * h)
    yo, zm = OUT_Y1 + R - h, (z0 + z1) / 2
    s4 = Plane(c, "x", SW2_X, "tongue dish").sketch("tongue dish")
    s4.path((yo - R, zm), [((yo, zm + R), (yo + R, zm)), ((yo - R, zm),)])
    axis = s4.sk.sketchCurves.sketchLines.addByTwoPoints(s4.pt(yo - R - 1, zm), s4.pt(yo + R + 1, zm))
    axis.isConstruction = True
    rv = c.features.revolveFeatures
    ri = rv.createInput(s4.profiles(), axis, CUT)
    ri.setAngleExtent(False, adsk.core.ValueInput.createByString("360 deg"))
    ri.participantBodies = [body]
    rv.add(ri).name = "tongue dish"
    # print tie: the tongue's first layer carried on across the end slit into the wall, set back from the
    # face out of sight; it makes that layer a bridge held at both ends, and snaps on the first press
    y1t = OUT_Y1 - TONGUE_TIE_BACK
    s5 = Plane(c, "z", z0, "tongue tie").sketch("tongue tie")
    s5.rrect(te - rt - 0.3, y1t - TONGUE_TIE_W, te + s + 0.6, y1t, 0)
    extrude(s5, s5.profiles(), z0, z0 + LAYER, JOIN, body, "tongue tie")


def build_pull(c, body):
    """The drawn-back whiskers (LATCH_STYLE "pull"): root block, bar, prongs, hood, print tie, clip boss and hole."""
    floor = LIP_IN - COG_RELIEF - 0.1          # just into the deepest floor of the FPC room
    q = 1 - math.sqrt(0.5)
    for xw, sg in RAIL_SIDES:
        tag = "%.0f" % xw
        xr, xt = pull_x(xw, sg, 0), pull_x(xw, sg, PULL_L)
        X = lambda k: xr - sg * k              # k from the root face towards the free end
        # root block: the board's bottom stop in Y, tied into the bottom wall
        xa0, xa1 = sorted((xr, xr + sg * RAIL_ANCHOR))
        s = Plane(c, "z", floor, "pull root " + tag).sketch("pull root " + tag)
        s.rrect(xa0, IN_Y0 - 0.5, xa1, PCB_Y0 - HOOK_GAP, 0)
        extrude(s, s.profiles(), floor, PULL_Z1, JOIN, body, "pull root " + tag)
        # the bar, rounded on its back where it leaves the block (the tension side is the face, whose step
        # to the block is only PULL_SET - HOOK_GAP)
        r = PULL_R_BACK
        s = Plane(c, "z", PULL_Z0, "pull bar " + tag).sketch("pull bar " + tag)
        s.path((xt, PULL_BACK), [
            ((X(r), PULL_BACK),),
            ((X(r * q), PULL_BACK - r * q), (xr, PULL_BACK - r)),
            ((xr + sg * 0.3, PULL_BACK - r),),
            ((xr + sg * 0.3, PULL_FACE),),
            ((xt, PULL_FACE),),
            ((xt, PULL_BACK),),
        ])
        extrude(s, s.profiles(), PULL_Z0, PULL_Z1, JOIN, body, "pull bar " + tag)
        # the prongs: flat underside PULL_ZC over the board, tip line following the drawn-back bar
        for u0, u1 in PULL_PRONGS:
            s = Plane(c, "z", PULL_ZC, "pull prong %s %.1f" % (tag, u0)).sketch("pull prong %s %.1f" % (tag, u0))
            s.poly([(pull_x(xw, sg, u0), PULL_FACE - 0.1), (pull_x(xw, sg, u1), PULL_FACE - 0.1),
                    (pull_x(xw, sg, u1), pull_tip(u1)), (pull_x(xw, sg, u0), pull_tip(u0))])
            extrude(s, s.profiles(), PULL_ZC, PULL_Z1, JOIN, body, "pull prong %s %.1f" % (tag, u0))
        # the hood over the bar's free end: rises 45 deg out of the side wall, flat only over the bar
        zh, e = PULL_Z1 + PULL_HOOD_GAP, PULL_END_GAP
        s = Plane(c, "y", IN_Y0 - 0.5, "pull hood " + tag).sketch("pull hood " + tag)
        s.poly([(xw - sg * 0.5, zh - e - 0.5), (xw, zh - e), (xt, zh), (xt + sg * PULL_HOOD_OVER, zh),
                (xt + sg * PULL_HOOD_OVER, zh + PULL_HOOD_T), (xw - sg * 0.5, zh + PULL_HOOD_T)])
        extrude(s, s.profiles(), IN_Y0 - 0.5, PCB_Y0 - 0.2, JOIN, body, "pull hood " + tag)
        # print tie: the bar's first layer carried on across the end gap into the side wall; it makes that
        # layer a bridge held at both ends, and snaps the first time the bar is drawn back. FDM only: resin
        # prints the bar free, and the tie would be a web to break for nothing
        if FDM:
            ty = (PULL_BACK + PULL_FACE) / 2
            x0t, x1t = sorted((xw - sg * 0.3, xt + sg * 0.5))
            s = Plane(c, "z", PULL_Z0, "pull tie " + tag).sketch("pull tie " + tag)
            s.rrect(x0t, ty - PULL_TIE_W / 2, x1t, ty + PULL_TIE_W / 2, 0)
            extrude(s, s.profiles(), PULL_Z0, PULL_Z0 + LAYER, JOIN, body, "pull tie " + tag)
        # the clip's boss and hole, under the fork
        xh = pull_x(xw, sg, PULL_A)
        s = Plane(c, "z", floor, "pull boss " + tag).sketch("pull boss " + tag)
        s.rrect(xh - 1.2, PULL_HOLE_Y - 1.2, xh + 1.2, PCB_Y0 - 0.10, 0.3)
        extrude(s, s.profiles(), floor, PULL_BOSS_TOP, JOIN, body, "pull boss " + tag)
        s = Plane(c, "z", PULL_BOSS_TOP + 0.1, "pull hole " + tag).sketch("pull hole " + tag)
        s.circle(xh, PULL_HOLE_Y, PULL_PIN_HOLE)
        extrude(s, s.profiles(), PULL_BOSS_TOP + 0.1, PULL_HOLE_BOTTOM, CUT, body, "pull hole " + tag)


def turner_profile(c0, u1, zt):
    """A light turner's outline across its slope (u along the light, z up): the 45 deg underside passes
    LED_HOLE_Z over the LED centre c0, and is cut flat where it would come nearer the LED than LED_REFL_CLEAR."""
    zf = LED_TOP + LED_REFL_CLEAR
    u0 = c0 - LED_REFL_BACK
    z0 = LED_HOLE_Z + (u0 - c0)
    pts = [(u0, zt)]
    if z0 < zf:
        pts += [(u0, zf), (c0 + zf - LED_HOLE_Z, zf)]
    else:
        pts += [(u0, z0)]
    return pts + [(u1, LED_HOLE_Z + (u1 - c0)), (u1, zt)]


def board_down():
    """How far the board can still go down from where it is drawn, onto its bottom stops."""
    return CLIP_GAP if LATCH_STYLE == "clip" else HOOK_GAP


def key_geometry():
    """The key drawn out on its slot floors, where laying the board in pushes it: its back face (BTN_GAP
    off the plunger of a board pushed up to the top wall), the slot floor its tabs rest on, and the tab
    outline (x0, x1, z0, z1)."""
    y_in = SW2_TIP_Y + FIT_TOP + BTN_GAP
    y_seat = IN_Y1 + BTN_SLOT_D
    assert y_seat - BTN_TAB_T >= y_in, "the key's back would stand behind its tabs"
    flange = (SW2_X - BTN_CAP_W / 2 - BTN_CLEAR - BTN_FLANGE, SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + BTN_FLANGE,
              BTN_FLANGE_Z0, 2 * BTN_CAP_Z - BTN_FLANGE_Z0)   # symmetric about the key's middle
    return y_in, y_seat, flange


def key_drop():
    """How far the key sinks from its slot floors onto the plunger of a board on its bottom stops."""
    return FIT_TOP + BTN_GAP + board_down()


def build_button(root, clear=None, name="SM Button", dx=0.0, marks=0):
    """The key: a pill standing proud of the top wall, with two end tabs that fill slots in the wall's
    inner face; drawn out on the slot floors. Print it standing on its back, tabs on the bed (model -y down).
    A trial key takes another hole clearance, stands dx along x and carries `marks` dots on its back."""
    c = new_component(root, name)
    y_in, y_seat, (x0, x1, z0, z1) = key_geometry()
    x0, x1, xm = x0 + dx, x1 + dx, SW2_X + dx
    clear = BTN_CLEAR if clear is None else clear
    cap_w, cap_h = BTN_CAP_W + 2 * (BTN_CLEAR - clear), BTN_CAP_H + 2 * (BTN_CLEAR - clear)   # the hole is fixed
    top = OUT_Y1 + BTN_OUT
    s = Plane(c, "y", y_in, "key tabs").sketch("key tabs")
    s.rrect(x0, z0, x1, z1, 0.8)
    body = extrude(s, s.profiles(), y_in, y_seat, NEW, name="key tabs").bodies.item(0)
    body.name = "Button"
    s = Plane(c, "y", y_in, "key body").sketch("key")
    s.slot(xm, BTN_CAP_Z, cap_w, cap_h)
    extrude(s, s.profiles(), y_in, top, JOIN, body, "key")
    fillet(c, loop_edges(planar_face(body, "y", top), True), 0.6, "key edge")
    chamfer(c, loop_edges(planar_face(body, "y", y_in), True), BTN_BACK_CHAMFER, "key back lead-in")
    if KEY_EAR_X:   # v1: steps across the back's top and bottom, clear of the switch's corner bosses (KEY_EAR_*)
        e = KEY_EAR_CLEAR
        xa, xb = min(a for a, _ in KEY_EAR_X) - e + dx, max(b for _, b in KEY_EAR_X) + e + dx
        zb, zt = BTN_CAP_Z - BTN_CAP_H / 2 - 0.5, BTN_CAP_Z + BTN_CAP_H / 2 + 0.5
        s = Plane(c, "y", y_in, "key ear reliefs").sketch("key ear reliefs")
        s.rrect(xa, zb, xb, KEY_EAR_Z[0] + e, 0)
        s.rrect(xa, KEY_EAR_Z[1] - e, xb, zt, 0)
        extrude(s, s.profiles(), y_in, y_in + KEY_EAR_DEPTH, CUT, body, "key ear reliefs")
    if marks:
        s = Plane(c, "y", y_in, "key marks").sketch("key marks")
        for i in range(marks):
            s.circle(xm + (i - (marks - 1) / 2) * KEY_MARK_PITCH, BTN_CAP_Z + KEY_MARK_DZ, KEY_MARK_D)
        extrude(s, s.profiles(), y_in, y_in + KEY_MARK_DEPTH, CUT, body, "key marks")
    if not BTN_CHANNEL or clear != BTN_CLEAR or dx:
        return c, body
    # U-channel round SW2's plunger: rails either side, a bar on the front side, open and flared towards the cover
    xi, xo = SW2_TIP[0] - BTN_RAIL_CLEAR, SW2_TIP[1] + BTN_RAIL_CLEAR
    w, f = BTN_RAIL_W, BTN_RAIL_FUNNEL
    zb, zt = BTN_CAP_Z - BTN_CAP_H / 2, BTN_CAP_Z + BTN_CAP_H / 2
    zbar = SW2_TIP[2] - BTN_BAR_GAP
    yr = y_in - BTN_RAIL_DEPTH
    s = Plane(c, "y", y_in, "key rails").sketch("key rails")
    s.poly([(xi - w, zb), (xo + w, zb), (xo + w, zt), (xo + f, zt), (xo, zt - f), (xo, zbar),
            (xi, zbar), (xi, zt - f), (xi - f, zt), (xi - w, zt)])
    extrude(s, s.profiles(), y_in, yr, JOIN, body, "key rails")
    tips = adsk.core.ObjectCollection.create()
    for e in body.edges:          # the rails' inner edges at their tips, along z
        a, b = e.startVertex.geometry, e.endVertex.geometry
        if (abs(a.y * 10 - yr) < 1e-3 and abs(b.y * 10 - yr) < 1e-3 and abs(a.x - b.x) < 1e-6
                and any(abs(a.x * 10 - x) < 1e-3 for x in (xi, xo))):
            tips.add(e)
    if tips.count != 2:
        raise RuntimeError("key rails: %d tip edges found, expected 2" % tips.count)
    chamfer(c, tips, BTN_RAIL_LEAD, "key rail lead-in")
    return c, body


def clip_tab_z():
    """z of the clip's tab (the part that goes into the wall slot)."""
    zm = COVER_IN - 0.5 - CLIP_ARM_H / 2
    return zm - CLIP_TAB_H / 2, zm + CLIP_TAB_H / 2


def clip_profile():
    """The clip in (y, z): tab in the wall, arm along the top, tapered finger down past the board
    edge, nose over the board's top face with a 45 deg lead-in. Extruded CLIP_W along x."""
    top = COVER_IN - 0.5                 # 0.5 under the cover plate
    za0 = top - CLIP_ARM_H
    tz0, tz1 = clip_tab_z()
    yw = IN_Y0 + 0.1                     # the arm ends 0.1 off the wall face; only the tab goes in
    yi = PCB_Y0 - CLIP_GAP
    ytip = PCB_Y0 + CLIP_OVER
    zu = PCB_TOP + CLIP_PLAY             # nose underside
    zn = zu + 0.35                       # nose tip height
    zs = zn + (ytip - yi)                # where the lead-in meets the finger
    g = 0.4                              # gusset where the finger leaves the arm
    return [(IN_Y0 - CLIP_TAB_IN, tz0), (IN_Y0 - CLIP_TAB_IN, tz1), (yw, tz1), (yw, top), (yi, top),
            (yi, zs), (ytip, zn), (ytip, zu), (yi - CLIP_T_NOSE, zu),
            (yi - CLIP_T_ROOT, za0 - g), (yi - CLIP_T_ROOT - g, za0), (yw, za0), (yw, tz0)]


def build_clips(root):
    """The two board clips as fitted, one at each bottom corner on the insert axes. They are the same
    part; export_print.py writes the first one, printed twice."""
    c = new_component(root, "SM Clips")
    bodies = []
    for i, x in enumerate(HOOK_X):
        s = Plane(c, "x", x, "clip %d" % (i + 1)).sketch("clip %d" % (i + 1))
        s.poly(clip_profile())
        b = extrude_sym(s, s.profiles(), CLIP_W, NEW, name="clip %d" % (i + 1)).bodies.item(0)
        b.name = "Clip %d" % (i + 1)
        bodies.append(b)
    return c, bodies


def build_turnbutton_seats(c, body):
    """Bosses, pilot holes and wall notches for the turnbuttons (LATCH_STYLE "screw")."""
    hw = TB_BOSS_W / 2
    s = Plane(c, "z", TB_FLOOR - 0.1, "turnbutton bosses").sketch("turnbutton bosses")
    for x in HOOK_X:              # from the FPC room's floor into the bottom wall, stopping short of the PCB edge
        s.rrect(x - hw, IN_Y0 - 0.2, x + hw, PCB_Y0 - TB_BOSS_GAP, 0.4)
    extrude(s, s.profiles(), TB_FLOOR - 0.1, TB_Z, JOIN, body, "turnbutton bosses")
    yf = IN_Y0 + 0.05
    for x in HOOK_X:              # the notch: the head, and the arm swung open along the wall (to -x)
        x0, x1 = x - TB_REACH - TB_NOTCH_CLEAR, x + TB_HUB_R + TB_NOTCH_CLEAR
        s = Plane(c, "x", (x0 + x1) / 2, "turnbutton notch %.0f" % x).sketch("turnbutton notch %.0f" % x)
        # the ceiling rises 45 deg towards the cavity: the case prints screen side down, so it prints unsupported
        s.poly([(yf, TB_Z), (TB_NOTCH_Y, TB_Z), (TB_NOTCH_Y, TB_NOTCH_TOP), (yf, TB_NOTCH_TOP + yf - TB_NOTCH_Y)])
        extrude_sym(s, s.profiles(), x1 - x0, CUT, body, "turnbutton notch %.0f" % x)
    s = Plane(c, "z", TB_Z, "turnbutton pilots").sketch("turnbutton pilots")
    for x in HOOK_X:
        s.circle(x, TB_YS, FIT_FOR["TB_PILOT_D"])
    extrude(s, s.profiles(), TB_Z, TB_Z - TB_PILOT_DEPTH, CUT, body, "turnbutton pilots")


def build_turnbuttons(root):
    """The two turnbuttons as fitted, closed (arm over the board), and stand-ins for their screws (head and
    the shank through the turnbutton) so the clearance checks see the heads. One part, printed twice,
    flat on its underside."""
    c = new_component(root, "SM Turnbuttons")
    r, a = TB_HUB_R, TB_ARM_W / 2
    yj = TB_YS + math.sqrt(r * r - a * a)        # where the arm's straight edges leave the hub
    yc = TB_ARM_TIP - a                          # centre of the arm's round end
    bodies = []
    for i, x in enumerate(HOOK_X):
        n = "turnbutton %d" % (i + 1)
        s = Plane(c, "z", TB_Z, n).sketch(n)
        s.path((x + a, yj), [((x + a, yc),), ((x, TB_ARM_TIP), (x - a, yc)), ((x - a, yj),),
                             ((x, TB_YS - r), (x + a, yj))])
        b = extrude(s, s.profiles(), TB_Z, TB_TOP, NEW, name=n).bodies.item(0)
        b.name = "Turnbutton %d" % (i + 1)
        s = Plane(c, "z", TB_TOP, n + " top").sketch(n + " hole")
        s.circle(x, TB_YS, FIT_FOR["TB_HOLE_D"])
        extrude(s, s.profiles(), TB_TOP, TB_Z, CUT, b, n + " hole")
        off, w, h = TB_RIB
        s = Plane(c, "z", TB_TOP, n + " rib").sketch(n + " rib")
        s.rrect(x - a + 0.4, PCB_Y0 + off, x + a - 0.4, PCB_Y0 + off + w, 0)
        extrude(s, s.profiles(), TB_TOP, TB_TOP + h, JOIN, b, n + " rib")
        bodies.append(b)
    cs = new_component(root, "SM Turnbutton screws (stand-in)")
    s = Plane(cs, "z", TB_TOP, "heads").sketch("heads")
    for x in HOOK_X:
        s.circle(x, TB_YS, TB_HEAD_D)
    extrude(s, s.profiles(), TB_TOP, TB_TOP + TB_HEAD_K, NEW, name="M2 heads")
    s = Plane(cs, "z", TB_Z, "shanks").sketch("shanks")
    for x in HOOK_X:
        s.circle(x, TB_YS, 2.0)
    extrude(s, s.profiles(), TB_Z, TB_TOP, NEW, name="M2 shanks")
    return c, bodies


def report_turnbuttons(front, rear, tb):
    """Swing the first turnbutton about its screw and say what it meets: nothing in the case all the way
    to open; a cover finger within a few degrees either way; and, left open, the cover itself."""
    tbm = adsk.fusion.TemporaryBRepManager.get()
    x = HOOK_X[0]
    box = tbm.createBox(adsk.core.OrientedBoundingBox3D.create(
        adsk.core.Point3D.create(cm(x), cm(TB_YS), cm(1.0)), adsk.core.Vector3D.create(1, 0, 0),
        adsk.core.Vector3D.create(0, 1, 0), cm(16), cm(16), cm(14)))
    near = {}
    for k, b in (("case", front), ("cover", rear)):
        t = tbm.copy(b)
        tbm.booleanOperation(t, box, adsk.fusion.BooleanTypes.IntersectionBooleanType)
        near[k] = t

    def hit(deg, what):
        t = tbm.copy(tb)
        m = adsk.core.Matrix3D.create()
        m.setToRotation(math.radians(deg), adsk.core.Vector3D.create(0, 0, 1),
                        adsk.core.Point3D.create(cm(x), cm(TB_YS), 0))
        tbm.transform(t, m)
        o = tbm.copy(near[what])
        tbm.booleanOperation(o, t, adsk.fusion.BooleanTypes.IntersectionBooleanType)
        if not o.faces.count:
            return 0.0, 0.0, t
        return o.volume * 1000, (o.boundingBox.maxPoint.z - o.boundingBox.minPoint.z) * 10, t

    case_hits = [d for d in range(0, 91, 5) if hit(d, "case")[0] > 1e-4]
    stop = {}
    for sg in (1, -1):                          # +: towards open (-x), -: past closed (+x)
        stop[sg] = next((d for d in range(1, 46) if hit(sg * d, "cover")[0] > 1e-4), None)
    vol, depth, t_open = hit(90, "cover")
    ymax = t_open.boundingBox.maxPoint.y * 10
    tip_at = lambda d: TB_YS + TB_REACH * math.cos(math.radians(d))
    worst = max(v for v in stop.values() if v) if all(stop.values()) else None
    print("turnbuttons: M2x%g cylinder head d%.1f; %.1f mm of thread in the boss, %.2f of floor under the "
          "pilot; boss wall to the PCB edge %.2f, wall behind the notch %.2f; arm %.1f over the PCB, %.2f "
          "above it; open, hub and head %.2f off the PCB edge (arm %.2f)%s"
          % (TB_SCREW_L, TB_HEAD_D, TB_SCREW_L - TB_T, (TB_Z - TB_PILOT_DEPTH) - FRONT,
             (PCB_Y0 - TB_BOSS_GAP) - (TB_YS + FIT_FOR["TB_PILOT_D"] / 2), TB_NOTCH_Y - OUT_Y0, TB_OVER,
             TB_Z - PCB_TOP, PCB_Y0 - (TB_YS + TB_HUB_R), PCB_Y0 - ymax,
             "" if TB_TOP - TB_SCREW_L > TB_Z - TB_PILOT_DEPTH + 0.1 else "  <-- WRONG: screw bottoms"))
    print("  swing in the case, closed to open: %s; cover on: stops at %s deg towards open, %s deg past "
          "closed (arm still %.2f over the PCB); left open, the cover hits it %.2f deep (%.2f mm3)%s"
          % ("free" if not case_hits else "HITS at %s deg  <-- WRONG" % case_hits, stop[1], stop[-1],
             (tip_at(worst) - PCB_Y0) if worst else float("nan"), depth, vol,
             "" if worst and tip_at(worst) - PCB_Y0 > 0.5 and depth > 0.3 else "  <-- WRONG"))


def build_magnet_support(root):
    """Print aid in each magnet pocket (MAG_SUPPORT_DX above), one body per pocket."""
    c = new_component(root, "PRN Magnet support")
    t, gap, side = POCKET_SUPPORT_T, POCKET_SUPPORT_GAP, POCKET_SUPPORT_SIDE
    r = MAG_POCKET_D / 2 - side
    zt = BACK - MAG_POCKET_DEPTH + gap
    bodies = []
    for i, (mx, my) in enumerate(MAGNETS):
        body = None
        for dx in MAG_SUPPORT_DX:
            xa = abs(dx) + t / 2
            h = math.sqrt(r * r - xa * xa)
            s = Plane(c, "x", mx + dx - t / 2, "magnet %d wall %.0f" % (i, dx)).sketch("magnet support")
            s.rrect(my - h, zt, my + h, BACK, 0)
            f = extrude(s, s.profiles(), mx + dx - t / 2, mx + dx + t / 2, JOIN if body else NEW, body)
            if body is None:
                body = f.bodies.item(0)
                body.name = "Magnet support %d" % (i + 1)
        lo, hi = mx + MAG_SUPPORT_DX[0] - t / 2, mx + MAG_SUPPORT_DX[-1] + t / 2
        s = Plane(c, "y", my - t / 2, "magnet %d tie" % i).sketch("magnet support tie")
        s.rrect(lo, zt, hi, BACK, 0)
        extrude(s, s.profiles(), my - t / 2, my + t / 2, JOIN, body, "magnet support tie")
        bodies.append(body)
    return c, bodies


def build_pin(root):
    """The hinge pin as fitted: 1.75 filament from the blind end to the cover edge."""
    c = new_component(root, "SM Stand pin")
    s = Plane(c, "x", PIN_X0, "pin start").sketch("pin")
    s.circle(STAND_HINGE_Y, STAND_AXIS_Z, 1.75)
    b = extrude(s, s.profiles(), PIN_X0, COVER_EDGE_X, NEW, name="pin").bodies.item(0)
    b.name = "Pin"
    return c, b


def build_stand(root):
    c = new_component(root, "SM Stand")
    x0, x1 = STAND_X0 + STAND_SIDE, STAND_X1 - STAND_SIDE
    s = Plane(c, "x", x0, "flap side").sketch("flap")
    r, ya, za = STAND_T / 2, STAND_HINGE_Y, STAND_AXIS_Z
    s.rrect(STAND_FOOT_Y, za - r, STAND_HEEL_END, za + r, 0)   # foot end down, heel end past the axis
    body = extrude(s, s.profiles(), x0, x1, NEW, name="flap").bodies.item(0)
    body.name = "Stand"
    if STAND_HINGE == "pin":
        s = Plane(c, "x", (x0 + x1) / 2, "flap axis").sketch("flap pin")
        s.circle(ya, za, STAND_FLAP_HOLE)
        extrude_sym(s, s.profiles(), x1 - x0 + 1.0, CUT, body, "flap pin")
    else:
        # slits through the flap free the outer strips into tongues: free at the heel end, rooted
        # STAND_TONGUE_L down the flap
        s = Plane(c, "z", za + r, "flap back (slits)").sketch("tongue slits")
        for xs in (x0 + STAND_TONGUE_W, x1 - STAND_TONGUE_W - STAND_SLIT):
            s.rrect(xs, STAND_HEEL_END - STAND_TONGUE_L, xs + STAND_SLIT, STAND_HEEL_END + 0.5, 0)
        extrude(s, s.profiles(), za + r, za - r, CUT, body, "tongue slits")
        stub_len = STAND_SIDE + STAND_STUB_IN
        for xw, sgn in [(x0, -1), (x1, 1)]:
            s = Plane(c, "x", xw, "stub %.0f" % xw).sketch("stub %.0f" % xw)
            s.circle(ya, za, STAND_STUB_D)
            extrude(s, s.profiles(), xw, xw + sgn * stub_len, JOIN, body, "stub %.0f" % xw)
            chamfer(c, loop_edges(planar_face(body, "x", xw + sgn * stub_len), True), 0.3, "stub tip %.0f" % xw)
    # a shallow dish on the heel says where to press
    xm = (x0 + x1) / 2
    hl, hw, dp = STAND_DISH
    s = Plane(c, "z", za + r, "heel face").sketch("heel dish")
    s.rrect(xm - hl, ya + STAND_HEEL / 2 - hw, xm + hl, ya + STAND_HEEL / 2 + hw, hw - 0.01)
    extrude(s, s.profiles(), za + r, za + r - dp, CUT, body, "heel dish")
    # the nail catch: a hollow under the foot's middle, roofed by the flap's outer skin (the lip)
    s = Plane(c, "z", za - r, "flap underside (nail)").sketch("nail hollow")
    s.rrect(xm - STAND_NAIL_W / 2, STAND_FOOT_Y - 1.5, xm + STAND_NAIL_W / 2, STAND_FOOT_Y + STAND_NAIL_HOLLOW, 1.0)
    extrude(s, s.profiles(), za - r, za + r - STAND_NAIL_LIP, CUT, body, "nail hollow")
    # the foot's closed detent: short tongues along its edges, free at the foot end
    s = Plane(c, "z", za + r, "flap back (foot slits)").sketch("foot slits")
    for xs in (x0 + STAND_FOOT_TONGUE_W, x1 - STAND_FOOT_TONGUE_W - STAND_FOOT_SLIT):
        s.rrect(xs, STAND_FOOT_Y - 0.5, xs + STAND_FOOT_SLIT, STAND_FOOT_Y + STAND_FOOT_TONGUE_L, 0)
    extrude(s, s.profiles(), za + r, za - r, CUT, body, "foot slits")
    # lead-in: the lower outer edge of each foot tongue cut at 45 deg, so the wall bump pushes the
    # tongue aside as the foot comes down instead of meeting a square corner
    k, e = STAND_FOOT_LEADIN, 0.01
    s = Plane(c, "y", STAND_FOOT_Y - 0.1, "foot lead-in").sketch("foot lead-in")
    s.poly([(x0 - e, za - r + k), (x0 - e, za - r - e), (x0 + k, za - r - e)])
    s.poly([(x1 + e, za - r + k), (x1 + e, za - r - e), (x1 - k, za - r - e)])
    extrude(s, s.profiles(), STAND_FOOT_Y - 0.1, STAND_FOOT_Y + STAND_FOOT_TONGUE_L - 1.0, CUT, body,
            "foot lead-in")
    for xw, sgn in [(x0, 1), (x1, -1)]:
        s = Plane(c, "x", xw, "flap side %.0f" % xw).sketch("flap dimples %.0f" % xw)
        s.circle(ya + STAND_DET_OPEN[0], za + STAND_DET_OPEN[1], STAND_DET_DIMPLE_D)
        s.circle(ya + STAND_DET_FOOT[0], za + STAND_DET_FOOT[1], STAND_DET_FOOT_DIMPLE_D)
        extrude(s, s.profiles(), xw, xw + sgn * STAND_DET_DIMPLE_DEPTH, CUT, body, "flap dimples %.0f" % xw)
    return c, body


# ---------------------------------------------------------------- print aids --
# The stand pocket's floor is a ceiling when the cover prints outer face down, and the slicer's own
# support filled the pocket as one solid block: it only came out by prying, and prying bent the thin
# rim at the pocket's foot end (owner, 2026-10-11). This is the support drawn instead: walls along the
# pocket, tied across, POCKET_SUPPORT_GAP short of every ceiling and POCKET_SUPPORT_SIDE off every wall,
# so nothing fuses to the cover and pliers get a wall to pull by. It goes into the cover's STL as a
# second shell (export_print.py), and a support blocker keeps the slicer's support out of the pocket.
POCKET_SUPPORT_T = 0.8                 # two 0.4 lines
POCKET_SUPPORT_GAP = 0.25              # two 0.12 layers of air under each ceiling
POCKET_SUPPORT_SIDE = 0.5
POCKET_SUPPORT_TIES = [11.0, 37.0, 62.9]
# The magnet pockets open to the outer face too, so their floors are 10.5 mm round ceilings in print,
# and without support they came out stringy (owner, 2026-10-11, who liked the drawn pocket support):
# the same kind of comb in each, four walls tied across the middle.
MAG_SUPPORT_DX = [-3.0, -1.0, 1.0, 3.0]


def pocket_support_walls():
    """x centres of the walls: one under each blocking rib (its underside is a ceiling too), the rest
    spread evenly between them, none closer than POCKET_SUPPORT_SIDE to a detent bump."""
    a = sum(STAND_BLOCK_X[0]) / 2
    b = sum(STAND_BLOCK_X[1]) / 2
    n = 6
    return [a + (b - a) * i / n for i in range(n + 1)]


def build_pocket_support(root):
    c = new_component(root, "PRN Pocket support")
    t, g = POCKET_SUPPORT_T, POCKET_SUPPORT_GAP
    ya, yb = STAND_POCKET_Y0 + POCKET_SUPPORT_SIDE, STAND_POCKET_Y1 - POCKET_SUPPORT_SIDE
    ys = STAND_RECESS_Y0 + POCKET_SUPPORT_SIDE
    xs = pocket_support_walls()
    body = None
    for i, xc in enumerate(xs):
        s = Plane(c, "x", xc - t / 2, "support wall %d" % i).sketch("support wall %d" % i)
        if i in (0, len(xs) - 1):             # under a rib: its underside is level with the pocket floor
            s.rrect(ya, STAND_FLOOR_Z + g, yb, BACK, 0)
        else:                                 # up into the heel recess
            s.poly([(ya, BACK), (yb, BACK), (yb, STAND_RECESS_Z + g), (ys, STAND_RECESS_Z + g),
                    (ys, STAND_FLOOR_Z + g), (ya, STAND_FLOOR_Z + g)])
        f = extrude(s, s.profiles(), xc - t / 2, xc + t / 2, JOIN if body else NEW, body)
        if body is None:
            body = f.bodies.item(0)
            body.name = "Pocket support"
    s = Plane(c, "z", BACK, "support ties").sketch("support ties")
    for y in POCKET_SUPPORT_TIES:
        s.rrect(xs[0] - t / 2, y - t / 2, xs[-1] + t / 2, y + t / 2, 0)
    extrude(s, s.profiles(), BACK, STAND_FLOOR_Z + g, JOIN, body, "support ties")
    return c, body


def _diag(r):
    """A point r from the centre of the outer corner arc at (OUT_X0, OUT_Y0), along its diagonal."""
    k = r / math.sqrt(2)
    return OUT_X0 + R_OUT - k, OUT_Y0 + R_OUT - k


def stand_tilt():
    """Screen tilt from vertical, foot and centre of mass behind the bottom-back edge, with the flap
    open to its stop. The flap hangs from its hinge and swings out of the back by STAND_STOP; the
    device stands on its bottom-back edge and on whichever foot corner reaches the desk first. The
    desk pushes the foot up, which turns the flap further open - onto the stop - only while the
    flap is open further than the screen leans (stop > tilt); otherwise it would fold shut."""
    b = math.radians(STAND_STOP)
    d = (-math.cos(b), math.sin(b))            # hinge -> foot
    n = (-math.sin(b), -math.cos(b))           # out of the face that lay on the pocket floor
    corners = [(STAND_HINGE_Y + STAND_L * d[0] + s * STAND_T / 2 * n[0],
                STAND_AXIS_Z + STAND_L * d[1] + s * STAND_T / 2 * n[1]) for s in (1, -1)]
    edge = (OUT_Y0, BACK)                       # the device pivots on its bottom-back edge
    com = (OUT_Y0 + (OUT_Y1 - OUT_Y0) / 2, (BACK + FRONT) / 2 + 1.5)   # the cell pulls it back a little
    best = None
    for k in range(900):
        th = math.radians(k / 10)
        W = lambda p: ((p[0] - edge[0]) * math.sin(th) + (p[1] - edge[1]) * math.cos(th),
                       (p[0] - edge[0]) * math.cos(th) - (p[1] - edge[1]) * math.sin(th))
        f = min((W(p) for p in corners), key=lambda w: w[1])
        if best is None or abs(f[1]) < best[0]:
            best = (abs(f[1]), k / 10, f[0], W(com)[0])
    return best[1:]


# ---------------------------------------------------------------- test coupons --
# Small pieces cut from the real parts, one fit each, so a fit is tried in 10-20 min of printing
# instead of the whole case (owner, 2026-10-10). They are cut from the parts this run built, so
# they always carry the current fits; the key and the flap are the real ones, printed once.
# Laid out COUPON_DX to the side of the case, in the case's own orientation.
COUPON_DX = 130.0
COUPONS = [  # name, cut from, box (x0, y0, z0, x1, y1, z1), parts that go with it
    ("CPN latch corner", "front", (OUT_X0 - 1, OUT_Y0 - 1, FRONT - 1,
                                   IN_X0 + (PULL_ROOT if LATCH_STYLE == "pull" else RAIL_SPAN[0]) + RAIL_ANCHOR + 1.5, 21.0,
                                   {"clip": COVER_IN - 0.3, "screw": TB_NOTCH_TOP + 2.0, "rail": RAIL_Z1 + 0.8,
                                    "pull": PULL_Z1 + PULL_HOOD_GAP + PULL_HOOD_T + 0.8}.get(LATCH_STYLE, 3.0)),
     ["clip", "turnbutton"]),
    ("CPN ledge corner", "front", (OUT_X0 - 1, 80.0, FRONT - 1, 21.0, OUT_Y1 + 1, 3.0), []),
    ("CPN key wall", "front", (24.0, 86.0, FRONT - 1, 44.0, OUT_Y1 + 1, 6.5), ["button"]) if BUTTON_STYLE == "key" else
    ("CPN key wall", "front", (TONGUE_X0 - 2.5, 86.0, FRONT - 1, 44.0, OUT_Y1 + 1,
                               TONGUE_Z1 + TONGUE_SLIT + 1.5), []),       # the whole tongue and its hinge
    ("CPN stand", "rear", (62.0, STAND_HINGE_Y - 12.0, STAND_RECESS_Z - STAND_FLOOR - 0.5,
                           92.0, STAND_POCKET_Y1 + STAND_WALL + 1.5, BACK + 1), ["stand", "pin"]),
    ("CPN magnet", "rear", (MAGNETS[1][0] - 8, MAGNETS[1][1] - 9, 4.0,
                            MAGNETS[1][0] + 8, MAGNETS[1][1] + 8, BACK + 1), []),
]
if V1 or V0:
    COUPONS = []   # v1 and v0 are printed whole: its fits are v2's, proven on the coupons and the prints of 2026-10-10..13
PLA_G_PER_CM3 = 1.24


def build_coupons(root, parts):
    tbm = adsk.fusion.TemporaryBRepManager.get()
    shift = adsk.core.Matrix3D.create()
    shift.translation = adsk.core.Vector3D.create(cm(COUPON_DX), 0, 0)
    made = []
    for name, src, (x0, y0, z0, x1, y1, z1), extras in COUPONS:
        c = new_component(root, name)
        tb = tbm.copy(parts[src])
        cut = tbm.createBox(adsk.core.OrientedBoundingBox3D.create(
            adsk.core.Point3D.create(cm((x0 + x1) / 2), cm((y0 + y1) / 2), cm((z0 + z1) / 2)),
            adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, 1, 0),
            cm(x1 - x0), cm(y1 - y0), cm(z1 - z0)))
        tbm.booleanOperation(tb, cut, adsk.fusion.BooleanTypes.IntersectionBooleanType)
        tbm.transform(tb, shift)
        body = c.bRepBodies.add(tb)
        body.name = name[4:]
        for e in extras:
            if parts.get(e) is None:
                continue
            te = tbm.copy(parts[e])
            tbm.transform(te, shift)
            c.bRepBodies.add(te).name = e
        made.append((name, body))
    return made


def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent
    for occ in [o for o in root.occurrences if o.component.name.startswith(("SM ", "CPN ", "PRN "))]:
        occ.deleteMe()
    # export_print.py names its folder by "print" (v1: print/v1_pla); check_interference.py reads the cover seat
    design.attributes.add("smartmeteo", "print", ("v1_" if V1 else "v0_" if V0 else "") + PRINT)
    design.attributes.add("smartmeteo", "cover_in", "%.3f" % COVER_IN)

    build_panel(root)
    build_cell(root)
    if not V0:                         # v0 has no inserts
        build_inserts(root)
    _, front = build_front(root)
    _, rear = build_rear(root)
    _, stand = build_stand(root)
    button = build_button(root)[1] if BUTTON_STYLE == "key" else None
    trials = [build_button(root, cl, "CPN key c%.2f" % cl, KEY_TRIAL_DX0 + KEY_TRIAL_STEP * i, i + 1)[1]
              for i, cl in enumerate(KEY_TRIALS)] if BUTTON_STYLE == "key" else []
    pin = build_pin(root)[1] if STAND_HINGE == "pin" else None
    # the cover's own printed supports are for FDM; resin is supported by the printer's own
    _, support = build_pocket_support(root) if FDM else (None, None)
    _, magsup = build_magnet_support(root) if FDM else (None, [None] * len(MAGNETS))
    clips = build_clips(root)[1] if LATCH_STYLE == "clip" else []
    turnbuttons = build_turnbuttons(root)[1] if LATCH_STYLE == "screw" else []
    coupons = build_coupons(root, {"front": front, "rear": rear, "button": button,
                                   "stand": stand, "pin": pin, "clip": clips[0] if clips else None,
                                   "turnbutton": turnbuttons[0] if turnbuttons else None})

    def bb(b):
        mn, mx = b.boundingBox.minPoint, b.boundingBox.maxPoint
        return "%.2f x %.2f x %.2f mm (z %.2f..%.2f)" % ((mx.x - mn.x) * 10, (mx.y - mn.y) * 10,
                                                         (mx.z - mn.z) * 10, mn.z * 10, mx.z * 10)
    print("fits for %s:" % PRINT.upper(), ", ".join("%s %g" % kv for kv in FIT_FOR.items()),
          "(v0: FIT_TOP %g)" % FIT_TOP if V0 else "")
    print("front:", bb(front), "| volume %.1f cm3" % front.volume)
    print("rear: ", bb(rear), "| volume %.1f cm3" % rear.volume)
    for name, body in coupons:   # one lump each: a loose sliver would print as a separate crumb
        print("coupon %-18s %s | %.1f g PLA | %d piece%s" % (
            name[4:] + ":", bb(body), body.volume * PLA_G_PER_CM3, body.lumps.count,
            "" if body.lumps.count == 1 else "s  <-- WRONG"))
    print("outer: %.2f x %.2f x %.2f mm" % (OUT_X1 - OUT_X0, OUT_Y1 - OUT_Y0, BACK - FRONT))
    print("z: front %.2f, lip %.2f, panel %.2f..%.2f, PCB %.2f..%.2f, cell top %.2f, cover %.2f..%.2f"
          % (FRONT, LIP_IN, PANEL_FRONT, PANEL_BACK, PCB_BOT, PCB_TOP, CELL_TOP, COVER_IN, BACK))
    probes = [  # (body, x, y, z, expected inside, what)
        (front, 53.3, 55.0, FRONT + 0.8, False, "window is open"),
        (front, 53.3, 20.0, FRONT + 0.5, True, "chin lip is solid"),
        (front, 53.3, 15.0, LIP_IN - 0.3, False, "COG relief is cut"),
        (front, OUT_X1 - 1, USB_Y, USB_Z, False, "USB opening"),
        (front, OUT_X1 - USB_RELIEF / 2, USB_Y + 5.5, USB_Z, False, "USB plug relief"),
        (front, IN_X1 + USB_GROOVE + 0.3, USB_Y + (USB_W / 2 + USB_CLEAR) + 0.2, USB_Z, True, "web hugs the receptacle"),
        (front, IN_X1 + 0.3, USB_Y, USB_Z + USB_H / 2 + USB_CLEAR - 0.1, False, "opening clears the shell top"),
        (front, OUT_X0 + 1, VENT_SCD_Y[2], sum(VENT_SCD_Z) / 2, False, "SCD41 vent"),
        *([(front, OUT_X1 - 1, VENT_BME_Y[1], sum(VENT_BME_Z) / 2, False, "BME690 vent")] if VENT_BME_Y else []),
        *([(front, IN_X1 - 0.2, IN_Y1 - 0.2, 0.0, False, "inner corner clears the PCB corner")] if not V0 else
          [(front, PCB_X1 - PCB_R + (PCB_R + 0.15) * 0.7071, PCB_Y1 - PCB_R + (PCB_R + 0.15) * 0.7071, 0.0, False,
            "cavity corner clears the board's round corner"),
           (front, PCB_X1 - PCB_R + (PCB_R + FIT + 0.15) * 0.7071, PCB_Y1 - PCB_R + (PCB_R + FIT + 0.15) * 0.7071,
            0.0, True, "cavity corner follows the board's, FIT off")]),
        *([(front, LEDGE_X[0], PCB_Y1 - 0.5, 0.8, True, "top ledge"),
           (front, LEDGE_X[0], PCB_Y1 - 0.5, PCB_TOP + 0.02, False, "ledge clears the PCB")] if LEDGE_X else
          [(front, INSERTS[2][0], PCB_Y1 - 0.5, 0.8, False, "no ledge over the top edge: the board goes in flat")]),
        (front, LATCH_X[0], IN_Y0 + 0.5, 0.0, False, "latch has room to flex"),
        (front, LED1_X, OUT_Y1 - 1, LED_HOLE_Z, False, "LED1 window"),
        (front, OUT_X1 - 1, CHRG_Y, LED_HOLE_Z, False, "CHRG window"),
        (front, OUT_X0 + 1, 50.0, 0.0, True, "side wall"),
        (rear, INSERTS[0][0] + BOSS_D / 2 - 0.3, INSERTS[0][1], 5.0, True, "boss"),
        (rear, INSERTS[0][0], INSERTS[0][1], 5.0, False, "screw clearance"),
        (rear, INSERTS[0][0] + 1.9, INSERTS[0][1], BACK - 0.5, False, "counterbore"),
        (rear, 40.0, 50.0, BACK - 1.0, True, "cover plate"),
        (rear, MAGNETS[0][0], MAGNETS[0][1], BACK - 1.5, False, "magnet pocket"),
        (rear, MAGNETS[1][0], MAGNETS[1][1], BACK - 1.5, False, "magnet pocket 2"),
        (rear, 15.0, VENT_SCD_Y[2], BACK - 1.0, False, "SCD41 cover vent"),
        (rear, 15.0, (VENT_SCD_Y[2] + VENT_SCD_Y[3]) / 2, BACK - 1.0, True, "rib between cover vents"),
        (rear, MAGNETS[0][0], MAGNETS[0][1], BACK - MAG_POCKET_DEPTH - 0.4, True, "magnet floor"),
        (rear, COVER_RIBS_X[0], IN_Y0 - REBATE + COVER_FIT - 0.05, COVER_IN + 0.7, True, "cover rib reaches the wall"),
        (rear, COVER_RIBS_X[0] + 1.0, IN_Y0 - REBATE + COVER_FIT - 0.05, COVER_IN + 0.7, False, "fit gap beside the rib"),
        (rear, IN_X1 + REBATE - COVER_FIT + 0.05, COVER_RIBS_Y[1], COVER_IN + 0.7, True, "side cover rib"),
        (front, COVER_RIBS_X[0], IN_Y0 - REBATE - 0.05, COVER_IN + 0.7, True, "rebate wall where the rib presses"),
        (rear, COVER_RIBS_X[0], IN_Y0 - REBATE - COVER_RIB_IN / 2, COVER_IN + 0.7, True, "rib overlaps the rebate wall"),
        (front, *_diag(R_OUT - 0.5), BACK - 1.0, True, "rim round the corner"),
        (front, *_diag(R_REBATE - COVER_FIT / 2), BACK - 1.0, False, "cover gap at the corner"),
        (rear, *_diag(R_REBATE - COVER_FIT / 2), BACK - 1.0, False, "cover clears the corner"),
        (rear, *_diag(R_REBATE - COVER_FIT - 0.3), BACK - 1.0, True, "cover fills its corner"),
        (rear, 77.0, 40.0, BACK - 1.0, False, "stand pocket"),
        (rear, 77.0, 40.0, STAND_FLOOR_Z - 0.6, True, "stand floor"),
        (rear, STAND_X0 - 0.6, STAND_HINGE_Y, STAND_AXIS_Z, False, "stand pin hole"),
        (rear, 77.0, STAND_HINGE_Y + 3.0, STAND_RECESS_Z + 0.3, False, "heel recess"),
        (rear, 77.0, STAND_HINGE_Y + 3.0, STAND_RECESS_Z - 0.4, True, "heel stop floor"),
        (rear, 77.0, STAND_HINGE_Y - 5.0, STAND_FLOOR_Z - 0.6, True, "floor before the recess"),
        (rear, 77.0, STAND_FOOT_Y + 1.0, STAND_FLOOR_Z - 0.6, True, "floor under the foot"),
        (rear, 77.0, STAND_POCKET_Y0 - 0.15, STAND_FLOOR_Z - 0.6, False, "no tub wall past the foot"),
        (rear, 70.0, STAND_POCKET_Y0 - 0.6, BACK - 0.5, True, "plate rim past the foot (beside the nail ramp)"),
        (rear, LED1_X, IN_Y1 - 0.5, 7.0, True, "LED1 turner"),
        (rear, LED1_X, LED1_Y + 1.0, LED_HOLE_Z, False, "LED1 light path"),
        (rear, LED1_X, LED1_Y, LED_HOLE_Z + 0.05, True, "LED1 turner's slope over the LED centre"),
        (rear, LED1_X, LED1_Y, LED_HOLE_Z - 0.05, False, "LED1 turner's slope ends there"),
        (rear, LED1_X, LED1_Y - 1.2, LED_TOP + LED_REFL_CLEAR + 0.05, True, "LED1 turner cut flat behind"),
        (rear, LED1_X, LED1_Y - 1.0, LED_TOP + LED_REFL_CLEAR - 0.05, False, "LED1 turner clears the LED"),
        (rear, CHRG_X, CHRG_Y, LED_HOLE_Z + 0.05, True, "CHRG turner's slope over the LED centre"),
        (rear, CHRG_X - 1.0, CHRG_Y, LED_TOP + LED_REFL_CLEAR - 0.05, False, "CHRG turner clears the LED"),
        (rear, IN_X1 - 0.5, CHRG_Y, 7.0, True, "CHRG turner"),
        (stand, 77.0, 40.0, STAND_AXIS_Z, True, "stand flap"),
        (stand, 70.0, STAND_FOOT_Y + 0.2, STAND_AXIS_Z, True, "flap reaches its foot end (beside the nail hollow)"),
        (stand, 77.0, STAND_HEEL_END - 0.3, STAND_AXIS_Z, True, "heel"),
        (stand, 77.0, STAND_FOOT_Y + 0.3, BACK - STAND_NAIL_LIP / 2, True, "nail lip: the flap's face is whole"),
        (stand, 77.0, STAND_FOOT_Y + 0.3, BACK - STAND_NAIL_LIP - 0.1, False, "nail hollow under the lip"),
        (stand, 77.0, STAND_FOOT_Y + STAND_NAIL_HOLLOW - 0.3, BACK - STAND_T + 0.1, False, "nail hollow's far end"),
        (stand, 77.0, STAND_FOOT_Y + STAND_NAIL_HOLLOW + 0.3, BACK - STAND_T + 0.1, True, "flap behind the hollow"),
        (stand, 77.0 + STAND_NAIL_W / 2 + 0.4, STAND_FOOT_Y + 0.3, BACK - STAND_T + 0.1, True,
         "foot end whole beside the hollow"),
        (rear, 77.0, COVER_EDGE_Y0 + 0.6, BACK - 0.3, False, "nail ramp in the cover strip"),
        (rear, 77.0, STAND_POCKET_Y0 - 0.1, BACK - STAND_NAIL_RAMP + 0.2, False, "ramp reaches below the lip"),
        (rear, 77.0, STAND_POCKET_Y0 - 0.1, BACK - STAND_NAIL_RAMP - 0.2, True, "cover left under the ramp"),
        (rear, 77.0 + STAND_NAIL_W / 2 + 0.4, COVER_EDGE_Y0 + 0.6, BACK - 0.1, True, "strip whole beside the ramp"),
        (stand, 77.0, STAND_HINGE_Y + STAND_HEEL / 2, STAND_AXIS_Z + STAND_T / 2 - 0.2, False, "dish on the heel"),
        (stand, STAND_X0 + STAND_SIDE + 0.1, STAND_HINGE_Y + STAND_DET_FOOT[0],
         STAND_AXIS_Z + STAND_DET_FOOT[1], False, "foot-detent dimple"),
        (stand, STAND_X0 + STAND_SIDE + STAND_FOOT_TONGUE_W + STAND_FOOT_SLIT / 2, STAND_FOOT_Y + 6.0,
         STAND_AXIS_Z, False, "foot slit"),
        (stand, STAND_X0 + STAND_SIDE + STAND_FOOT_TONGUE_W + STAND_FOOT_SLIT / 2, STAND_FOOT_Y + STAND_FOOT_TONGUE_L + 0.5,
         STAND_AXIS_Z, True, "foot tongue root"),
        (stand, STAND_X0 + STAND_SIDE + 0.05, STAND_FOOT_Y + 2.0, STAND_AXIS_Z - STAND_T / 2 + 0.05, False,
         "foot lead-in"),
        (stand, STAND_X1 - STAND_SIDE - 0.1, STAND_HINGE_Y + STAND_DET_OPEN[0],
         STAND_AXIS_Z + STAND_DET_OPEN[1], False, "open-detent dimple"),
        (rear, STAND_X0 + 0.2, *stand_world(STAND_DET_FOOT, 0.0), True, "foot-detent bump"),
        (rear, STAND_X1 - 0.2, *stand_world(STAND_DET_FOOT, 0.0), True, "foot-detent bump 2"),
        (rear, STAND_X1 - 0.2, *stand_world(STAND_DET_OPEN, STAND_STOP), True, "open-detent bump"),
        (rear, sum(STAND_BLOCK_X[0]) / 2, STAND_HINGE_Y + 3, (STAND_RECESS_Z + STAND_FLOOR_Z) / 2, True,
         "blocking rib"),
        (rear, sum(STAND_BLOCK_X[1]) / 2, STAND_HINGE_Y + 3, STAND_FLOOR_Z + 0.1, False,
         "rib stays below the closed flap"),
        (rear, STAND_BLOCK_X[0][0] - 0.2, STAND_HINGE_Y + 3, (STAND_RECESS_Z + STAND_FLOOR_Z) / 2, False,
         "room for the tongue beside the rib"),
        (support, pocket_support_walls()[3], 40.0, BACK - 0.5, True, "pocket support wall"),
        (support, pocket_support_walls()[3], 40.0, STAND_FLOOR_Z + 0.15, False, "support clears the pocket floor"),
        (support, pocket_support_walls()[3], STAND_HINGE_Y + 3, STAND_RECESS_Z + 0.5, True,
         "support reaches into the heel recess"),
        (support, pocket_support_walls()[3], STAND_HINGE_Y + 3, STAND_RECESS_Z + 0.15, False,
         "support clears the recess floor"),
        (support, pocket_support_walls()[0], STAND_HINGE_Y + 3, STAND_FLOOR_Z + 0.15, False,
         "support clears the rib"),
        (support, (pocket_support_walls()[0] + pocket_support_walls()[1]) / 2, 40.0, BACK - 0.5, False,
         "pliers fit between the support walls"),
        (support, 77.5, POCKET_SUPPORT_TIES[1], BACK - 0.5, True, "support tie"),
    ]
    x0_, te_, s_, z0_, z1_, yt_ = TONGUE_X0, TONGUE_END, TONGUE_SLIT, TONGUE_Z0, TONGUE_Z1, TONGUE_YT
    zm_, xm_, yo_ = (TONGUE_Z0 + TONGUE_Z1) / 2, (TONGUE_X0 + TONGUE_END) / 2, OUT_Y1 - 0.5
    if BUTTON_STYLE == "tongue":
        probes += [
            (front, xm_, yo_, zm_, True, "tongue"),
            (front, xm_, yt_ - 0.3, zm_, False, "wall cut away behind the tongue"),
            (front, xm_, yo_, z1_ + s_ / 2, False, "top slit"),
            (front, xm_, yo_, z0_ - s_ / 2, False, "bottom slit"),
            (front, xm_, IN_Y1 + 0.2, z1_ + s_ / 2, False, "top slit goes through the wall"),
            (front, xm_, yo_, z1_ + s_ + 0.15, True, "wall over the top slit"),
            (front, xm_, yo_, z0_ - s_ - 0.15, True, "wall under the bottom slit"),
            (front, te_ + s_ / 2, yo_, zm_, False, "end slit"),
            (front, te_ + s_ + 0.15, yo_, zm_, True, "wall past the end slit"),
            (front, te_ - 0.05, yo_, z0_ + 0.05, False, "tongue corner rounded"),
            (front, te_ + s_ - 0.05, yo_, z0_ - s_ + 0.05, True, "slit corner rounded"),
            (front, x0_ - 1.0, yo_, zm_, True, "hinge: the tongue runs into the wall"),
            (front, x0_ - 0.15, yo_, z1_ + s_ / 2, False, "half-round slit end"),
            (front, x0_ - s_ / 2 - 0.1, yo_, z1_ + s_ / 2, True, "slit stops at the hinge"),
            (front, x0_ + 0.1, yt_ - 0.1, zm_, True, "fillet at the hinge, inside"),
            (front, x0_ + TONGUE_ROOT_R + 0.3, yt_ - 0.1, zm_, False, "past the fillet"),
            (front, SW2_X, TONGUE_NUB_Y + 0.1, (SW2_TIP[2] + SW2_TIP[3]) / 2 + 0.2, True, "nub reaches the plunger"),
            (front, SW2_X, TONGUE_NUB_Y - 0.05, (SW2_TIP[2] + SW2_TIP[3]) / 2 + 0.2, False, "nub stops short of it"),
            (front, SW2_X, TONGUE_NUB_Y + 0.1, TONGUE_NUB_TIP_Z - 0.05, False, "nub's flat underside"),
            (front, SW2_X, PCB_Y1 + 0.3, TONGUE_NUB_Z + (TONGUE_YT - PCB_Y1 - 0.3) - 0.1, False,
             "nub's underside rises from the tongue"),
            (front, SW2_X, PCB_Y1 + 0.3, TONGUE_NUB_Z + (TONGUE_YT - PCB_Y1 - 0.3) + 0.1, True, "nub's 45 deg underside"),
            (front, SW2_X + TONGUE_NUB_W / 2 + 0.1, TONGUE_NUB_Y + 0.3, 1.5, False, "nub is TONGUE_NUB_W wide"),
            (front, SW2_X, OUT_Y1 - 0.15, zm_, False, "press dish"),
            (front, SW2_X, OUT_Y1 - TONGUE_DISH_DEPTH - 0.05, zm_, True, "dish depth"),
            (front, SW2_X + TONGUE_DISH_D / 2 + 0.2, OUT_Y1 - 0.05, zm_, True, "tongue face beside the dish"),
            (front, te_ + s_ / 2, OUT_Y1 - TONGUE_TIE_BACK - TONGUE_TIE_W / 2, z0_ + LAYER / 2, True,
             "print tie across the end slit"),
            (front, te_ + s_ / 2, OUT_Y1 - TONGUE_TIE_BACK - TONGUE_TIE_W / 2, z0_ + LAYER + 0.05, False,
             "tie is one layer"),
            (front, te_ + s_ / 2, OUT_Y1 - TONGUE_TIE_BACK + 0.1, z0_ + LAYER / 2, False, "tie set back from the face"),
            (front, te_ - TONGUE_R - 1.0, OUT_Y1 - 0.5, z0_ - s_ / 2, False, "nothing under the tongue"),
            (front, LED1_X - 2.0, yo_, zm_, True, "wall between the tongue and the LED"),
        ]
    elif BUTTON_STYLE == "key":
        key_y_in, key_y_seat, (kfx0, kfx1, kfz0, kfz1) = key_geometry()
        key_top = OUT_Y1 + BTN_OUT
        kxs = SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + BTN_FLANGE / 2   # middle of a tab, beyond the hole
        kzc = kfz1 + BTN_SEAT_CLEAR + 0.4                          # slot ceiling 0.4 in from its floor ...
        probes += [
        (front, SW2_X, OUT_Y1 - 1, BTN_CAP_Z, False, "button hole"),
        (front, SW2_X + BTN_CAP_W / 2 + 1.0, OUT_Y1 - 0.5, BTN_CAP_Z, True, "wall beside the button"),
        (button, SW2_X, OUT_Y1 - 1, BTN_CAP_Z, True, "key in the hole"),
        (button, SW2_X, key_top + 0.05, BTN_CAP_Z, False, "key top"),
        (button, SW2_X, key_top - 0.1, BTN_CAP_Z, True, "key reaches its top"),
        (button, kxs, key_y_seat - 0.5, BTN_CAP_Z, True, "key tab at its end"),
        (button, kxs, key_y_seat - 0.1, BTN_CAP_Z, True, "tab reaches the slot floor"),
        *([(button, kxs, IN_Y1 + 0.05, BTN_CAP_Z, True, "tab reaches the wall's inner face"),
           (button, kxs, IN_Y1 - 0.05, BTN_CAP_Z, True, "flange runs on past the wall's inner face")]
          if key_y_in < IN_Y1 else
          [(button, kxs, IN_Y1 + 0.05, BTN_CAP_Z, False, "tab stands back from the wall's inner face (v1)")]),
        (button, kxs, key_y_in + 0.05, BTN_CAP_Z, True, "flange reaches the key's back: one flat face to stand on"),
        (button, kxs, key_y_in - 0.05, BTN_CAP_Z, False, "flange stops at the key's back"),
        (button, SW2_X, key_y_seat - 0.5, BTN_CAP_Z + BTN_CAP_H / 2 + 0.3, False, "no tab above the key"),
        (button, kxs, max(key_y_seat - 0.5, key_y_in + BTN_BACK_CHAMFER + 0.1), kfz1 - 0.1, True,
         "end tab as tall above the middle as below"),   # past the back's lead-in on v1's thin tab
        (button, SW2_X, SW2_TIP_Y + FIT_TOP + 0.02, SW2_Z, False, "key clears the plunger of a board pushed up"),
        (button, SW2_X, key_y_in + 0.05, SW2_Z, True, "key's back at the plunger's height"),
        (button, SW2_X, key_y_in + 0.05, BTN_CAP_Z + BTN_CAP_H / 2 - 0.05, False, "lead-in chamfer round the key's back"),
        # the key's back reaches 0.11 over the board's top edge; at 0.15 in the hole its underside is 0.06 above
        # the PCB (the hole's floor, not the key, sets how far it can drop: z 0.00, same for every key size)
        (button, SW2_X, key_y_in + 0.4, PCB_TOP + 0.02, False, "key clears the PCB"),
        (front, kxs, IN_Y1 + 0.02, BTN_CAP_Z, False, "tab slot in the wall"),
        (front, kxs, key_y_seat + 0.2, BTN_CAP_Z, True, "wall behind the tab slot"),
        (front, kxs, key_y_seat - 0.05, BTN_CAP_Z, False, "slot is BTN_SLOT_D deep"),
        (front, kfx1 + BTN_SEAT_CLEAR + 0.2, IN_Y1 + 0.3, BTN_CAP_Z, True, "wall past the slot's end"),
        (front, kxs, key_y_seat - 0.4, kzc - 0.1, False, "slot ceiling rises 45 deg towards the inside"),
        (front, kxs, key_y_seat - 0.4, kzc + 0.1, True, "wall over the slot ceiling"),
        (front, kxs, IN_Y1 + 0.2, BTN_SEAT_Z0 - 0.05, True, "slot floor above the PCB"),
        (front, SW2_X, OUT_Y1 - 0.5, BTN_CAP_Z - BTN_CAP_H / 2 - BTN_CLEAR - 0.1, True, "wall under the key hole"),
        ]
        for i, (tk, cl) in enumerate(zip(trials, KEY_TRIALS)):
            txm = SW2_X + KEY_TRIAL_DX0 + KEY_TRIAL_STEP * i
            tw = BTN_CAP_W + 2 * (BTN_CLEAR - cl)
            tz = BTN_CAP_Z + KEY_MARK_DZ
            last = txm + i / 2 * KEY_MARK_PITCH
            probes += [
                (tk, txm + tw / 2 - 0.05, OUT_Y1 - 1, BTN_CAP_Z, True, "trial key %.2f is %.2f wide" % (cl, tw)),
                (tk, txm + tw / 2 + 0.05, OUT_Y1 - 1, BTN_CAP_Z, False, "trial key %.2f no wider" % cl),
                (tk, last, key_y_in + 0.1, tz, False, "trial key %.2f: dot %d" % (cl, i + 1)),
                (tk, last + KEY_MARK_PITCH, key_y_in + 0.1, tz, True, "trial key %.2f: no dot %d" % (cl, i + 2)),
            ]
        if BTN_CHANNEL:
            probes += [
            (button, SW2_X, key_y_in - 0.25, SW2_Z, False, "channel between the rails"),
            (button, SW2_TIP[0] - BTN_RAIL_CLEAR - BTN_RAIL_W / 2, key_y_in - 0.25, SW2_Z, True, "left rail"),
            (button, SW2_TIP[1] + BTN_RAIL_CLEAR + BTN_RAIL_W / 2, key_y_in - 0.25, SW2_Z, True, "right rail"),
            (button, SW2_TIP[0] - BTN_RAIL_CLEAR - 0.05, key_y_in - 0.25, BTN_CAP_Z + BTN_CAP_H / 2 - 0.1, False,
             "rails flare at the open end"),
            (button, SW2_TIP[0] - BTN_RAIL_CLEAR - 0.05, key_y_in - BTN_RAIL_DEPTH + 0.05, SW2_Z, False,
             "lead-in on the rail's tip"),
            (button, SW2_TIP[0] - BTN_RAIL_CLEAR - BTN_RAIL_W / 2, key_y_in - BTN_RAIL_DEPTH - 0.05, SW2_Z, False,
             "rail is BTN_RAIL_DEPTH deep"),
            (button, SW2_X, key_y_in - 0.25, SW2_TIP[2] - BTN_BAR_GAP - 0.05, True, "bar on the plunger's front side"),
            (button, SW2_X, key_y_in - 0.25, SW2_TIP[2] - BTN_BAR_GAP + 0.05, False, "bar clears the plunger"),
            (button, SW2_X, key_y_in - 0.25, SW2_TIP[3] + 0.3, False, "channel open towards the cover"),
            ]
        if BTN_WELL_D > 0:
            probes += [
                (button, SW2_X, OUT_Y1 - BTN_WELL_D + 0.2, BTN_CAP_Z, True, "key stands above the well floor"),
                (front, SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + 1.0, OUT_Y1 - BTN_WELL_D / 2, BTN_CAP_Z, False, "key well"),
                (front, SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + BTN_WELL_MARGIN + 0.4, OUT_Y1 - BTN_WELL_D / 2, BTN_CAP_Z,
                 True, "wall face past the well"),
            ]
        else:
            probes += [
                (front, SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + 0.3, OUT_Y1 - 0.05, BTN_CAP_Z, True,
                 "wall face flat right up to the key hole"),
                (front, SW2_X, OUT_Y1 - 0.05, BTN_CAP_Z + BTN_CAP_H / 2 + BTN_CLEAR + 0.3, True,
                 "wall face flat above the key hole"),
            ]
    else:   # "stem", v0
        r_ = STEM_D / 2 + STEM_CLEAR
        zsd_, ysl_ = sum(SD_Z) / 2, sum(SLIDE_Y) / 2
        probes += [
            (front, SW2_X, OUT_Y1 - 1, SW2_Z, False, "stem hole"),
            (front, SW2_X + r_ - 0.05, OUT_Y1 - 1, SW2_Z, False, "stem hole is STEM_CLEAR round the stem"),
            (front, SW2_X + r_ + 0.1, OUT_Y1 - 1, SW2_Z, True, "wall beside the stem hole"),
            (front, SW2_X, OUT_Y1 - 1, SW2_Z - r_ - 0.1, True, "wall under the stem hole"),
            (front, SW2_X, OUT_Y1 - 0.5, COVER_IN + 1.0, False, "stem slot open to the rim"),
            (rear, SW2_X, OUT_Y1 - 0.5, COVER_IN - 1.0, True, "cover tongue fills the stem slot"),
            (rear, SW2_X, OUT_Y1 - 0.5, SW2_Z + STEM_D / 2 + 0.1, False, "tongue clears the stem"),
            (rear, SW2_X, OUT_Y1 - 0.5, SW2_Z + r_ + 0.05, True, "tongue's end is the hole's upper half"),
            (rear, SW2_X + r_ - 0.05, OUT_Y1 - 0.5, COVER_IN - 1.0, False, "tongue clear of the slot's side"),
            (rear, SW2_X, IN_Y1 - 0.1, COVER_IN - 1.0, False, "tongue stays out of the cavity (the switch's body)"),
            (rear, SW2_X, OUT_Y1 - 0.05, BACK - 1.0, True, "tongue flush with the top wall"),
            (front, OUT_X1 - 1.5, SD_Y, zsd_, False, "microSD slot"),
            (front, IN_X1 + 0.3, SD_Y + SD_W / 2 - 0.3, zsd_, False, "microSD slot full width"),
            (front, IN_X1 + 0.3, SD_Y + SD_W / 2 + 0.3, zsd_, True, "wall beside the microSD slot"),
            (front, IN_X1 + 0.3, SD_Y, SD_Z[1] + 0.3, True, "wall over the microSD slot"),
            (front, IN_X1 + 0.3, SD_Y, SD_Z[0] - 0.1, True, "wall under the microSD slot"),
            (front, OUT_X1 - 0.6, SD_Y + SD_W / 2 + 0.6, zsd_ + 2.0, False, "microSD dish"),
            (front, OUT_X1 - SD_DISH[2] - 0.1, SD_Y + SD_W / 2 + 0.6, zsd_, True, "dish floor"),
            (front, OUT_X1 - 1, ysl_, SLIDE_Z0 + 1.0, False, "switch notch"),
            (front, OUT_X1 - 1, ysl_, SLIDE_Z0 - 0.2, True, "wall under the switch notch"),
            (front, OUT_X1 - 1, ysl_, COVER_IN + 1.0, False, "switch notch open to the rim"),
            (rear, OUT_X1 - 1, ysl_, COVER_IN - 1.0, True, "cover tongue fills the switch notch"),
            (rear, OUT_X1 - 1, ysl_, SLIDE_TOP - 0.1, False, "tongue ends SLIDE_TOP over the PCB"),
            (rear, OUT_X1 - 1, SLIDE_Y[1] - 0.05, COVER_IN - 1.0, False, "tongue clear of the notch's side"),
            (rear, IN_X1 - 0.1, ysl_, COVER_IN - 1.0, False, "switch tongue stays out of the cavity"),
            (rear, OUT_X1 - 0.05, ysl_, BACK - 1.0, True, "switch tongue flush with the right wall"),
            (front, OUT_X1 - 1, SLIDE_Y[1] + 0.3, SLIDE_Z0 + 1.0, True, "wall past the switch notch"),
            (front, OUT_X1 - 0.2, SLIDE_Y[1] + 2.2, SLIDE_Z0 + 1.0, False, "mark I (on) above the notch"),
            (front, OUT_X1 - 0.2, SLIDE_Y[0] - 2.2 + (SLIDE_TEXT_H - 0.5) / 2, SLIDE_Z0 + 1.0, False,
             "mark O (off) below it"),
            (front, OUT_X1 - 0.2, SLIDE_Y[0] - 2.2, SLIDE_Z0 + 1.0, True, "the O's middle stays"),
            (front, OUT_X1 - SLIDE_TEXT_D - 0.1, SLIDE_Y[1] + 2.2, SLIDE_Z0 + 1.0, True, "marks are SLIDE_TEXT_D deep"),
        ]
        for k, (x, y) in enumerate(INSERTS):
            probes += [
                (front, x + 1.5, y, PCB_BOT - 0.5, True, "shell boss %d under the board's hole" % k),
                (front, x + 1.5, y, PCB_BOT + 0.05, False, "shell boss %d ends at the PCB's underside" % k),
                (front, x, y, PCB_BOT - 1.0, False, "screw pilot %d" % k),
                (front, x, y, PILOT_BOTTOM - 0.2, True, "pilot %d is blind" % k),
                (rear, x + BOSS_D / 2 - 0.3, y, PCB_TOP + BOSS_GAP + 0.1, True, "cover boss %d reaches the PCB" % k),
                (rear, x + BOSS_D / 2 - 0.3, y, PCB_TOP + BOSS_GAP - 0.03, False, "cover boss %d stops BOSS_GAP short" % k),
            ]
    for i, (mx, my) in enumerate(MAGNETS):
        probes += [
            (magsup[i], mx + MAG_SUPPORT_DX[1], my + 2.0, BACK - 1.0, True, "magnet support wall"),
            (magsup[i], mx, my + 2.0, BACK - 1.0, False, "gap between the magnet support walls"),
            (magsup[i], mx, my, BACK - 1.0, True, "magnet support tie"),
            (magsup[i], mx + MAG_SUPPORT_DX[1], my, BACK - MAG_POCKET_DEPTH + 0.15, False,
             "magnet support clears the pocket floor"),
            (magsup[i], mx + MAG_SUPPORT_DX[1], my + MAG_POCKET_D / 2 - 0.3, BACK - 1.0, False,
             "magnet support clears the pocket wall"),
        ]
    if STAND_HINGE == "pin":
        probes += [
            (rear, PIN_X0 - 0.1, STAND_HINGE_Y, STAND_AXIS_Z, True, "pin hole blind on the left"),
            (rear, COVER_EDGE_X - 0.1, STAND_HINGE_Y, STAND_AXIS_Z, False, "pin hole open at the cover edge"),
            (rear, 94.0, STAND_HINGE_Y, STAND_AXIS_Z - 1.4, True, "rib under the pin"),
            (front, IN_X1 + 0.5, STAND_HINGE_Y, COVER_IN - 0.2, False, "shoulder notch under the pin end"),
            (front, IN_X1 + 0.5, STAND_HINGE_Y + 3.0, COVER_IN - 0.2, True, "shoulder beside the notch"),
            (pin, 77.0, STAND_HINGE_Y, STAND_AXIS_Z, True, "pin through the flap"),
            (stand, 77.0, STAND_HINGE_Y, STAND_AXIS_Z, False, "flap pin hole"),
        ]
    else:
        xl, xr = STAND_X0 + STAND_SIDE, STAND_X1 - STAND_SIDE
        probes += [
            (stand, xl - STAND_SIDE - 0.4, STAND_HINGE_Y, STAND_AXIS_Z, True, "left stub in its seat"),
            (stand, xr + STAND_SIDE + 0.4, STAND_HINGE_Y, STAND_AXIS_Z, True, "right stub in its seat"),
            (stand, STAND_X0 - STAND_STUB_IN + 0.1, STAND_HINGE_Y, STAND_AXIS_Z, True, "stub reaches 1.1 into the wall"),
            (rear, STAND_X0 - STAND_STUB_HOLE_DEPTH + 0.05, STAND_HINGE_Y, STAND_AXIS_Z, False, "seat deeper than the stub"),
            (rear, STAND_X0 - STAND_STUB_HOLE_DEPTH - 0.1, STAND_HINGE_Y, STAND_AXIS_Z, True, "seat is blind"),
            (stand, xl + STAND_TONGUE_W + STAND_SLIT / 2, STAND_HINGE_Y + 3, STAND_AXIS_Z, False, "left slit"),
            (stand, xr - STAND_TONGUE_W - STAND_SLIT / 2, STAND_HINGE_Y + 3, STAND_AXIS_Z, False, "right slit"),
            (stand, xl + STAND_TONGUE_W + STAND_SLIT / 2, STAND_HEEL_END - STAND_TONGUE_L - 1,
             STAND_AXIS_Z, True, "tongue root"),
            (stand, 77.0, STAND_HINGE_Y, STAND_AXIS_Z, True, "no through hole in the flap"),
        ]
    if LATCH_STYLE == "rail":
        yi_, zc_ = PCB_Y0 - HOOK_GAP, PCB_TOP + CATCH_PLAY
        for (xw, sg), span, d, tw, xt in zip(RAIL_SIDES, RAIL_SPAN, RAIL_DEPTH, RAIL_TOOTH_W, RAIL_TOOTH_X):
            probes += [
                (front, xt, yi_ - d / 2, RAIL_Z0 - 0.4, False, "rail is free underneath"),
                (front, xt, yi_ - d / 2, zc_, True, "rail bar"),
                (front, xt, yi_ - d / 2, RAIL_Z1 - 0.2, True, "rail bar rises above the tooth"),
                (front, xt, yi_ - d / 2, RAIL_Z1 + 0.2, False, "rail bar ends at its top"),
                (front, xt, yi_ - d + 0.1, PCB_TOP - 0.5, True, "rail bar is the board's stop in Y"),
                (front, xt, PCB_Y0 + 0.2, zc_ + 0.2, True, "rail tooth over the PCB"),
                (front, xt, PCB_Y0 + 0.2, zc_ - 0.1, False, "rail tooth clears the PCB top"),
                (front, xt + sg * (tw / 2 + 0.3), PCB_Y0 + 0.2, zc_ + 0.2, False, "rail tooth ends"),
                (front, xw + sg * (span + RAIL_ANCHOR / 2), (IN_Y0 + yi_) / 2, RAIL_Z1 - 0.3, True, "rail anchor"),
                (front, xw + sg * (span + RAIL_ANCHOR / 2), (IN_Y0 + yi_) / 2, RAIL_Z0 - 1.0, True,
                 "rail anchor stands on the floor"),
                (front, xt, yi_ - d - 0.3, zc_, False, "rail can bow outwards"),
                (front, xt, IN_Y0 + 0.3, zc_, False, "rail has room behind it"),
            ]
    elif LATCH_STYLE == "pull":
        ym_, zh_ = (PULL_BACK + PULL_FACE) / 2, PULL_Z1 + PULL_HOOD_GAP
        for xw, sg in RAIL_SIDES:
            xm_, xr_, xt_ = pull_x(xw, sg, PULL_L / 2), pull_x(xw, sg, 0), pull_x(xw, sg, PULL_L)
            xo_, xi_, xg_ = [pull_x(xw, sg, (a + b) / 2) for a, b in (PULL_PRONGS[1], PULL_PRONGS[0], PULL_FORK_U)]
            probes += [
                (front, xm_, ym_, 1.0, True, "pull bar"),
                (front, xm_, ym_, PULL_Z0 - 0.2, False, "pull bar is free underneath"),
                (front, xm_, PULL_FACE + 0.05, PCB_TOP - 0.5, False, "pull bar stands back from the board edge"),
                (front, xm_, PULL_BACK - 0.15, 1.0, False, "pull bar has room behind it"),
                (front, xr_ + sg * RAIL_ANCHOR / 2, PCB_Y0 - HOOK_GAP - 0.03, PCB_TOP - 0.5, True,
                 "pull root block is the board's stop in Y"),
                (front, xr_ + sg * RAIL_ANCHOR / 2, (IN_Y0 + PULL_FACE) / 2, PULL_Z0 - 1.5, True,
                 "pull root block stands on the floor"),
                (front, xw + sg * PULL_END_GAP / 2, ym_, 1.0, False, "pull bar end is free of the side wall"),
                (front, xw + sg * PULL_END_GAP / 2, ym_, PULL_Z0 + LAYER / 2, FDM,
                 "pull print tie" if FDM else "no print tie on resin"),
                (front, xo_, PCB_Y0 + 1.0, PULL_ZC + 0.2, True, "pull far prong over the PCB"),
                (front, xo_, PCB_Y0 + 1.0, PULL_ZC - 0.1, False, "pull far prong clears the PCB top"),
                (front, xi_, PCB_Y0 + 0.5, PULL_ZC + 0.2, True, "pull near prong over the PCB"),
                (front, xg_, PCB_Y0 + 0.5, PULL_ZC + 0.5, False, "pull fork is open"),
                (front, xt_ + sg * PULL_HOOD_OVER / 2, ym_, zh_ + 0.3, True, "pull hood"),
                (front, xt_ + sg * PULL_HOOD_OVER / 2, ym_, zh_ - 0.15, False, "pull hood clears the bar"),
                (front, xg_, PULL_HOLE_Y, PULL_BOSS_TOP - 0.5, False, "pull clip hole"),
                (front, xg_, PULL_HOLE_Y, PULL_HOLE_BOTTOM - 0.1, True, "pull clip hole is blind"),
                (front, xg_ + 1.0, PULL_HOLE_Y, PULL_BOSS_TOP - 0.3, True, "pull clip boss"),
                (front, xg_ + 1.0, PULL_HOLE_Y, PULL_BOSS_TOP + 0.2, False, "pull boss clears the bar"),
            ]
    elif LATCH_STYLE == "clip":
        tz0_, tz1_ = clip_tab_z()
        tm_, yi_, top_ = (tz0_ + tz1_) / 2, PCB_Y0 - CLIP_GAP, COVER_IN - 0.5
        for x, cl in zip(HOOK_X, clips):
            probes += [
                (cl, x, IN_Y0 - CLIP_TAB_IN / 2, tm_, True, "clip tab in the wall"),
                (front, x, IN_Y0 - CLIP_TAB_IN / 2, tm_, False, "slot for the clip tab"),
                (front, x, IN_Y0 - CLIP_TAB_IN - 0.3, tm_, True, "clip slot is blind"),
                (front, x, IN_Y0 - CLIP_TAB_IN / 2, tz1_ + 0.3, True, "wall over the clip slot"),
                (cl, x, (IN_Y0 + yi_) / 2, top_ - 0.3, True, "clip arm"),
                (cl, x, yi_ - 0.4, 3.5, True, "clip finger"),
                (cl, x, yi_ - CLIP_T_ROOT - 0.3, 3.5, False, "finger free on its outer side"),
                (cl, x, PCB_Y0 + 0.2, PCB_TOP + CLIP_PLAY + 0.15, True, "clip nose over the PCB"),
                (cl, x, PCB_Y0 + 0.2, PCB_TOP + CLIP_PLAY - 0.05, False, "nose clears the PCB top"),
                (cl, x, PCB_Y0 + 0.2, PCB_TOP + CLIP_PLAY + 0.35 + 0.4, False, "nose lead-in"),
                (cl, x + CLIP_W / 2 + 0.05, yi_ - 0.4, 3.5, False, "clip is CLIP_W wide"),
            ]
    elif LATCH_STYLE == "screw":
        yw_ = (TB_NOTCH_Y + IN_Y0) / 2
        yc_ = IN_Y0 - 0.2                                   # the 45 deg ceiling, near the cavity
        zc_ = TB_NOTCH_TOP + (yc_ - TB_NOTCH_Y)
        fx_, fy_ = sum(TB_FINGER_X) / 2, sum(TB_FINGER_Y) / 2
        for x, tb in zip(HOOK_X, turnbuttons):
            probes += [
                (front, x, TB_YS + 1.2, TB_Z - 0.5, True, "turnbutton boss"),
                (front, x + TB_BOSS_W / 2 + 0.2, TB_YS + 1.2, TB_Z - 0.5, False, "boss is TB_BOSS_W wide"),
                (front, x, PCB_Y0 - TB_BOSS_GAP / 2, TB_Z - 0.5, False, "boss stops short of the PCB edge"),
                (front, x, TB_YS, TB_Z - 2.0, False, "pilot hole"),
                (front, x, TB_YS, TB_Z - TB_PILOT_DEPTH - 0.3, True, "pilot hole is blind"),
                (front, x, IN_Y0 - CLIP_TAB_IN / 2, sum(clip_tab_z()) / 2, True, "no clip slot in the wall"),
                (front, x - TB_REACH + 0.3, yw_, TB_Z + 0.6, False, "notch takes the arm swung open"),
                (front, x - 1.0, yw_, TB_TOP + TB_HEAD_K - 0.2, False, "notch takes the screw head"),
                (front, x - 1.0, TB_NOTCH_Y - 0.3, TB_Z + 0.6, True, "wall behind the notch"),
                (front, x + TB_HUB_R + TB_NOTCH_CLEAR + 0.3, yw_, TB_Z + 0.6, True, "notch ends past the hub"),
                (front, x - 1.0, yc_, zc_ - 0.15, False, "notch ceiling rises 45 deg (under it)"),
                (front, x - 1.0, yc_, zc_ + 0.15, True, "notch ceiling rises 45 deg (over it)"),
                (front, x - 2.5, yw_, TB_Z - 0.2, True, "notch floor: the wall stays below it"),
                (tb, x, PCB_Y0 + 0.7, TB_Z + 0.6, True, "turnbutton arm over the PCB"),
                (tb, x, PCB_Y0 + 0.7, PCB_TOP + 0.03, False, "arm clears the PCB top"),
                (tb, x, TB_ARM_TIP - 0.15, TB_Z + 0.6, True, "arm reaches TB_OVER over the PCB"),
                (tb, x, TB_ARM_TIP + 0.15, TB_Z + 0.6, False, "arm ends there"),
                (tb, x, TB_YS, TB_Z + 0.6, False, "screw hole through the turnbutton"),
                (tb, x + TB_HUB_R - 0.2, TB_YS, TB_Z + 0.6, True, "hub round the hole"),
                (tb, x, PCB_Y0 + TB_RIB[0] + TB_RIB[1] / 2, TB_TOP + TB_RIB[2] / 2, True, "nail rib on the arm"),
                (rear, x - fx_, fy_, TB_FINGER_Z0 + 0.3, True, "cover finger (-x)"),
                (rear, x + fx_, fy_, TB_FINGER_Z0 + 0.3, True, "cover finger (+x)"),
                (rear, x - fx_, fy_, TB_FINGER_Z0 - 0.2, False, "finger ends over the strip"),
                (rear, x - fx_, IN_Y0 + 0.1, 3.0, False, "finger clears the wall"),
            ]
    elif LATCH_STYLE == "hook":
        yi_, zc_, fl_ = PCB_Y0 - HOOK_GAP, PCB_TOP + CATCH_PLAY, LIP_IN - COG_RELIEF
        zr_ = zc_ + 0.45 + HOOK_OVER + HOOK_GAP
        for x in HOOK_X:
            probes += [
                (front, x, yi_ - 0.2, (fl_ + zc_) / 2, True, "hook body"),
                (front, x, PCB_Y0 + 0.2, zc_ + 0.2, True, "hook tooth over the PCB"),
                (front, x, yi_ - HOOK_T_ROOT - 0.15, fl_ + 1.0, False, "hook free on its outer side"),
                (front, x, yi_ - 0.65, zr_ - 0.1, False, "hook tapers to its top"),
                (front, x, yi_ + 0.08, fl_ + 0.08, True, "hook root rounded (board side)"),
                (front, x + HOOK_W / 2 + 0.3, yi_ - 0.2, (fl_ + zc_) / 2, False, "hook free at its side"),
            ]
    if USB_GROOVE > 0:
        hw_ = USB_W / 2 + USB_CLEAR
        probes += [
            (front, IN_X1 + USB_GROOVE - 0.1, USB_Y, 8.0, False, "USB groove from the opening up to the rim"),
            (front, IN_X1 + USB_GROOVE + 0.1, USB_Y, 8.0, True, "wall behind the USB groove"),
            (front, IN_X1 + 0.3, USB_Y + hw_ + USB_LEAN - 0.1, 8.0, False, "USB groove leaves room for the lean"),
            (front, IN_X1 + 0.3, USB_Y + hw_ + USB_LEAN + 0.2, 8.0, True, "USB groove ends past the lean"),
            (front, IN_X1 + 0.3, USB_Y - hw_ - USB_SLIDE + 0.1, 8.0, False, "USB groove long enough for the slide"),
            (front, IN_X1 + 0.3, USB_Y - hw_ - USB_SLIDE - 0.2, 8.0, True, "USB groove ends past the slide"),
            (front, IN_X1 + USB_GROOVE + USB_WEB / 2, USB_Y, USB_Z + USB_H / 2 + USB_CLEAR + 0.3, True,
             "web over the opening: the groove stays inside the wall"),
            (front, IN_X1 + USB_GROOVE + USB_WEB / 2, USB_Y - hw_ - 0.3, USB_Z, True,
             "web beside the opening, over the groove's slide end"),
        ]
    if SW2_GROOVE > 0:
        probes += [
            (front, SW2_X, SW2_TIP_Y + FIT_TOP + 0.1, COVER_IN - 1.0, False, "switch recess above the key slots"),
            (front, SW2_X, IN_Y1 + SW2_GROOVE + 0.1, COVER_IN - 1.0, True, "wall behind the switch recess"),
            (front, SW2_BODY[1] + 0.5, IN_Y1 + 0.2, COVER_IN - 1.0, True, "switch recess no wider than the switch"),
            (front, 28.5, IN_Y1 + 0.3, 2.0, False, "switch recess below the key hole, for the lower bosses"),
            (front, 28.5, IN_Y1 + 0.3, SW2_BODY[2] - 0.5, True, "wall below the switch recess"),
        ]
    if BUTTON_STYLE == "key":
        for ex0, ex1 in KEY_EAR_X:
            xe = (ex0 + ex1) / 2
            probes += [
                (button, xe, key_y_in + 0.2, KEY_EAR_Z[0] + 0.05, False, "key's back stepped clear of a switch boss"),
                (button, xe, key_y_in + KEY_EAR_DEPTH + 0.1, KEY_EAR_Z[0] + 0.05, True, "key full behind the step"),
                (button, xe, key_y_in + 0.2, KEY_EAR_Z[1] - 0.05, False, "upper step"),
            ]
    # each coupon must still hold the fit it was cut for (none on v1: their probes drop out with a None body)
    class _Coupons(dict):
        def __missing__(self, key):
            return None
    cpn, D = _Coupons(coupons), COUPON_DX
    tooth_ = LATCH_STYLE in ("rail", "hook", "pull")
    probes += [
        (cpn["CPN latch corner"], LATCH_X[0] + D, PCB_Y0 + 0.2, PCB_TOP + 0.3, tooth_,
         "coupon: latch tooth" if tooth_ else "coupon: no tooth, a separate part holds"),
        (cpn["CPN ledge corner"], INSERTS[2][0] + D, PCB_Y1 - 0.5, PCB_TOP + 0.02, False, "coupon: ledge clears the PCB"),
        (cpn["CPN ledge corner"], INSERTS[2][0] + D, PCB_Y1 - 0.5, PCB_TOP + CATCH_PLAY + 0.3, True, "coupon: ledge over the PCB"),
        (cpn["CPN key wall"], LED1_X + D, OUT_Y1 - 1, LED_HOLE_Z, False, "coupon: LED window"),
        (cpn["CPN magnet"], MAGNETS[1][0] + D, MAGNETS[1][1], BACK - 1.5, False, "coupon: magnet pocket"),
        (cpn["CPN magnet"], MAGNETS[1][0] + D, MAGNETS[1][1], BACK - MAG_POCKET_DEPTH - 0.4, True, "coupon: magnet floor"),
    ]
    if BUTTON_STYLE == "tongue":
        probes += [
            (cpn["CPN key wall"], xm_ + D, yo_, zm_, True, "coupon: tongue"),
            (cpn["CPN key wall"], xm_ + D, yo_, z1_ + s_ / 2, False, "coupon: top slit"),
            (cpn["CPN key wall"], te_ + s_ / 2 + D, yo_, zm_, False, "coupon: end slit"),
            (cpn["CPN key wall"], x0_ - 1.0 + D, yo_, zm_, True, "coupon: hinge"),
            (cpn["CPN key wall"], SW2_X + D, TONGUE_NUB_Y + 0.1, 1.4, True, "coupon: nub"),
            (cpn["CPN key wall"], te_ + s_ / 2 + D, OUT_Y1 - TONGUE_TIE_BACK - TONGUE_TIE_W / 2, z0_ + LAYER / 2, True,
             "coupon: print tie"),
        ]
    else:
        probes += [
            (cpn["CPN key wall"], SW2_X + D, OUT_Y1 - 1, BTN_CAP_Z, False, "coupon: key hole"),
            (cpn["CPN key wall"], SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + 1.0 + D, OUT_Y1 - max(BTN_WELL_D, 0.1) / 2,
             BTN_CAP_Z, BTN_WELL_D == 0, "coupon: key well" if BTN_WELL_D > 0 else "coupon: wall face flat"),
            (cpn["CPN key wall"], SW2_X + BTN_CAP_W / 2 + BTN_CLEAR + BTN_FLANGE / 2 + D, IN_Y1 + 0.02, BTN_CAP_Z,
             False, "coupon: tab slot"),
        ]
    if tooth_:
        probes.append((cpn["CPN latch corner"], LATCH_X[0] + D, IN_Y0 + 0.5, 0.0, False,
                       "coupon: latch has room to flex"))
    if LATCH_STYLE == "screw":
        probes += [
            (cpn["CPN latch corner"], HOOK_X[0] + D, TB_YS + 1.2, TB_Z - 0.5, True, "coupon: turnbutton boss"),
            (cpn["CPN latch corner"], HOOK_X[0] + D, TB_YS, TB_Z - 2.0, False, "coupon: pilot hole"),
            (cpn["CPN latch corner"], HOOK_X[0] - 1.0 + D, (TB_NOTCH_Y + IN_Y0) / 2, TB_Z + 0.6, False,
             "coupon: wall notch"),
        ]
    if LATCH_STYLE == "clip":
        tm_ = sum(clip_tab_z()) / 2
        probes += [
            (cpn["CPN latch corner"], HOOK_X[0] + D, IN_Y0 - CLIP_TAB_IN / 2, tm_, False, "coupon: clip slot"),
            (cpn["CPN latch corner"], HOOK_X[0] + D, IN_Y0 - CLIP_TAB_IN - 0.3, tm_, True, "coupon: clip slot is blind"),
        ]
    if STAND_HINGE == "snap":
        probes += [
            (cpn["CPN stand"], STAND_X0 - STAND_STUB_HOLE_DEPTH + 0.05 + D, STAND_HINGE_Y, STAND_AXIS_Z, False,
             "coupon: stub seat"),
            (cpn["CPN stand"], STAND_X0 - STAND_STUB_HOLE_DEPTH - 0.1 + D, STAND_HINGE_Y, STAND_AXIS_Z, True,
             "coupon: seat is blind"),
        ]
    probes = [p for p in probes if p[0] is not None]   # the FDM print aids are not built for resin
    bad = 0
    for b, x, y, z, want, what in probes:
        pc = b.pointContainment(adsk.core.Point3D.create(cm(x), cm(y), cm(z)))
        inside = pc == adsk.fusion.PointContainment.PointInsidePointContainment
        if inside != want:
            bad += 1
            print("PROBE FAILED: %s at (%.2f, %.2f, %.2f) is %s" % (what, x, y, z, "solid" if inside else "empty"))
    print("probes: %d of %d as designed" % (len(probes) - bad, len(probes)))
    app.activeViewport.fit()
    tilt, foot, com = stand_tilt()
    print("stand open %.0f deg: screen %.1f deg off vertical, foot %.1f mm behind the edge, "
          "centre of mass %.1f mm behind it; the weight %s" % (
              STAND_STOP, tilt, foot, com,
              "pushes the flap onto its stop" if STAND_STOP > tilt + 3 else "FOLDS THE FLAP  <-- WRONG"))
    print("stand: hinge y %.2f, heel stop floor z %.2f (%.2f below the pocket floor), tub bottom z %.2f"
          % (STAND_HINGE_Y, STAND_RECESS_Z, STAND_FLOOR_Z - STAND_RECESS_Z, STAND_RECESS_Z - STAND_FLOOR))
    if BUTTON_STYLE == "tongue":   # cantilever from the hinge, pressed at the nub; it bends ALONG the layers
        a = SW2_X - TONGUE_X0
        h = TONGUE_Z1 - TONGUE_Z0
        k = 3 * E_MOD * (h * TONGUE_T ** 3 / 12) / a ** 3
        click = TONGUE_GAP + CLIP_GAP + SW2_TRAVEL           # the plunger pushes the board onto the clips first
        dmax = click + TONGUE_OVERTRAVEL
        # the board edge passes under the nub: at full press the nub point over it was dmax further out
        y_e = PCB_Y1 - CLIP_GAP + dmax
        z_e = TONGUE_NUB_Z + max(0.0, TONGUE_YT - y_e) if y_e > TONGUE_NUB_Y + TONGUE_NUB_FLAT else TONGUE_NUB_TIP_Z
        clear = z_e - PCB_TOP
        cover = (SW2_TIP[3] - max(TONGUE_NUB_TIP_Z, SW2_TIP[2])) / (SW2_TIP[3] - SW2_TIP[2])
        print("tongue: %.1f x %.2f mm, %.2f thick, slits %.2f; SW2 clicks after %.2f of travel at ~%.1f N (%.1f of it "
              "the switch); at %.2f it bends %.2f %% along the layers"
              % (TONGUE_L, h, TONGUE_T, TONGUE_SLIT, click, k * click + 2.55, 2.55, dmax,
                 1.5 * TONGUE_T * dmax / a ** 2 * 100))
        print("tongue nub: %.2f off the plunger, meets %.0f %% of its face (z %.2f..%.2f); fully pressed it passes "
              "%.2f over the PCB edge%s"
              % (TONGUE_GAP, cover * 100, max(TONGUE_NUB_TIP_Z, SW2_TIP[2]), SW2_TIP[3], clear,
                 "" if clear > 0.1 else "  <-- WRONG"))
        print("tongue edges: z %.2f / %.2f, on the 0.12 layers from the bed (%.3f / %.3f)"
              % (TONGUE_Z0, TONGUE_Z1, (TONGUE_Z0 - FRONT - LAYER0) / LAYER, (TONGUE_Z1 - FRONT - LAYER0) / LAYER))
    elif BUTTON_STYLE == "stem":
        r_ = STEM_D / 2 + STEM_CLEAR
        print("stem: hole %.2f round SW2's %.2f stem (axis %.2f over the PCB), slotted up to the rim; the cover's "
              "tongue fills the slot %.2f clear each side, its end the hole's upper half"
              % (2 * r_, STEM_D, SW2_Z - PCB_TOP, STEM_FILL_CLEAR))
        print("board: rests on four bosses %.1f under its holes (glass %.2f off the bottom ones, %.2f off the top "
              "ones); to find the screws it floats %.2f left, %.2f right (the USB groove's floor), %.2f up (SW2's "
              "body), and the screws pass its holes with %.2f; the cover's bosses press it %.2f"
              % (SHELL_BOSS_D, PANEL_Y0 - (INSERTS[0][1] + SHELL_BOSS_D / 2),
                 (INSERTS[2][1] - SHELL_BOSS_D / 2) - (PANEL_Y0 + PANEL_H), FIT,
                 IN_X1 + USB_GROOVE - USB_FACE, FIT_TOP - SW2_BODY_OUT, (2.9 - 2.0) / 2, -BOSS_GAP))
        sock_ = PCB_X0 + 89.1                  # the socket's open end, build_board_v0.py
        print("microSD: slot %.1f x %.1f at y %.1f (z %.2f..%.2f), dish %.1f deep; the socket's mouth is %.2f "
              "behind the dish floor" % (SD_W, SD_Z[1] - SD_Z[0], SD_Y, SD_Z[0], SD_Z[1], SD_DISH[2],
                                         (OUT_X1 - SD_DISH[2]) - sock_))
        print("slide switch: notch y %.2f..%.2f from z %.2f; the cover's tongue closes it from z %.2f, so the window "
              "is %.1f x %.1f; I above, O below" % (SLIDE_Y[0], SLIDE_Y[1], SLIDE_Z0, SLIDE_TOP,
                                                     SLIDE_Y[1] - SLIDE_Y[0], SLIDE_TOP - SLIDE_Z0))
        print("LED turners: slope at z %.2f over the LED centre, %.2f above the LED top; cut flat %.2f above it behind"
              % (LED_HOLE_Z, LED_HOLE_Z - LED_TOP, LED_REFL_CLEAR))
    else:
        y_in_, y_seat_, (fx0_, fx1_, fz0_, fz1_) = key_geometry()
        drop = key_drop()
        rest = BTN_OUT - drop
        click = rest - SW2_TRAVEL
        print("board in Y: %.2f at the top + %.2f at the bottom = %.2f; key %.2f off the plunger with the board up "
              "and the key out on its slot floors" % (FIT_TOP, board_down(), FIT_TOP + board_down(), BTN_GAP))
        print("key: out on its slot floors the top stands %.2f above the wall face, resting on the plunger (board on "
              "its stops) %.2f (loose by %.2f); SW2 clicks %s (owner's call: height first)"
              % (BTN_OUT, rest, drop, "level with the face" if abs(click) < 0.01 else
                 "%.2f %s the face" % (abs(click), "above" if click > 0 else "below")))
        print("key tabs: ends only, %.1f past the hole each side, %.2f thick, z %.2f..%.2f (key middle %.2f); slots "
              "%.2f deep with %.2f of wall behind, %.2f round the tabs, ceiling at 45 deg; key %.2f long, its back "
              "%.2f %s the wall's inner face; hole %.2f round the key, %.2f long"
              % (BTN_FLANGE, BTN_TAB_T, fz0_, fz1_, BTN_CAP_Z, BTN_SLOT_D, WALL - BTN_SLOT_D, BTN_SEAT_CLEAR,
                 OUT_Y1 + BTN_OUT - y_in_, abs(IN_Y1 - y_in_), "inside" if y_in_ < IN_Y1 else "behind",
                 BTN_CLEAR, WALL - BTN_SLOT_D))
        if KEY_TRIALS:
            print("trial keys (dots on the back): " + ", ".join("%d dot%s: %.2f round, %.2f x %.2f"
                  % (i + 1, "s" if i else "", cl, BTN_CAP_W + 2 * (BTN_CLEAR - cl), BTN_CAP_H + 2 * (BTN_CLEAR - cl))
                  for i, cl in enumerate(KEY_TRIALS)))
        if BTN_CHANNEL:
            print("key channel: %.2f wide for the %.2f plunger, %.2f deep, bar %.2f off its front side, open and flared "
              "%.2f towards the cover; pressed to SW2's full travel (~0.35) the rails' tips stand %.2f over the bumps "
              "on SW2's face"
              % (SW2_TIP[1] - SW2_TIP[0] + 2 * BTN_RAIL_CLEAR, SW2_TIP[1] - SW2_TIP[0], BTN_RAIL_DEPTH, BTN_BAR_GAP,
                 BTN_RAIL_FUNNEL, (SW2_TIP_Y - board_down()) - 0.35 - BTN_RAIL_DEPTH - 88.835))
        print("LED turners: slope at z %.2f over the LED centre, %.2f above the LED top; cut flat %.2f above it behind"
              % (LED_HOLE_Z, LED_HOLE_Z - LED_TOP, LED_REFL_CLEAR))
    if STAND_HINGE == "snap":   # tongue: cantilever from its root, squeezed at its free end
        def tongue(w, a, l, d):
            return 3 * d * l * w / (a * a * (3 * l - a)), w ** 3 / (a * a * (3 * l - a))
        a_now = STAND_TONGUE_L - STAND_HEEL
        e_now, k_now = tongue(STAND_TONGUE_W, a_now, STAND_TONGUE_L, STAND_STUB_IN + 0.1)
        _, k_v2 = tongue(1.2, 13.0, 18.0, 1.3)
        free = (STAND_STUB_IN + 0.1) * 2 * STAND_TONGUE_L ** 3 / (a_now ** 2 * (3 * STAND_TONGUE_L - a_now))
        print("tongues: %.1fx as stiff as v2 at the stub, %.2f %% at the root to snap in, free end moves %.2f "
              "of the %.2f slit%s; open, the ribs let a stub out by %.2f of its %.2f"
              % (k_now / k_v2, e_now * 100, free, STAND_SLIT, "" if free < STAND_SLIT - 0.1 else "  <-- WRONG",
                 STAND_BLOCK_CLEAR, STAND_STUB_IN))
    gap_ = (OUT_X1 - USB_RELIEF) - USB_FACE   # the relief's floor: where the plug's overmould stops
    print("USB: opening %.2f x %.2f, %s" % (USB_W + 2 * USB_CLEAR, USB_H + 2 * USB_CLEAR,
          "plug overmould stops %.2f mm from the receptacle face" % gap_ if gap_ > 0 else
          "the receptacle's face stands %.2f into the plug relief, so the overmould meets it" % -gap_))
    if USB_GROOVE > 0:
        print("USB groove: %.2f deep in the right wall's inner face (the receptacle stands %.2f past it), %.2f "
              "longer towards the bottom for the slide; %.2f of web left outside it, the plug relief %.2f deep"
              % (USB_GROOVE, USB_FACE - IN_X1, USB_SLIDE, USB_WEB, USB_RELIEF))
    if SW2_GROOVE > 0:
        print("switch recess: %.2f deep in the top wall's inner face (%.2f of wall behind), x %.2f..%.2f, z %.2f "
              "up to the rim; the cover's edge stands %.2f in"
              % (SW2_GROOVE, WALL - SW2_GROOVE, SW2_BODY[0] - 0.3, SW2_BODY[1] + 0.3, SW2_BODY[2] - 0.3,
                 REBATE - COVER_FIT))
    if LEDGE_X and LATCH_STYLE == "pull":
        # laid in: the top end at its place, the bottom end lifted till the panel's face clears the whiskers'
        # hoods, then the board slid up LEDGE_OVER under the ledges. The tilt leans the plunger out into the key
        lift_ = (PULL_Z1 + PULL_HOOD_GAP + PULL_HOOD_T + 0.2) - PANEL_FRONT
        tilt_ = math.asin(lift_ / PANEL_H)
        lean_ = (SW2_TIP[3] - PCB_TOP) * math.sin(tilt_)
        press_ = lean_ - BTN_GAP
        print("slide under the ledges: bottom end lifted %.1f (%.1f deg) to clear the whiskers' hoods; the plunger "
              "leans %.2f towards the key: %.2f free (BTN_GAP), %s"
              % (lift_, math.degrees(tilt_), lean_, BTN_GAP,
                 "it never touches the key out on its floors" if press_ <= 0 else
                 "then SW2 goes in %.2f (clicks at %.2f, %.2f full)%s"
                 % (press_, SW2_TRAVEL, 0.35, "" if press_ < 0.35 else "  <-- WRONG")))
    if LATCH_STYLE == "rail":   # bar held at both ends, tooth in the middle (RAIL_* above)
        def rail(span, d, h):
            strain = 12 * d * HOOK_OVER / span ** 2
            force = 16 * E_MOD * h * d ** 3 * HOOK_OVER / span ** 3
            a, b = max(d, h), min(d, h)             # torsion constant of the bar's rectangle
            j = a * b ** 3 / 3 * (1 - 0.63 * b / a + 0.052 * (b / a) ** 5)
            return strain, force, j / span          # twist under the tooth goes as span / J
        h = RAIL_Z1 - RAIL_Z0
        _, f_old, t_old = rail(13.0, 0.8, 1.8)     # the rail of 2026-10-11 (never printed: v3 tried the hook)
        for side, span, d in zip(("left (SCD41 side)", "right (USB side)"), RAIL_SPAN, RAIL_DEPTH):
            strain, force, tw = rail(span, d, h)
            print("latch rail %s: span %.1f, %.2f deep x %.2f tall; snapping in bends it %.2f %% ALONG the "
                  "extrusion lines, ~%.1f N (the 10-11 rail: %.1f N); %.1fx as stiff in twist"
                  % (side, span, d, h, strain * 100, force, f_old, tw / t_old))
        mid = (RAIL_Z0 + RAIL_Z1) / 2 - (PCB_TOP + CATCH_PLAY)
        print("latch rail: its middle %.2f %s the tooth's underside, so a screen press twists the tooth %s"
              % (abs(mid), "above" if mid > 0 else "below",
                 "further over the board" if mid > 0.2 else "OFF the board  <-- WRONG"))
    elif LATCH_STYLE == "pull":   # cantilever, drawn back by a clip at the fork (PULL_* above)
        ei = E_MOD * PULL_H * PULL_D ** 3 / 12
        da_max = PULL_ROOM / pull_defl(PULL_L, 1.0)          # fork travel when the free end meets the wall
        strain = lambda da: 1.5 * PULL_D * da / PULL_A ** 2
        force = lambda da: 3 * ei * da / PULL_A ** 3
        print("latch pull: cantilever %.1f long, %.2f deep x %.2f tall; held back by the clip (%.2f at the fork, "
              "%.2f at the free end) it bends %.2f %% ALONG the lines at ~%.1f N; pushed to the wall (%.2f at the "
              "fork) %.2f %%, ~%.1f N" % (PULL_L, PULL_D, PULL_H, PULL_PARK, pull_defl(PULL_L, PULL_PARK),
                                         strain(PULL_PARK) * 100, force(PULL_PARK), da_max,
                                         strain(da_max) * 100, force(da_max)))
        for name, (u0, u1) in zip(("near", "far"), PULL_PRONGS):
            o0, o1 = pull_tip(u0) - PCB_Y0, pull_tip(u1) - PCB_Y0
            print("latch pull: %s prong %.1f wide reaches %.2f..%.2f over the board; held back, %.2f off its edge"
                  % (name, u1 - u0, o0, o1, PCB_Y0 - (pull_tip(u0) - pull_defl(u0, PULL_PARK))))
        tip_max = max(pull_tip(u) for p_ in PULL_PRONGS for u in p_)
        flange_y = INSERTS[0][1] - INSERT_D / 2
        print("latch pull: prong tips reach y %.2f, %.2f short of the insert flanges (y %.2f)%s"
              % (tip_max, flange_y - tip_max, flange_y, "" if flange_y - tip_max > 0.3 else "  <-- WRONG"))
        spare = da_max - PULL_PARK
        print("latch pull: the clip drops into its hole once the fork is %.2f back; the wall stops it at %.2f, %.2f "
              "to spare%s; held back, the free end stands %.2f off the wall"
              % (PULL_PARK, da_max, spare, "" if spare > 0.15 else "  <-- TOO TIGHT", PULL_ROOM - pull_defl(PULL_L, PULL_PARK)))
        print("latch pull: the clip in its hole stands %.2f off the board edge (it comes out with the board in); "
              "fork %.1f for a %.1f clip; hole %.1f, %.2f deep"
              % (PCB_Y0 - (PULL_HOLE_Y + PULL_PIN_HOLE / 2), PULL_FORK, PULL_PIN_D, PULL_PIN_HOLE,
                 PULL_BOSS_TOP - PULL_HOLE_BOTTOM))
        kz = 3 * E_MOD * (PULL_D * PULL_H ** 3 / 12) / PULL_A ** 3
        print("latch pull: prong underside %.2f over the PCB top; a screen press lifts the bar ~%.2f mm per N at the "
              "fork until the hood stops its end at %.2f" % (PULL_ZC - PCB_TOP, 1 / kz, PULL_HOOD_GAP))
    elif LATCH_STYLE == "clip":   # printed lying on its side: the bend runs ALONG the layers
        zu = PCB_TOP + CLIP_PLAY
        z_root = COVER_IN - 0.5 - CLIP_ARM_H - 0.4      # where the gusset ends
        a = z_root - (zu + 0.35)                         # free root to the nose tip, where the PCB edge passes
        d = CLIP_OVER + CLIP_GAP
        n, num, ts = 400, 0.0, []
        for k in range(n):
            z = (k + 0.5) * a / n                        # from the root down
            t = CLIP_T_ROOT + (CLIP_T_NOSE - CLIP_T_ROOT) * z / a
            ts.append((z, t))
            num += (a - z) ** 2 / (E_MOD * CLIP_W * t ** 3 / 12) * a / n
        force = d / num
        peak = max(6 * force * (a - z) / (CLIP_W * t * t) / E_MOD for z, t in ts)
        ib = CLIP_W * CLIP_T_NOSE ** 3 / 12
        buckle = math.pi ** 2 * E_MOD * ib / (4 * a * a)
        print("latch clip: finger %.2f mm free, %.1f -> %.1f thick, nose %.2f over the PCB; snapping the board in "
              "bends it %.2f %% ALONG the layers (the hooks that cracked: 1.86 %% across), ~%.1f N each; a screen "
              "press loads it lengthwise, buckling > %.0f N each"
              % (a, CLIP_T_ROOT, CLIP_T_NOSE, CLIP_OVER, peak * 100, force, buckle))
    elif LATCH_STYLE == "hook":   # standing in Z, the bend runs ACROSS the layers: the number that decides if it lives
        a = PCB_TOP + CATCH_PLAY + 0.45 - (LIP_IN - COG_RELIEF)   # free root to where the board leaves it
        n, num, ts = 400, 0.0, []
        for k in range(n):
            z = (k + 0.5) * a / n
            t = HOOK_T_ROOT + (HOOK_T_TOP - HOOK_T_ROOT) * z / a
            ts.append((z, t))
            num += (a - z) ** 2 / (E_MOD * HOOK_W * t ** 3 / 12) * a / n
        force = HOOK_OVER / num
        peak = max(6 * force * (a - z) / (HOOK_W * t * t) / E_MOD for z, t in ts)
        print("latch hook: %.2f mm free, %.1f -> %.1f thick, r %.1f root, bends %.2f %% ACROSS the layers to "
              "snap in (the 10-10 hook that cracked: 1.86 %% and a sharp root), ~%.1f N each"
              % (a, HOOK_T_ROOT, HOOK_T_TOP, HOOK_ROOT_R, peak * 100, force))
    if LATCH_STYLE == "screw":
        report_turnbuttons(front, rear, turnbuttons[0])
    tip = BACK - CBORE_DEPTH - SCREW_L
    if V0:   # through the cover, the board's hole, the shell's boss, into the bezel
        print("M2x%g countersunk: tip at z %.2f, thread %.2f mm in the shell's boss and bezel, %.2f short of the "
              "pilot's end (%.2f of bezel left in front)%s"
              % (SCREW_L, tip, PCB_BOT - tip, tip - PILOT_BOTTOM, PILOT_BOTTOM - FRONT,
                 "" if tip - PILOT_BOTTOM >= 0.15 and PCB_BOT - tip >= 2.0 else "  <-- WRONG"))
    else:
        print("M2x%g countersunk: tip at z %.2f, thread engaged %.2f mm of the %.1f mm insert, %.2f mm above the PCB%s"
              % (SCREW_L, tip, INSERT_TOP - tip, INSERT_H, tip - PCB_TOP,
                 "" if tip - PCB_TOP >= 0.3 and INSERT_TOP - tip >= 2.0 else "  <-- WRONG"))
    print("stack: tape %.2f, PCB top %.2f above the glass face" % (TAPE_PANEL, PCB_TOP - PANEL_FRONT))
