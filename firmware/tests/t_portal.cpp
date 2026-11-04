// Экран настройки Wi-Fi: он собирается на приборе из картинки, QR и букв пароля, и **отказывается
// целиком**, если хоть у одного знака нет глифа — тогда со стекла пропадают и код, и пароль. После
// того как алфавит стал полным (93 знака, 2026-09-19), это единственное место, где цена ошибки в
// алфавите видна сразу.
//
// Настоящие portal_screen.cpp, portal_art.cpp и qrcodegen.c — зависимостей от ESP-IDF у них нет.
#include "check.h"
#include "ap_password.h"
#include "portal_screen.cpp"
#include "logo4g.h"
#include "lock4g.h"
#include <string>
#include <vector>
#include <cstdio>

static const char *ACCEPT =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVXYZ0123456789"
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";

static uint8_t p24[PortalScreen::kPlaneBytes], p26[PortalScreen::kPlaneBytes];

// 0 чёрный .. 3 белый, как в таблице вендора
static int px(int x, int y) {
    const int i = y * 400 + x, byte = i >> 3, bit = 7 - (i & 7);
    return (((p24[byte] >> bit) & 1) << 1) | ((p26[byte] >> bit) & 1);
}

struct Box { int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1; bool any() const { return x1 >= 0; } };
static Box inkIn(int y0, int y1) {
    Box b;
    for (int y = y0; y <= y1; y++)
        for (int x = 0; x < 400; x++)
            if (px(x, y) < 3) {
                if (x < b.x0) b.x0 = x; if (x > b.x1) b.x1 = x;
                if (y < b.y0) b.y0 = y; if (y > b.y1) b.y1 = y;
            }
    return b;
}

int main() {
    const char *SSID = "Smart Meteo";

    area("алфавит");
    eq_int((int)strlen(PORTAL_AP_PASS_ALPHABET), 93, "принимаемых знаков 93");
    eq_int((int)strlen(PORTAL_AP_PASS_GEN_ALPHABET), 79, "генератор берёт из 79");
    eq_int(PORTAL_AP_PASS_LEN, 8, "длина фиксирована на 8");
    eq_int(PORTAL_AP_PASS_MIN, PORTAL_AP_PASS_LEN, "минимум равен максимуму — длина фиксирована");
    {
        bool subset = true, dup = false;
        for (const char *c = PORTAL_AP_PASS_GEN_ALPHABET; *c; c++)
            if (!strchr(PORTAL_AP_PASS_ALPHABET, *c)) subset = false;
        for (const char *c = PORTAL_AP_PASS_ALPHABET; *c; c++)
            if (strchr(c + 1, *c)) dup = true;
        ok(subset, "генератор берёт только из принимаемого");
        ok(!dup, "повторов в алфавите нет — иначе перекос распределения");
        ok(!strchr(PORTAL_AP_PASS_ALPHABET, 'W'), "заглавная W не принимается: 16.3 px не влезают");
        ok(strchr(PORTAL_AP_PASS_ALPHABET, 'w') != nullptr, "строчная w принимается");
        ok(!strchr(PORTAL_AP_PASS_ALPHABET, ' '), "пробел не принимается: на стекле его не видно");
    }

    area("каждый знак рисуется");
    {
        int bad = 0; std::string worst;
        for (const char *c = ACCEPT; *c; c++) {
            std::string pw(PORTAL_AP_PASS_LEN, *c);
            if (!PortalScreen::compose(p24, p26, SSID, pw.c_str())) { bad++; worst = pw; }
        }
        ok(bad == 0, bad ? ("не собрались пароли из знака: " + worst).c_str()
                         : "все 93 знака по восемь штук подряд собираются");
    }
    {
        // Самый широкий возможный пароль — тот, ради которого длина и равна восьми.
        ok(PortalScreen::compose(p24, p26, SSID, "mmmmmmmm"), "самый широкий пароль собирается");
        Box b = inkIn(160, 185);
        ok(b.any(), "строка пароля нарисована");
        char msg[80];
        snprintf(msg, sizeof(msg), "чернила строки внутри панели: x %d..%d", b.x0, b.x1);
        ok(b.x0 >= 2 && b.x1 <= 397, msg);
    }
    {
        // Знаки, которые экранируются в QR и в C-литерале, — на них ломались бы генератор и разбор.
        ok(PortalScreen::compose(p24, p26, SSID, "\\\";,:'`~"), "экранируемые знаки собираются");
        ok(PortalScreen::compose(p24, p26, "Wi-Fi;\\name", "aBc12!@#"), "имя сети со спецзнаками");
    }

    area("отказ оставляет голую картинку");
    for (const char *pw : {"Wwwwwwww", "abc defg", "abcdefg\n", "abcdefg\x01"}) {
        memset(p24, 0xAA, sizeof(p24)); memset(p26, 0x55, sizeof(p26));
        const bool got = PortalScreen::compose(p24, p26, SSID, pw);
        ok(!got, "знак вне алфавита отвергнут");
        if (!got) {
            static uint8_t b24[PortalScreen::kPlaneBytes], b26[PortalScreen::kPlaneBytes];
            ok(PortalScreen::expandArt(b24, b26) &&
               memcmp(p24, b24, PortalScreen::kPlaneBytes) == 0 &&
               memcmp(p26, b26, PortalScreen::kPlaneBytes) == 0,
               "плоскости остались голой картинкой, а не полуфабрикатом");
        }
    }
    {
        std::string too_long(PORTAL_AP_PASS_LEN + 1, 'a');
        ok(PortalScreen::compose(p24, p26, SSID, too_long.c_str()) ||
           !PortalScreen::compose(p24, p26, SSID, too_long.c_str()),
           "девять знаков не роняют сборку");   // рисоваться может, но падать не должно
    }


    area("распаковка картинок");
    {
        // Каждая плоскость распаковывается кодом ПРОШИВКИ (rle.h) и сверяется с числом, посчитанным
        // по данным, чей sha256 совпал с исходником ДО сжатия. То есть проверяются две независимые
        // реализации против заведомо верного источника, а не одна против самой себя.
        auto fnv = [](const uint8_t *b, size_t n) {
            uint64_t h = 0xcbf29ce484222325ULL;
            for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ULL;
            return h;
        };
        struct P { const uint8_t *rle; size_t n; uint64_t want; const char *name; };
        static const P planes[] = {
            { portal_art_plane24_rle, PORTAL_ART_PLANE24_RLE_LEN, 0xBCA91792FF7055B3ULL, "portal_art_plane24" },
            { portal_art_plane26_rle, PORTAL_ART_PLANE26_RLE_LEN, 0x2FA507F3883B1EF4ULL, "portal_art_plane26" },
            { logo4g_plane24_rle,     LOGO4G_PLANE24_RLE_LEN,     0x7239498CF648D41AULL, "logo4g_plane24" },
            { logo4g_plane26_rle,     LOGO4G_PLANE26_RLE_LEN,     0xF6958542881B02BFULL, "logo4g_plane26" },
            { lock4g_plane24_rle,     LOCK4G_PLANE24_RLE_LEN,     0x8FD23CD3C024C602ULL, "lock4g_plane24" },
            { lock4g_plane26_rle,     LOCK4G_PLANE26_RLE_LEN,     0x86EDA3064ED91629ULL, "lock4g_plane26" },
        };
        static uint8_t plane[PortalScreen::kPlaneBytes];
        for (const P &p : planes) {
            char msg[96];
            const bool got = rle_expand(plane, sizeof(plane), p.rle, p.n);
            snprintf(msg, sizeof(msg), "%s распаковалась", p.name);
            ok(got, msg);
            snprintf(msg, sizeof(msg), "%s совпала с эталоном", p.name);
            ok(got && fnv(plane, sizeof(plane)) == p.want, msg);
        }
        // Битый поток — это несовпадение сборки, а не смазанная картинка, и должен быть отказом.
        ok(!rle_expand(plane, sizeof(plane), portal_art_plane24_rle, PORTAL_ART_PLANE24_RLE_LEN - 1),
           "обрезанный поток отвергается");
        ok(!rle_expand(plane, 10, portal_art_plane24_rle, PORTAL_ART_PLANE24_RLE_LEN),
           "поток длиннее буфера отвергается");
        uint8_t small[4];
        const uint8_t exact[] = { 0x83, 0xAB };          // повтор 0xAB четыре раза
        ok(rle_expand(small, 4, exact, sizeof(exact)) && small[0] == 0xAB && small[3] == 0xAB,
           "короткий поток разбирается верно");
    }

    area("батарейка в углу");
    {
        // Где группа может лечь: самая широкая — вилка и «100%» — начинается с x 292. Картинка экрана
        // в этом углу обязана быть белой, иначе батарейка рисуется поверх рисунка.
        ok(PortalScreen::expandArt(p24, p26), "картинка распакована");
        {
            const Box art = inkIn(0, 29);       // в этих строках есть верх логотипа, по центру
            char msg[96];
            snprintf(msg, sizeof(msg), "в строках 0..29 рисунок кончается на x %d — левее угла 286", art.x1);
            ok(!art.any() || art.x1 < 286, msg);
        }

        // Тот же угол, что у дашборда, — пиксель в пиксель против эталонов, нарисованных настоящим
        // screen.cpp (tests/golden, t_screen). Состояния взяты из tools/screenhost/main.cpp.
        const std::string file = __FILE__;
        const std::string gdir = file.substr(0, file.rfind('/') + 1) + "golden/";
        struct C { const char *g; PortalScreen::Battery b; } cs[] = {
            { "normal",  { 87,  false, false } },
            { "extreme", { 100, false, false } },
            { "night",   { 4,   false, false } },   // ниже 15 %: белые цифры в чёрной плашке
            { "charge",  { 100, true,  true  } },   // вилка, цифры и молния вместе
        };
        for (const C &c : cs) {
            static uint8_t gb[PortalScreen::kPlaneBytes];
            FILE *f = fopen((gdir + c.g + ".pbm").c_str(), "rb");
            char m[3] = {0}; int w = 0, h = 0;
            const bool got = f && fscanf(f, "%2s %d %d", m, &w, &h) == 3 && !strcmp(m, "P4") &&
                             w == 400 && h == 300 && fgetc(f) != EOF && fread(gb, 1, sizeof(gb), f) == sizeof(gb);
            if (f) fclose(f);
            char msg[120];
            snprintf(msg, sizeof(msg), "эталон %s.pbm прочитан", c.g);
            ok(got, msg);
            if (!got) continue;
            memset(p24, 0xFF, sizeof(p24)); memset(p26, 0xFF, sizeof(p26));
            PortalScreen::drawBattery(p24, p26, c.b);
            const Box b = inkIn(0, 29);
            int diff = 0;
            for (int y = 6; y <= 24; y++)
                for (int x = b.x0 - 3; x <= 394; x++)
                    if ((((gb[y * 50 + x / 8] >> (7 - x % 8)) & 1) != 0) != (px(x, y) == 0)) diff++;
            snprintf(msg, sizeof(msg), "%s: угол совпал с дашбордом (x %d..394, отличий %d px)",
                     c.g, b.x0 - 3, diff);
            ok(diff == 0, msg);
        }

        // Заряд идёт — цифры нет: напряжение на кабеле принадлежит заряднику, а не ячейке. Вилка
        // тогда стоит в 8 px от контура, как от цифр.
        memset(p24, 0xFF, sizeof(p24)); memset(p26, 0xFF, sizeof(p26));
        PortalScreen::drawBattery(p24, p26, { -1, true, true });
        eq_int(inkIn(0, 29).x0, 362 - 18, "без цифры вилка стоит в 8 px от контура");
        ok(px(362 + 13, 15) == 3, "молния белая внутри контура");

        // Плашка — ровно правило дашборда (bench_screens.cpp: lowbat = soc < 15 && !usb): у границы и
        // на кабеле. Эталонов с такими числами нет, поэтому смотрится сама плашка: сплошная чёрная
        // полоса в строке y 9 левее контура. Цифры размера 2 туда не достают никогда, а у вилки в этой
        // строке только зубцы по 2 px — первая проба смотрела на самую левую точку группы, а с кабелем
        // это вилка, и плашку на кабеле она пропускала.
        auto pill = [&](PortalScreen::Battery b) {
            memset(p24, 0xFF, sizeof(p24)); memset(p26, 0xFF, sizeof(p26));
            PortalScreen::drawBattery(p24, p26, b);
            int run = 0, best = 0;
            for (int x = 286; x < 362; x++) { run = px(x, 9) == 0 ? run + 1 : 0; if (run > best) best = run; }
            return best >= 9;
        };
        ok(pill({ 14, false, false }), "14 % на батарее — в плашке");
        ok(!pill({ 15, false, false }), "15 % на батарее — без плашки");
        ok(!pill({ 9, true, false }), "9 % на кабеле — без плашки, как на дашборде");

        // Заливка — формула дашборда lround(p * 22 / 100), не меньше 1 px, на всех 101 значениях:
        // три эталона выше не различают округление вниз и к ближайшему.
        {
            int bad = -1;
            for (int p = 0; p <= 100 && bad < 0; p++) {
                memset(p24, 0xFF, sizeof(p24)); memset(p26, 0xFF, sizeof(p26));
                PortalScreen::drawBattery(p24, p26, { p, false, false });
                int w = 0;
                for (int x = 362 + 1; x <= 362 + 25; x++) w += px(x, 15) == 0;
                long want = lround(p * 22 / 100.0);
                if (want < 1) want = 1;
                if (w != want) bad = p;
            }
            char msg[80];
            snprintf(msg, sizeof(msg), bad < 0 ? "заливка 0..100 %% совпадает с дашбордом"
                                               : "заливка разошлась с дашбордом на %d %%", bad);
            ok(bad < 0, msg);
        }

        // Сама группа на белом листе — только в своём углу, в каждом из состояний.
        {
            bool inside = true;
            for (PortalScreen::Battery b : { PortalScreen::Battery{ 100, true, false },
                                             PortalScreen::Battery{ 0, false, false },
                                             PortalScreen::Battery{ -1, true, true } }) {
                memset(p24, 0xFF, sizeof(p24)); memset(p26, 0xFF, sizeof(p26));
                PortalScreen::drawBattery(p24, p26, b);
                for (int y = 0; y < 300; y++)
                    for (int x = 0; x < 400; x++)
                        if ((y >= 30 || x < 286) && px(x, y) != 3) inside = false;
            }
            ok(inside, "на белом листе группа не выходит из угла x 286..399, y 0..29");
        }

        // Поверх собранного экрана батарейка трогает только свой угол: QR и пароль на месте.
        static uint8_t a24[PortalScreen::kPlaneBytes], a26[PortalScreen::kPlaneBytes];
        ok(PortalScreen::compose(p24, p26, SSID, "aBc12!@#"), "экран собран");
        memcpy(a24, p24, sizeof(a24)); memcpy(a26, p26, sizeof(a26));
        PortalScreen::drawBattery(p24, p26, { 100, true, false });
        bool outside = true;
        for (int y = 0; y < 300; y++)
            for (int x = 0; x < 400; x++) {
                if (y < 30 && x >= 286) continue;
                const size_t i = (size_t)y * 50 + x / 8; const uint8_t bit = (uint8_t)(0x80 >> (x % 8));
                if ((p24[i] & bit) != (a24[i] & bit) || (p26[i] & bit) != (a26[i] & bit)) outside = false;
            }
        ok(outside, "вне угла x 286..399, y 0..29 не изменилось ни одного пикселя");
    }

    area("текст QR");
    {
        char t[256];
        ok(PortalScreen::wifi_qr_text(t, sizeof(t), "Smart Meteo", "aBc12!@#"), "строка QR собрана");
        eq_str(t, "WIFI:S:Smart Meteo;T:WPA;P:aBc12!@#;;", "обычный случай");
        ok(PortalScreen::wifi_qr_text(t, sizeof(t), "Smart Meteo", "a\\b;c,d:"), "пароль со спецзнаками");
        eq_str(t, "WIFI:S:Smart Meteo;T:WPA;P:a\\\\b\\;c\\,d\\:;;", "каждый спецзнак экранирован");
        char small[8];
        ok(!PortalScreen::wifi_qr_text(small, sizeof(small), "Smart Meteo", "aBc12!@#"), "тесный буфер — отказ");
    }

    return report("экран настройки");
}
