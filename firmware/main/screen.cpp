#include "screen.h"
#include "logo_small.h"
#include "org01.h"
#include "battery_icons.h"
#include "weather_icons.h"
#include "weather_icons_small.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

// --- mockup primitives (render_states.py Cv) mapped to EPaper. color 0=black,1=white ---
static inline int  O (EPaper& e,int x,int y,int sc,const char* t,uint8_t c=0){ return e.drawGFXString(x,y,t,&Org_01,c,sc); }
static inline int  Ow(EPaper& e,int sc,const char* t){ return e.gfxStrWidth(t,&Org_01,sc); }
static inline void Oc(EPaper& e,int cx,int y,int sc,const char* t,uint8_t c=0){ e.drawGFXString(cx-Ow(e,sc,t)/2,y,t,&Org_01,c,sc); }
static inline int  G (EPaper& e,int x,int y,int sc,const char* t,uint8_t c=0){ return e.drawString(x,y,t,c,sc); }
static inline int  Gw(int sc,const char* t){ return (int)strlen(t)*6*sc; }
static inline void Gc(EPaper& e,int cx,int y,int sc,const char* t,uint8_t c=0){ e.drawString(cx-Gw(sc,t)/2,y,t,c,sc); }
static inline void hln(EPaper& e,int x,int y,int w){ e.fillRect(x,y,w,1,0); }
static inline void vln(EPaper& e,int x,int y,int h){ e.fillRect(x,y,1,h,0); }
static inline void frm(EPaper& e,int x,int y,int w,int h){ e.drawRect(x,y,w,h,0); }
static inline void pxr(EPaper& e,int x,int y,int w,int h,uint8_t c=0){ e.fillRect(x,y,w,h,c); }
// degree mark: an Org 'o' one/two sizes down, raised (render_states odeg)
static inline void odegAt(EPaper& e,int xend,int nb,int snum,int ds){ if(ds<1)ds=1; O(e,xend+2,nb-4*(snum-ds)-3,ds,"o"); }
static inline void odeg(EPaper& e,int xend,int nb,int snum){ odegAt(e,xend,nb,snum,snum-3); }

// The bolt and the plug grids live in battery_icons.h: the setup screen draws the same corner.
static void drawBolt(EPaper& e,int cx,int cy){
    int gw=10,gh=14,x0=cx-gw/2,y0=cy-gh/2;
    for(int r=0;r<gh;r++) for(int c=0;c<gw;c++) if(BOLT_V1[r][c]=='X')
        for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) e.drawPixel(x0+c+dx,y0+r+dy,0);
    for(int r=0;r<gh;r++) for(int c=0;c<gw;c++) if(BOLT_V1[r][c]=='X') e.drawPixel(x0+c,y0+r,1);
}
static void drawPlug(EPaper& e,int x0,int y0){
    for(int r=0;r<12;r++) for(int c=0;c<10;c++) if(PLUG_ICON[r][c]=='X') e.drawPixel(x0+c,y0+r,0);
}
static const char* ARR_UP[8]={"....XXXX",".....XXX","....XXXX","...XX..X","..XX....",".XX.....","XX......","X......."};
static const char* ARR_DN[8]={"X.......","XX......",".XX.....","..XX....","...XX..X","....XXXX",".....XXX","....XXXX"};
static const char* ARR_RT[6]={"....X...",".....XX.","XXXXXXXX","XXXXXXXX",".....XX.","....X..."};
static void drawTrendArrow(EPaper& e,int X,int Y,int tr,uint8_t col){
    const char** g; int rows;
    if(tr>0){ g=ARR_UP; rows=8; } else if(tr<0){ g=ARR_DN; rows=8; } else { g=ARR_RT; rows=6; }
    for(int r=0;r<rows;r++) for(int c=0;c<8;c++) if(g[r][c]=='X') e.drawPixel(X+c,Y+r,col);
}

// The outlook under the graph (render_forecast_band.py, variant G, chosen by the owner 2026-09-13): three
// days, each name white in a black band, and under it an icon with the high over the low. Every cell is laid
// out the same (owner, 2026-09-14): the icon and a column of numbers as one group, centred in the cell. The
// column is as wide as the widest number in the row and the numbers stand right-aligned in it, the low under
// the high with their right edges level — so a narrow "21" no longer leaves its cell emptier on the right than
// a "30" does. The icon is 32 px, as tall as a size-3 high over a size-2 low; at 36 px it filled the space
// under the band and outweighed the numbers (owner, 2026-09-14). No degree marks in this row. The sizes of the
// high and the low are chosen once for the row, the largest the column can take. The forecast still holds
// four days; the fourth is not drawn.
static const int FC_Y = 250;                    // the band's top; the outdoor strip's rule shares it
static const int FC_BAND_H = 11;
static void drawForecastRow(EPaper& e, const ScreenData& d){
    const int n = 3, top = FC_Y + FC_BAND_H;
    const int CELL = 82, GAP = 3;               // a cell's inside, between its dividers; icon to numbers
    const char* hi[n]; const char* lo[n];
    for(int i=0;i<n;i++){ hi[i] = d.fcHi[i] ? d.fcHi[i] : "--"; lo[i] = d.fcLo[i] ? d.fcLo[i] : ""; }
    auto vis = [&](int s, const char* t){ return Ow(e,s,t) - s; };      // drawn width, less the trailing gap
    static const struct { int hs, ls; } CAND[] = { {3,2}, {3,1}, {2,2}, {2,1} };
    int hs = 2, ls = 1, colw = 0;
    for(const auto& c : CAND){
        int w = 0;
        for(int i=0;i<n;i++){
            const int a = vis(c.hs,hi[i]); if(a > w) w = a;
            if(lo[i][0]){ const int b = vis(c.ls,lo[i]); if(b > w) w = b; }
        }
        hs = c.hs; ls = c.ls; colw = w;
        if(WXS_ICON_W + GAP + w <= CELL - 4) break;           // at least 2 px clear of each divider
    }
    const int group = WXS_ICON_W + GAP + colw;
    const int iy = top + 3;                     // the icon's top level with a size-3 high (baseline top+15)

    pxr(e,0,FC_Y,250,FC_BAND_H,0);
    for(int i=0;i<n;i++){
        const int x0 = i*83, gx = x0 + 1 + (CELL - group)/2;
        if(i>0){ pxr(e,x0,FC_Y,1,FC_BAND_H,1); vln(e,x0,top+3,298-top-4); }
        if(d.fcDay[i]) Gc(e,x0+1+CELL/2,FC_Y+2,1,d.fcDay[i],1);
        if(d.fcIcon[i] && d.fcIcon[i][0])
            e.drawBitmap(gx,iy,weatherIconSmall(d.fcIcon[i]),WXS_ICON_W,WXS_ICON_H,0);
        const int cl = gx + WXS_ICON_W + GAP, cr = cl + colw - 1;
        const int xh = cr - vis(hs,hi[i]) + 1;
        O(e,xh,top+15,hs,hi[i]);
        if(lo[i][0]){
            // Under the high, right edges level; kept inside the column when the low is the wider number.
            int xl = xh + vis(hs,hi[i]) - vis(ls,lo[i]);
            if(xl < cl) xl = cl;
            if(xl > cr - vis(ls,lo[i]) + 1) xl = cr - vis(ls,lo[i]) + 1;
            O(e,xl,top+32,ls,lo[i]);
        }
    }
}

// The console's address band, at the bottom of BOTH dashboard layouts — online and offline draw the
// same thing, and for a while they drew it from two copies of these lines. One copy is the point:
// the last change to it (the password's size) had to be made twice, and a test only caught that
// because a state was added for the second copy.
//
// Two rows while the console is open: where to point a browser, and the password to get in. The
// password has to be HERE because this is the only screen the owner sees on that path — on the
// no-network path the QR screen prints it, but a device on the home network never showed it at all,
// so a console that asks for a password would have locked the owner out.
//
// The password is drawn THREE TIMES the size of the address, and that is the point: the owner could
// not read it on the glass (hardware, 2026-09-19) and so never got as far as the console. Twelve
// random characters have to be transcribed into a phone; at size 1 a glyph is 5x7 px, about
// 1 x 1.5 mm on this panel. At size 3 it is 15x21 px and the twelve of them come to 216 px of the
// band's 249 — the largest that fits. The word "password" cannot fit beside them at any size above
// 1, so it is not drawn: the row above is plainly an address, the console's sign-in box says the
// password is on the device's screen, and a label nobody can read would help nobody.
//
// One row when there is no password to show: NVS would not keep one, so it lives only for this boot.
static void drawSetupBand(EPaper& epd, const ScreenData& d) {
    const int bh = d.setupLine2 ? 36 : 16;
    // От самого края: плашка стояла на x=1 и кончалась на y=298, потому что снаружи её обводила
    // внешняя рамка. Рамки больше нет (2026-09-30), и этот отступ остался бы белым волоском вдоль
    // корпуса. Правый край на 250 не трогаем — он совпадает с разделителем блока графика.
    pxr(epd,0,300-bh,250,bh);
    if (d.setupLine2) {
        Gc(epd,125,266,1,d.setupLine,1);
        Gc(epd,125,276,3,d.setupLine2,1);
    } else {
        Gc(epd,125,288,1,d.setupLine,1);
    }
}

void drawMainScreen(EPaper& epd, const ScreenData& d) {
    epd.clear(0xFF);
    // Внешней рамки нет намеренно (решение пользователя 2026-09-30): край экрана теперь обозначает
    // корпус, и нарисованная линия в миллиметре от него читается как вторая рамка, а не как край.

    // ===== HEADER =====
    O(epd,8,19,2,d.date);
    int clk_right = 214;   // right edge of whatever occupies the clock slot; the moon's by default
    if (d.night) { epd.fillCircle(200,15,9,0); epd.fillCircle(205,12,8,1); }  // crescent moon
    else if (d.updClock) {
        // When the screen only repaints on a measurement, live minutes are impossible, and a clock
        // frozen at 14:05 at half past two reads as broken. The label changes the claim from "it is
        // now" to "last updated at", which is both true and useful. Two small lines rather than one
        // abbreviation: there is room for it here, because a device that never brings its radio up
        // has no failed sync to report either, so the "!wifi" pill can never appear beside it.
        // That last sentence is wrong, which is why clk_right below exists: the screen clock and
        // the radio interval are independent settings, so a Custom profile can switch the live
        // clock off while keeping the radio on, and Ultra Save can carry a failed-sync flag in
        // from before the switch, where nothing ever clears it.
        const char *l1="Last", *l2="Update";
        const int lw = Gw(1,l1) > Gw(1,l2) ? Gw(1,l1) : Gw(1,l2);
        const int tw = Ow(epd,3,d.time), gap = 7;
        const int x0 = 200 - (lw + gap + tw) / 2;
        // Each line centred within the label block, and the block centred on the row's middle
        // line — the same one the digits and the battery sit on.
        G(epd, x0 + (lw - Gw(1,l1)) / 2,  7, 1, l1);
        G(epd, x0 + (lw - Gw(1,l2)) / 2, 17, 1, l2);
        O(epd, x0 + lw + gap, 21, 3, d.time);
        clk_right = x0 + lw + gap + tw;
    }
    else { Oc(epd,200,21,3,d.time); clk_right = 200 + Ow(epd,3,d.time)/2; }   // live clock
    // A negative percentage means the gauge did not answer, which is not the same as an empty
    // cell — and drawn as "0 %" over a hairline of fill it reads as a device about to die. Dashes,
    // like every other missing reading on this screen, and no fill to argue with them.
    const bool batt_known = (d.batt >= 0);
    const int battp = d.batt > 100 ? 100 : d.batt;   // a gauge reading over full is still "100 %"
    char bs[8]; if (batt_known) snprintf(bs,sizeof(bs),"%d",battp); else snprintf(bs,sizeof(bs),"--");
    int bx=362, numw=Ow(epd,2,bs), pw=numw+4+Ow(epd,2,"%"), sx=bx-8-pw;
    // Plug sits left of the % with an 8px gap (outside the low-battery pill, so it stays
    // black-on-white); y9 centers its 12px height on the battery's own center line.
    if (d.usb) drawPlug(epd,sx-(d.lowbat?5:0)-18,9);
    if (d.lowbat) pxr(epd,sx-5,9,pw+9,14);      // y9 (not 8): centers the % digit in the pill
    uint8_t bc = d.lowbat?1:0;
    O(epd,sx,19,2,bs,bc); O(epd,sx+numw+4,19,2,"%",bc);
    frm(epd,bx,8,27,14); pxr(epd,bx+27,12,3,6);
    if (batt_known) { int lvl=(int)lround(battp*22/100.0); if(lvl<1)lvl=1; pxr(epd,bx+2,10,lvl,10); }
    if (d.chg) drawBolt(epd,bx+13,15);          // charging: bolt on the battery

    // "!wifi" sits in whatever gap the header leaves it, centred in it rather than at a fixed
    // spot: the clock is three characters wide or a moon, and the battery group grows a plug or a
    // low-battery pill, so a hardcoded x was only ever right for one combination. Vertically it
    // lines up with the battery's own centre line, which is what the eye reads the row against.
    if (d.wifiFail) {
        const char* t="!wifi";
        const int w=Gw(1,t)+8, h=13, cy=15;                 // 15 = centre of the battery outline
        // Taken from where the clock was actually laid out, not recomputed from its digits. The
        // recomputed form knew two of the three header layouts and understated the third — the
        // "Last / Update" one — by the width of its label block, so the pill was drawn over the
        // final digit of the very timestamp the mode exists to show.
        const int clkR = d.night ? 214 : clk_right;
        const int batL = d.usb ? sx-(d.lowbat?5:0)-18       // plug is the leftmost thing
                       : d.lowbat ? sx-5                    // ...or the low-battery pill
                       : sx;                                // ...or the percentage itself
        int x0 = clkR + ((batL-clkR)-w)/2;                  // centred in the gap
        if (x0 < clkR+6)     x0 = clkR+6;                   // never crowd the clock
        if (x0+w > batL-6)   x0 = batL-6-w;                 // nor the battery
        const int y0 = cy-h/2;
        pxr(epd,x0,y0,w,h);
        G(epd,x0+4,y0+(h-7)/2,1,t,1);                       // 7 = glcd cap height at size 1
    }
    hln(epd,0,30,400);

    // ===== GRID =====
    const int AX=185, BX=291, OX=250;
    vln(epd,AX,31,119); vln(epd,BX,31,119); hln(epd,0,150,400);
    hln(epd,AX,90,BX-AX); hln(epd,BX,90,400-BX);
    if (d.noNet!=2) vln(epd,OX,151,149);        // no divider in offline full-width mode

    // ===== CO2 hero =====
    int cA=AX/2;
    char tag[16]; snprintf(tag,sizeof(tag),"[%s]",d.fresh);
    { int gw2=Ow(epd,2,"CO2")+5+Gw(1,tag); int x0=cA-gw2/2;
      int xe=O(epd,x0,46,2,"CO2"); G(epd,xe+5,39,1,tag); }   // glcd (bigger than Org1 for the big hero)
    int csz=(Ow(epd,6,d.co2)<=175)?6:5;
    Oc(epd,cA,90,csz,d.co2); Oc(epd,cA,110,2,"PPM");
    // scale pill (Arduino-style): good=white | normal=dither | poor=SOLID black, with solid
    // vertical dividers between zones and the fill running into the rounded right end.
    const int bXp=22,bYp=132,bWp=140,bHp=8, seg=bWp/3;
    const int x1=bXp+seg, x2=bXp+2*seg, xR=bXp+bWp;
    epd.fillRectDither(x1+1, bYp+1, x2-(x1+1), bHp-2, 2, 0);   // normal (checker)
    epd.fillRoundRect(x2, bYp, xR-x2, bHp, 3, 0);              // poor solid, rounded right...
    epd.fillRect(x2, bYp, 4, bHp, 0);                          // ...squared on its left
    epd.drawRoundRect(bXp,bYp,bWp,bHp,3,0);                    // outer pill outline
    epd.fillRect(x1, bYp, 1, bHp, 0);                          // divider good|normal
    epd.fillRect(x2, bYp, 1, bHp, 0);                          // divider normal|poor
    // The needle may only be drawn when the hero is carrying a real number. Keying this to the
    // failure latch was not enough: "---" appears in TWO states, and the second one needs no fault
    // at all. A brand-new device repaints this dashboard during first-time setup before any sensor
    // has been read, and a sensor that has just started failing takes three misses to latch — some
    // thirty minutes on the default cadence. In both, atoi("---") is 0, the clamp lifts it to 400,
    // and the needle parks in the middle of the white "good" third. Everything else on the panel
    // is dashed and honest; the pill is the one element read without reading digits, and it was
    // the one asserting that unknown air is good.
    const bool co2_known = d.co2 && d.co2[0] >= '0' && d.co2[0] <= '9';
    if (co2_known) {
        int v=atoi(d.co2); if(v<400)v=400; if(v>1600)v=1600;
        int aX=(int)lround(bXp+5+(v-400)*(bWp-10)/1200.0);
        for(int i=0;i<=6;i++){ int hw=(int)lround(4*(1-i/6.0)); pxr(epd,aX-hw,bYp-8+i,2*hw+1,1); }
    }

    // ===== Temp / Hum =====
    int cB=(AX+BX)/2;
    Gc(epd,cB,40,1,"Temperature");
    if (d.err) Oc(epd,cB,74,3,"--");
    else { int tg=Ow(epd,3,d.temp)+2+Ow(epd,1,"o"); int te=O(epd,cB-tg/2,74,3,d.temp); odeg(epd,te,74,3); }
    // Damp belongs beside the number it was read from, and this block is the same 59 px slot as
    // Pressure next door — which already inverts for Storm. Same idiom, mirrored. The number keeps
    // size 3: all four big readings on this dashboard share it, and Storm only shrinks its own
    // because it must also fit a trend arrow. Nothing here needs one, so nothing is paid for.
    const bool damp = d.humDamp && !d.err;
    if (damp) pxr(epd,AX+1,91,BX-(AX+1),59);
    const uint8_t hc = damp ? 1 : 0;
    Gc(epd,cB,100,1,damp?"Humidity \xFA Damp":"Humidity",hc);   // 0xFA is glcd's middle dot
    if (d.err) Oc(epd,cB,134,3,"--");
    else { int hg=Ow(epd,3,d.hum)+2+Ow(epd,2,"%"); int he=O(epd,cB-hg/2,134,3,d.hum,hc); O(epd,he+2,134,2,"%",hc); }

    // ===== Air Quality / Pressure =====
    int cC=(BX+400)/2;
    if (d.aqInvert) pxr(epd,BX+1,31,399-BX,59);
    uint8_t aqc=d.aqInvert?1:0;
    G(epd,298,40,1,"Air Quality",aqc);
    if (d.cal) O(epd,396-Ow(epd,1,d.cal),46,1,d.cal,aqc);
    // The word at size 2 over seven cells, one per Bosch class (BME690 datasheet, table 6), filled up
    // to the class of the index (owner, 2026-09-14). No needle: there is no room for one. The words are
    // the screen's short forms, 8 letters at most (Analytics::iaqBandShort), so one size fits them all.
    Gc(epd,cC,56,2,d.aq,aqc);
    for(int i=0;i<7;i++){
        const int x=cC-45+i*13;
        epd.drawRect(x,78,12,8,aqc);
        if(i<=d.aqLevel) epd.fillRect(x+2,80,8,4,aqc);
    }
    if (d.presMode==0) {
        Gc(epd,cC,100,1,"Pressure");
        int pg=Ow(epd,3,d.pres)+3+Ow(epd,1,d.presU); int pe=O(epd,cC-pg/2,134,3,d.pres); O(epd,pe+3,134,1,d.presU);
    } else {
        // barometric forecast: Rain (fall 1.6-3.5 hPa/3h) or Storm (>3.5, inverted). §4.14
        const char* word=(d.presMode==1)?"Rain":"Storm"; bool inv=(d.presMode==2);
        if (inv) pxr(epd,BX+1,91,399-BX,59);
        uint8_t pc=inv?1:0;
        // The label carries the horizon while the word carries the verdict. "Pressure" is spare
        // here — the unit beside the number already says what this block is — and the word alone
        // reads as a promise about right now. With the level gate the lead time is hours, not the
        // days a bare falling barometer can mean, and saying so costs nothing: 12 characters at
        // size 1 in a 106 px block. In number mode the label stays "Pressure".
        Gc(epd,cC,100,1,"Within hours",pc);
        Gc(epd,cC,113,2,word,pc);
        int w=8+3+Ow(epd,2,d.pres)+2+Ow(epd,1,d.presU); int x0=cC-w/2;
        drawTrendArrow(epd,x0,136,d.ptrend,pc);
        int xe=O(epd,x0+11,144,2,d.pres,pc); O(epd,xe+2,144,1,d.presU,pc);   // the unit in Org, as in the number mode
    }

    // Damp and Storm arrive together more often than not — a front brings both — and then the two
    // inverted halves merge into one black band with the column rule swallowed inside it. The
    // forecast strip at the foot of the screen is the same shape and solves it the same way: one
    // black band, white partitions. Drawn last, because either half may have been painted over it.
    if (d.humDamp && !d.err && d.presMode == 2) pxr(epd,BX,91,1,59,1);
    // The same seam on the other axis, and OLDER than the damp move: Air Quality and Pressure are
    // the two halves of one column, so a polluted or stuffy room under a falling barometer already
    // merged them into one block with the rule between them black on black. Found while checking
    // the first divider, fixed with it because it is the identical case one line away.
    if (d.aqInvert && d.presMode == 2) pxr(epd,BX+1,90,399-BX,1,1);

    // ===== Graph =====
    { int xe=O(epd,8,165,1,"CO2"); char gt[10]; snprintf(gt,sizeof(gt),"[%s]",d.graphSpan?d.graphSpan:"24H"); O(epd,xe+3,165,1,gt); }
    if (d.noNet==2) {
        // OFFLINE variant (§4.14): full-width CO2 graph, big axis digits, dotted crosshair.
        static const uint8_t BAYER4[16]={0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5};
        int gL=64,gT=178,gR=384, gB=d.setupLine?(d.setupLine2?230:250):272;
        if (d.hist && d.histN>1) {
            float dmin=1e9f,dmax=-1e9f;
            for(int i=0;i<d.histN;i++){ if(d.hist[i]<dmin)dmin=d.hist[i]; if(d.hist[i]>dmax)dmax=d.hist[i]; }
            if(dmax-dmin<50) dmax=dmin+50;
            auto Yfor=[&](float v)->int{ if(v<dmin)v=dmin; if(v>dmax)v=dmax; return (int)lround(gB-(v-dmin)/(dmax-dmin)*(gB-gT)); };
            for(int i=0;i<d.histN-1;i++){
                int X0=(int)(gL+i*(gR-gL)/(float)(d.histN-1)), X1=(int)(gL+(i+1)*(gR-gL)/(float)(d.histN-1));
                int Y0=Yfor(d.hist[i]), Y1=Yfor(d.hist[i+1]);
                for(int X=X0;X<=X1&&X<=gR;X++){
                    float tt=(X1>X0)?(float)(X-X0)/(X1-X0):0.f; int cy=(int)lround(Y0+(Y1-Y0)*tt);
                    for(int Y=cy;Y<=gB;Y++){ float co2AtY=dmin+(float)(gB-Y)/(float)(gB-gT)*(dmax-dmin);
                        float dens=(co2AtY-800.0f)/800.0f; if(dens<0.0f)dens=0.0f; if(dens>1.0f)dens=1.0f;
                        if(dens>(BAYER4[(Y&3)*4+(X&3)]+0.5f)/16.f) epd.drawPixel(X,Y,0); }
                }
            }
            vln(epd,gL-2,gT-2,gB-(gT-2)+1); hln(epd,gL-2,gB+2,gR-(gL-2));
            float vmid=(dmin+dmax)/2.0f; int ymid=Yfor(vmid); int xmid=(gL+gR)/2;
            for(int x=gL;x<gR;x+=5) epd.drawPixel(x,ymid,0);          // dotted midlines
            for(int y=gT;y<gB;y+=5) epd.drawPixel(xmid,y,0);
            int px=0,py=0;
            for(int i=0;i<d.histN;i++){ int X=(int)(gL+i*(gR-gL)/(float)(d.histN-1)); int Y=Yfor(d.hist[i]); if(i>0) epd.drawLine(px,py,X,Y,0); px=X; py=Y; }
            char smax[12],smid[12],smin[12];
            snprintf(smax,12,"%d",(int)lround(dmax)); snprintf(smid,12,"%d",(int)lround(vmid)); snprintf(smin,12,"%d",(int)lround(dmin));
            // Right-aligned at gL-8 with nothing stopping them: Org_01 advances '1' at 2 px and
            // every other digit at 6, so a five-digit label with no '1' in it is 60 px wide and
            // starts left of the frame. Clipped, not unsafe — drawPixel bounds-checks — but the
            // leading digit loses columns against the border. Needs an SCD41 fault to reach (the
            // sensor's range ends at 5000), which is exactly the state this screen has to survive.
            auto axl=[&](const char*t,int y){ int x=gL-8-Ow(epd,2,t); if(x<2)x=2; O(epd,x,y,2,t); };
            axl(smax,gT+8); axl(smid,ymid+4); axl(smin,gB);
            O(epd,gL-2,gB+14,2,d.tStart);
            Oc(epd,xmid,gB+14,2,d.tMid);
            O(epd,gR-Ow(epd,2,d.tEnd),gB+14,2,d.tEnd);
        } else Gc(epd,(gL+gR)/2,(gT+gB)/2-4,1,"Collecting data...");
        if (d.setupLine) drawSetupBand(epd, d);
        return;                                 // offline: no outdoor block
    }
    // The outlook row takes the bottom of this block when there is one to show. The console's address
    // band outranks it: that band is up only while someone is holding the device and needs the
    // address right then, and the row comes back the moment the band goes.
    const bool fcRow = d.fcN > 0 && !d.setupLine;
    const int gL=38,gR=234;
    const int gT = fcRow ? 174 : 181;           // the max label still clears "CO2 [24H]" by 7 rows
    const int gB = fcRow ? 230 : d.setupLine ? (d.setupLine2 ? 242 : 262) : 275;   // shorten to clear the row / setup band
    if (fcRow) drawForecastRow(epd,d);
    vln(epd,gL-2,gT-2,gB-(gT-2)+1); hln(epd,gL-2,gB+2,gR-(gL-2));
    if (d.hist && d.histN>1) {
        float dmin=1e9f,dmax=-1e9f;
        for(int i=0;i<d.histN;i++){ if(d.hist[i]<dmin)dmin=d.hist[i]; if(d.hist[i]>dmax)dmax=d.hist[i]; }
        if(dmax-dmin<50) dmax=dmin+50;
        auto Yfor=[&](float v)->int{ if(v<dmin)v=dmin; if(v>dmax)v=dmax; return (int)lround(gB-(v-dmin)/(dmax-dmin)*(gB-gT)); };
        // Gradient under the line via ordered (Bayer 4x4) dither, shaded by ABSOLUTE CO2 (the
        // same 400->1600 mapping as the scale pill), NOT by relative height. So the grey at a
        // given CO2 matches the scale's good|normal|poor bands: the 800 and 1200 boundaries land
        // at density 0.33 / 0.67 exactly. A graph sitting in the good range stays light; only
        // climbs into normal/poor darken. Pure 1-bit, so it survives partial refresh.
        static const uint8_t BAYER4[16] = {0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5};
        // Below 800 (good) -> nothing painted (clean/white); from 800 a smooth ramp to medium at
        // ~1200 and to black by 1600. Matches the scale: good clear, normal grey, poor -> black.
        auto co2Dens=[&](float co2)->float{ float d=(co2-800.0f)/800.0f; return d<0.0f?0.0f:(d>1.0f?1.0f:d); };
        for(int i=0;i<d.histN-1;i++){
            int X0=(int)(gL+i*(gR-gL)/(float)(d.histN-1));
            int X1=(int)(gL+(i+1)*(gR-gL)/(float)(d.histN-1));
            int Y0=Yfor(d.hist[i]), Y1=Yfor(d.hist[i+1]);
            for(int X=X0; X<=X1 && X<=gR; X++){
                float tt=(X1>X0)?(float)(X-X0)/(X1-X0):0.f;
                int cy=(int)lround(Y0+(Y1-Y0)*tt);
                for(int Y=cy; Y<=gB; Y++){
                    float co2AtY = dmin + (float)(gB-Y)/(float)(gB-gT)*(dmax-dmin);
                    float dens=co2Dens(co2AtY);
                    float thr=(BAYER4[(Y&3)*4+(X&3)]+0.5f)/16.f;
                    if(dens>thr) epd.drawPixel(X,Y,0);
                }
            }
        }
        int px=0,py=0;
        for(int i=0;i<d.histN;i++){
            int X=(int)(gL+i*(gR-gL)/(float)(d.histN-1)); int Y=Yfor(d.hist[i]);
            if(i>0) epd.drawLine(px,py,X,Y,0);
            px=X; py=Y;
        }
        char smax[12],smin[12]; snprintf(smax,sizeof(smax),"%d",(int)lround(dmax)); snprintf(smin,sizeof(smin),"%d",(int)lround(dmin));
        O(epd,gL-4-Ow(epd,1,smax),gT+3,1,smax); O(epd,gL-4-Ow(epd,1,smin),gB,1,smin);
        O(epd,gL-2,gB+11,1,d.tStart); O(epd,gR-Ow(epd,1,d.tEnd),gB+11,1,d.tEnd);
    } else {
        Gc(epd,(gL+gR)/2,(gT+gB)/2-4,1,"Collecting data...");
    }
    // setup address band (inverted, shown on USB/portal) — §4.14 setupStyle 3
    if (d.setupLine) drawSetupBand(epd, d);

    // ===== Outdoor =====
    int cO=(OX+400)/2;
    if (!d.city) {
        Gc(epd,cO,205,1,"No weather yet"); Gc(epd,cO,222,1,"Hold the button 3 s"); Gc(epd,cO,239,1,"and add your city");
        return;
    }
    char tagw[16]; snprintf(tagw,sizeof(tagw)," [%s]",d.wxFresh);
    int maxc=24-(int)strlen(tagw);
    char full[48];
    if((int)strlen(d.city)<=maxc) snprintf(full,sizeof(full),"%s%s",d.city,tagw);
    else snprintf(full,sizeof(full),"%.*s..%s",maxc-2,d.city,tagw);
    // Everything above the strip sits a few pixels higher than it used to: the outlook's rule now
    // runs on across this block at y=250, and the description would otherwise sit on it.
    Gc(epd,cO,157,1,full);
    // weather icon (weather_icons.h). Centered a touch below the city label so the larger
    // glyph clears the city (top) and the description (bottom).
    // Shifted a few pixels towards the block's edge. The bottom row is full: icon and temperature
    // together fill the width to within a pixel or two, so the only way to give the degree mark
    // any room is to move everything left a little.
    int iconCx=(OX+cO)/2-3;
    epd.drawBitmap(iconCx-WX_ICON_W/2, 199-WX_ICON_H/2, weatherIcon(d.wxIcon), WX_ICON_W, WX_ICON_H, 0);
    // Outdoor temperature. The number sizes itself down (5->3) so Fahrenheit extremes still fit.
    // The degree mark takes one size more than three steps below the number whenever the width
    // allows, but never more than size 2 (8x8 px): at size 3 (12x12 px, a three-pixel stroke) beside a
    // size-5 number it was too big (owner, 2026-09-13).
    int tcx=(cO+400)/2-3;
    char ots[16]; snprintf(ots,sizeof(ots),"%s%d", d.outT>=0?"+":"-", abs(d.outT));
    const int budget=78;
    int osz=3, dsz=1;
    bool placed=false;
    for(int s=5;s>=3&&!placed;s--){
        for(int extra=1;extra>=0&&!placed;extra--){        // a bigger mark first, plain one as fallback
            int ds=s-3+extra; if(ds<1)ds=1; if(ds>2)ds=2;
            if(Ow(epd,s,ots)+2+Ow(epd,ds,"o")<=budget){ osz=s; dsz=ds; placed=true; }
        }
    }
    if(!placed){ osz=3; dsz=1; }
    { int grp=Ow(epd,osz,ots)+2+Ow(epd,dsz,"o"); int oe=O(epd,tcx-grp/2,197,osz,ots);
      odegAt(epd,oe,197,osz,dsz); }
    // min / max at size 2, NO degree marks (they pushed the group past the right screen edge).
    // "+15/+17" fits comfortably; only extreme Fahrenheit ("+68/+122") drops to size 1.
    int msz=(Ow(epd,2,d.mmLo)+Ow(epd,2,"/")+Ow(epd,2,d.mmHi)<=96)?2:1;
    { int mmw=Ow(epd,msz,d.mmLo)+Ow(epd,msz,"/")+Ow(epd,msz,d.mmHi); int mx0=tcx-mmw/2;
      if(mx0+mmw>391) mx0=391-mmw;                 // keep the last digit off the right border
      int xa=O(epd,mx0,222,msz,d.mmLo); int xb=O(epd,xa,222,msz,"/"); O(epd,xb,222,msz,d.mmHi); }
    // description (truncated)
    char descBuf[28];
    if((int)strlen(d.desc)<=24) snprintf(descBuf,sizeof(descBuf),"%s",d.desc);
    else snprintf(descBuf,sizeof(descBuf),"%.22s..",d.desc);
    Gc(epd,cO,236,1,descBuf);
    // The bottom strip. WHICH three things it holds is the owner's choice (strip_cells.h); the
    // outlook's BLACK BAND runs on across it to the right edge — together they make one band across
    // the whole screen — split by the same white dividers the day names have.
    //
    // The cells are EQUAL, 50 px, and stay equal whatever goes in them. That is forced, not chosen:
    // the content is picked in the console, so the layout cannot be fitted to it. Everything offered
    // therefore has to fit a 50 px cell, and the one thing that does not is a clock — five
    // characters where every other reading is two or three. Hence SUN holds BOTH times in one cell,
    // two rows, size 1, with the mark on the right; if a clock took a cell of its own it would, by
    // the row rule below, drag its neighbours down six times in area for no gain of its own.
    //
    // The units live in the band, and that is the point of the design rather than decoration: it
    // leaves the line below to the number alone, which is what lets the figure reach size 3 — the
    // weight of the highs on the left — in every weather.
    {
        const int top = FC_Y + FC_BAND_H, BOT = 298, LAB_GAP = 3, CW = 50;
        struct Opt { const char* lab; const char* unit; bool number; bool temp; };
        // Indexed by StripCell: the order here IS the wire format, see strip_cells.h.
        static const Opt OPT[STRIP_OPT_COUNT] = {
            {"Sun",  nullptr, false, false}, {"Moon", nullptr, false, false},
            {"Hum",  "%",     true,  false}, {"Wind", "m/s",   true,  false},
            {"Feels",nullptr, true,  true }, {"UV",   nullptr, true,  false},
        };
        const uint32_t packed = d.strip ? d.strip : STRIP_DEFAULT;
        uint8_t id[3]; const char* val[3];
        for(int i=0;i<3;i++){
            id[i] = strip_cell(packed, i);
            // An id this build does not know cannot be drawn, and refusing the whole strip over one
            // bad byte would blank two good cells. Fall back per cell instead.
            if(id[i] >= STRIP_OPT_COUNT) id[i] = strip_cell(STRIP_DEFAULT, i);
            const char* v = nullptr;
            switch(id[i]){
                case STRIP_HUM:   v = d.outHum; break;
                case STRIP_WIND:  v = d.wind;   break;
                case STRIP_FEELS: v = d.feels;  break;
                case STRIP_UV:    v = d.uv;     break;
                default: break;                       // widgets carry no string
            }
            val[i] = OPT[id[i]].number ? (v ? v : "--") : nullptr;
        }
        // Ink width, not advance: a string's ink is its advance less one size step, as
        // drawForecastRow already counts it. ONE size for the row, the largest every NUMBER takes.
        // Widgets take no part — they have their own fixed typography, so a sun or a moon beside a
        // number never shrinks it.
        auto vis = [&](int s, const char* t){ return Ow(epd,s,t) - s; };
        int vs = 1;
        for(int s=3;s>=1;s--){
            bool all = true;
            for(int i=0;i<3;i++) if(val[i] && vis(s,val[i]) > 44) all = false;
            if(all){ vs = s; break; }
        }
        pxr(epd,OX,FC_Y,400-OX,FC_BAND_H,0);
        for(int i=0;i<3;i++){
            const int x0 = OX + i*CW, cx = x0 + CW/2;
            if(i>0){ pxr(epd,x0,FC_Y,1,FC_BAND_H,1); vln(epd,x0,top+3,BOT-top-4); }
            // The word and its unit with a 3 px gap, not a 6 px space: "Wind m/s" is eight characters
            // in a 50 px cell, and a full space left it flush against the dividers.
            const Opt& o = OPT[id[i]];
            const int lw = Gw(1,o.lab) + (o.unit ? LAB_GAP + Gw(1,o.unit) : 0);
            const int le = G(epd,cx-lw/2,FC_Y+2,1,o.lab,1);
            if(o.unit) G(epd,le+LAB_GAP,FC_Y+2,1,o.unit,1);

            if(id[i] == STRIP_SUN){
                // Two rows, size 1, the mark on the right. Worst case "00:00"/"20:00" this is
                // 37 x 16 px inside 50 x 37. The times share one column width so the two marks line
                // up: centring each row on its own width would stagger them, because "11:11" is nine
                // pixels of ink where "00:00" is twenty-five.
                const char* a = d.sunrise ? d.sunrise : "--:--";
                const char* b = d.sunset  ? d.sunset  : "--:--";
                const int colw = vis(1,a) > vis(1,b) ? vis(1,a) : vis(1,b);
                const int xs = cx - (colw + LAB_GAP + 9)/2;
                const int gt = top + ((BOT - top) - (5 + 6 + 5))/2;
                for(int j=0;j<2;j++){
                    const char* t = j ? b : a;
                    const int y = gt + j*11;
                    O(epd, xs + (colw - vis(1,t))/2, y + 4, 1, t);
                    // Solid 45-degree triangle, rows 1-3-5-7-9. Every row is wider than the one
                    // above by exactly two, so every step along the edge is identical; repeat a row
                    // and the edge gets one step of double height, which the eye catches at this
                    // size. Apex up for the rise, down for the set.
                    const int tcx = xs + colw + LAB_GAP + 4;
                    for(int k=0;k<5;k++){
                        const int w = 1 + 2*k, ry = j ? (y + 4 - k) : (y + k);
                        pxr(epd, tcx - k, ry, w, 1, 0);
                    }
                }
                continue;
            }
            if(id[i] == STRIP_MOON){
                if(d.moon < 0.0f){
                    // No clock, so no phase. An empty cell under a "Moon" label reads as a fault,
                    // and an empty circle would read as a new moon, so it says what every other cell
                    // says when it has nothing: dashes.
                    const int nb = (top+1 + BOT)/2 - (5*vs)/2 + 4*vs;
                    O(epd, cx - vis(vs,"--")/2, nb, vs, "--");
                    continue;
                }
                // Terminator ellipse, scanline by scanline: the disc, then the unlit side wiped, then
                // the ellipse that separates them. Drawn here rather than with fillCircle because the
                // terminator is an ellipse and there is no primitive for one.
                const int r = 9, mcy = (top + BOT)/2;
                const float k = (1.0f - cosf(6.2831853f * d.moon)) * 0.5f;   // lit fraction
                const bool waxing = d.moon < 0.5f;
                const int a = (int)(r * fabsf(2.0f*k - 1.0f) + 0.5f);
                for(int y=-r; y<=r; y++){
                    const int dx = (int)(sqrtf((float)(r*r - y*y)) + 0.5f);
                    if(dx <= 0) continue;
                    pxr(epd, cx-dx, mcy+y, 2*dx+1, 1, 0);                    // whole disc
                    if(waxing) pxr(epd, cx-dx, mcy+y, dx, 1, 1);             // wipe the left half
                    else       pxr(epd, cx+1,  mcy+y, dx, 1, 1);             // wipe the right half
                    if(a){
                        const int ax = (int)((float)a * (float)dx / (float)r + 0.5f);
                        if(ax) pxr(epd, cx-ax, mcy+y, 2*ax+1, 1, k < 0.5f ? 1 : 0);
                    }
                }
                // The limb, drawn as a curve rather than one pixel per scanline: near the top and
                // bottom dx jumps by more than one between rows, and per-row pixels left the outline
                // visibly dotted on the unlit side. Sixty steps is well past one per pixel at r = 9.
                for(int t2=0; t2<60; t2++){
                    const float an = 6.2831853f * (float)t2 / 60.0f;
                    epd.drawPixel(cx + (int)(r*cosf(an) + (cosf(an)>=0?0.5f:-0.5f)),
                                  mcy + (int)(r*sinf(an) + (sinf(an)>=0?0.5f:-0.5f)), 0);
                }
                continue;
            }
            // A plain number, centred in the cell both ways. Vertically on the INK: Org_01 digits are
            // height 5 at yOffset -4 and have no descender, so centring on the glyph cell rides high.
            const int base = (top+1 + BOT)/2 - (5*vs)/2 + 4*vs;
            // The degree survives only where it leaves real air, and only on a temperature — UV has
            // no unit but is not one. Merely fitting is not enough: at the tighter test "-19" cleared
            // the divider by half a pixel a side and read as jammed, so the mark gives way instead of
            // the digits — the label already says this is a temperature.
            const bool deg = o.temp && (Ow(epd,vs,val[i]) + 6 <= 42);
            const int w = deg ? Ow(epd,vs,val[i]) + 6 : vis(vs,val[i]);
            const int xe = O(epd,cx-w/2,base,vs,val[i]);
            if(deg) odegAt(epd,xe,base,vs,1);
        }
    }
}

// ---- Matter pairing ---------------------------------------------------------------------------
// The QR code carries the Matter onboarding payload ("MT:..."); the numbers under it are the same
// code for typing. Module size is the largest whole number of pixels that keeps the code within
// 150 px, which leaves the four-module quiet zone the QR standard asks for on every side (the
// payload always encodes as version 2, 25 modules: 6 px each, 24 px of white around).
void drawPairingScreen(EPaper& epd, const PairingData& d) {
    epd.clear(0xFF);
    // Без внешней рамки, как и дашборд: границу даёт корпус.

    // Title band
    pxr(epd,0,0,400,32);
    Gc(epd,200,9,2,"ADD TO YOUR SMART HOME",1);

    // The code
    const int left_cx = 106;
    if (d.qr && d.qrSize > 0) {
        int sc = 150 / d.qrSize;
        if (sc > 6) sc = 6;
        if (sc < 1) sc = 1;
        const int side = d.qrSize * sc;
        const int x0 = left_cx - side / 2, y0 = 32 + 4 * sc;
        for (int y = 0; y < d.qrSize; y++)
            for (int x = 0; x < d.qrSize; x++)
                if (d.qr[y * d.qrSize + x]) pxr(epd, x0 + x * sc, y0 + y * sc, sc, sc);
        const int ty = y0 + side + 12;
        Gc(epd,left_cx,ty,1,"Setup code");
        if (d.code) Oc(epd,left_cx,ty + 24,2,d.code);
    } else {
        Gc(epd,left_cx,120,1,"No setup code");
    }

    // Steps
    const int x = 214;
    struct Step { const char* n; const char* l1; const char* l2; const char* l3; };
    static const Step steps[] = {
        { "1", "Open your smart home app:", "Google Home, Apple Home,", "Alexa or SmartThings" },
        { "2", "Add a device, then choose", "Matter", nullptr },
        { "3", "Scan this code, or type", "the numbers under it", nullptr },
    };
    int y = 48;
    for (const Step& st : steps) {
        O(epd, x, y + 9, 2, st.n);
        G(epd, x + 16, y, 1, st.l1);
        if (st.l2) G(epd, x + 16, y + 11, 1, st.l2);
        if (st.l3) G(epd, x + 16, y + 22, 1, st.l3);
        y += st.l3 ? 42 : 31;
    }
    hln(epd, x, y + 2, 176);
    G(epd, x, y + 12, 1, "A Thread hub is needed:");
    G(epd, x, y + 24, 1, "Nest Hub (2nd gen), Nest");
    G(epd, x, y + 35, 1, "Wifi Pro, HomePod mini,");
    G(epd, x, y + 46, 1, "Apple TV 4K, Echo (4th gen)");

    // State band
    pxr(epd,0,266,400,34);
    const char* word = "READY TO PAIR";
    const char* h1 = "Pairing stays open 15 min";
    switch (d.state) {
        case PAIR_JOINING: word = "CONNECTING";     h1 = "Keep the phone nearby";      break;
        case PAIR_CLOSED:  word = "PAIRING PAUSED"; h1 = "Press the button to resume"; break;
        case PAIR_ERROR:   word = "NOT AVAILABLE";  h1 = "Smart home did not start";   break;
        default: break;
    }
    const char* h2 = "Hold 3 s: settings";
    G(epd, 10, 275, 2, word, 1);
    G(epd, 390 - Gw(1, h1), 273, 1, h1, 1);
    G(epd, 390 - Gw(1, h2), 285, 1, h2, 1);
}


// ---- switching off by hand -------------------------------------------------------------------
// Laid out on the instruction rather than on the state. Weeks later the owner does not need to be
// told what "OFF" means; they need to be told which button brings it back, and that nothing was
// lost by switching it off. So the word is big enough to read across a room and the sentence under
// it is the one that answers the question they will actually have.
// The logo, one bit a pixel. Blitted rather than composed: the four-gray original is driven by a
// vendor sequence of its own that no text can be written into, and on this screen the words are the
// point. Same drawing either way — logo_small.py reduces the expanded planes of main/logo4g.h, so
// the glass shows what the device already shows at its first power-on.
static void blitLogo(EPaper& e, int x0, int y0)
{
    for (int y = 0; y < LOGO_SMALL_H; y++)
        for (int x = 0; x < LOGO_SMALL_W; x++)
            if (logo_small[y * LOGO_SMALL_STRIDE + x / 8] & (0x80 >> (x % 8)))
                e.drawPixel(x0 + x, y0 + y, 0);
}

void drawOffScreen(EPaper& epd, const char* name)
{
    epd.clear(0xFF);
    blitLogo(epd, 200 - LOGO_SMALL_W / 2, 10);
    Gc(epd, 200, 148, 6, "OFF");
    Gc(epd, 200, 208, 2, "PRESS THE BUTTON TO START");
    // The device's own name folded into the small print rather than given a line of its own: the
    // logo already says what this is, and the name only answers "which one" for a shelf with two.
    // ...and dropped again when it does not fit. dev_name is whatever the owner typed, so the
    // combined line can be any length; the sentence is the content and the name is the nicety, so
    // the nicety is what goes. Measured rather than assumed — at size 1 that is six pixels a
    // character, and the screen is four hundred wide.
    char keep[128];
    const char *plain = "Readings, settings and calibration are kept";
    if (name && name[0]) {
        snprintf(keep, sizeof(keep), "%s - %s", name, plain);
        if (Gw(1, keep) > 384) snprintf(keep, sizeof(keep), "%s", plain);
    } else {
        snprintf(keep, sizeof(keep), "%s", plain);
    }
    Gc(epd, 200, 246, 1, keep);
    // The one genuine cost of a long break, said here rather than discovered later: BSEC carries a
    // learned baseline through time, and after weeks away it comes back at accuracy 0 and needs
    // hours of running to earn its verdict again. That looks like a fault if nobody warned you.
    Gc(epd, 200, 264, 1, "Air quality needs a few hours to settle again");
}

// ---- the CO2 calibration, on the glass -------------------------------------------------------
// Laid out from Software/mockups/render_calib.py, approved by the owner 2026-09-30. Words in glcd,
// numbers in Org_01, the same division the rest of this file has always used.
void drawCalibScreen(EPaper& epd, const CalibScreen& c)
{
    epd.clear(0xFF);
    static const char *WORD[4] = { "SETTLING", "MEASURING", "DONE", "STOPPED" };
    static const char *SUB[4]  = { "waiting for the air in the case to change",
                                   "the air has settled - six readings, one a minute",
                                   "baseline anchored to fresh outdoor air", "" };
    const uint8_t ph = (c.phase > 3) ? 3 : c.phase;

    G(epd, 6, 6, 1, "CO2 CALIBRATION");
    char right[16] = {0};
    if (ph < 2) snprintf(right, sizeof(right), "step %d of 2", ph + 1);
    if (right[0]) G(epd, 400 - 6 - Gw(1, right), 6, 1, right);
    hln(epd, 0, 18, 400);

    G(epd, 6, 28, 3, WORD[ph]);
    G(epd, 6, 60, 1, ph == 3 ? (c.why ? c.why : "nothing was changed") : SUB[ph]);

    if (c.value >= 0) {
        // Org_01 is height 5 with yOffset -4, so the call takes a BASELINE and the ink lands above
        // it: base = ink_top + 4*size. The same arithmetic the outdoor strip already uses; getting
        // it wrong is what put the digits through the sub-line on the mockup's first pass.
        const int s = 9, top = 96, base = top + 4 * s;
        char num[8]; snprintf(num, sizeof(num), "%d", (int)c.value);
        const int nw = Ow(epd, s, num), uw = Gw(2, "ppm");
        const int x = 200 - (nw + 10 + uw) / 2;
        O(epd, x, base, s, num);
        G(epd, x + nw + 10, top + 5 * s - 16, 2, "ppm");
    }
    if (c.trail_n) {
        char line[64] = {0};
        int n = 0;
        for (uint8_t i = 0; i < c.trail_n && i < 6; i++)
            n += snprintf(line + n, sizeof(line) - n, i ? "  %d" : "%d", (int)c.trail[i]);
        Gc(epd, 200, 155, 1, line);
    }
    if (ph == 1 || ph == 2) {
        // Cells filled from the left, one per reading — the idiom the Air Quality block already
        // uses. A count in words would say the same; a row of boxes says it without being read.
        const int cw = 18, ch = 12, gap = 6;
        const int x0 = 200 - (6 * cw + 5 * gap) / 2;
        for (int i = 0; i < 6; i++) {
            const int x = x0 + i * (cw + gap);
            frm(epd, x, 185, cw, ch);
            if (i < c.done) pxr(epd, x + 3, 188, cw - 6, ch - 6);
        }
    }
    if (ph == 2) {
        char t[32]; snprintf(t, sizeof(t), "correction %+d ppm", (int)c.corr);
        Gc(epd, 200, 218, 2, t);
    }
    if (ph == 3) {
        Gc(epd, 200, 110, 2, "Try again outside");
        Gc(epd, 200, 145, 1, "the baseline is exactly as it was");
    }
    if (c.ran_min && ph != 3) {
        char t[32];
        snprintf(t, sizeof(t), ph == 2 ? "took %u min" : "running %u min", (unsigned)c.ran_min);
        Gc(epd, 200, 248, 1, t);
    }
    // One instruction, inverted, impossible to miss — and it is the instruction that changes when
    // the device may be carried back in.
    pxr(epd, 0, 300 - 26, 400, 26);
    const char *bar = (ph >= 2) ? "TAKE IT BACK INSIDE" : "KEEP IT OUTSIDE";
    Gc(epd, 200, 300 - 26 + 5, 2, bar, 1);
}

// The question, and it is asked on the glass rather than in the console because this is the path
// for someone with no phone in their hand. Everything about it is reversible: another press
// confirms, anything else does not, and doing nothing at all puts the device back to work.
void drawOffConfirmScreen(EPaper& epd, const char* name)
{
    epd.clear(0xFF);
    if (name && name[0]) Gc(epd, 200, 44, 2, name);
    Gc(epd, 200, 104, 5, "TURN OFF?");
    Gc(epd, 200, 178, 2, "PRESS THE BUTTON AGAIN");
    Gc(epd, 200, 206, 2, "TO CONFIRM");
    Gc(epd, 200, 244, 1, "Readings, settings and calibration are kept");
    Gc(epd, 200, 262, 1, "Doing nothing keeps the device running");
}
