// The two pictograms of the battery group, shared by the dashboard header (screen.cpp) and the Wi-Fi
// setup screen (portal_screen.cpp), so the corner looks the same on both and cannot drift apart.
// Pixel art: 'X' is ink. Plain data, no ESP-IDF — portal_screen.cpp builds on the host too.
#pragma once

// charging bolt (V1 Classic grid) drawn white with a 1px black halo, so it reads on the
// battery whether it lands on the filled or empty part.
static const char *const BOLT_V1[14] = {
  "......X...",".....XX...","....XX....","...XXX....","..XXXX....",
  "..XXXXXXXX",".XXXXXXXX.","XXXXXXXX..","...XXXXX..","...XXXX...",
  "...XXX....","...XX.....","...X......","...X......"};

// Mains plug (approved 'Plug' grid from render_states.py): two prongs over a body with a cord.
// Unlike the bolt this sits on plain white next to the %, so it needs no halo — plain black.
// Scaled up from the 7x8 mockup grid to 10x12: at this size 1px prongs and a 1px cord looked
// spindly next to the Org digits, so both are 2px. Redrawn by hand rather than scaled 1.5x,
// which would have left the pixel art uneven.
static const char *const PLUG_ICON[12] = {
  ".XX....XX.",".XX....XX.",".XX....XX.","XXXXXXXXXX","XXXXXXXXXX","XXXXXXXXXX",
  ".XXXXXXXX.","..XXXXXX..","...XXXX...","....XX....","....XX....","....XX...."};
