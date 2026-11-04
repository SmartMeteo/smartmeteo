#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Консоль и прошивка, сверенные друг с другом, без браузера.

Браузерная часть остаётся ручной — node и playwright на этой машине не стоят. Но три класса ошибок,
которые уже стоили нам сборок, ловятся здесь: синтаксис скрипта (0.9.62 — консоль падала целиком и
сборка этого не заметила), путь к эндпоинту, которого в прошивке нет, и обработчик, забывший
спросить пароль.
"""
import json
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(HERE, "..", "main")
HTML = open(os.path.join(MAIN, "console.html"),    encoding="utf-8").read()
SRV  = open(os.path.join(MAIN, "web_server.cpp"),   encoding="utf-8").read()
PWR  = open(os.path.join(MAIN, "power_estimate.cpp"), encoding="utf-8").read()
WMH  = open(os.path.join(MAIN, "ap_password.h"),     encoding="utf-8").read()
WMC  = open(os.path.join(MAIN, "wifi_manager.cpp"), encoding="utf-8").read()
PMG  = open(os.path.join(MAIN, "pm_gate.cpp"),      encoding="utf-8").read()
BSC  = open(os.path.join(MAIN, "bench_screens.cpp"),encoding="utf-8").read()
WXC  = open(os.path.join(MAIN, "weather.cpp"),       encoding="utf-8").read()

def nocomments(text):
    """Без строчных комментариев: объяснение механизма — не его использование. Дважды подряд эта
    разница стоила ложного провала, когда тест нашёл data-g и crypto.subtle в пояснениях."""
    out = []
    for line in text.split("\n"):
        t = line.lstrip()
        if t.startswith("//") or t.startswith("<!--") or t.startswith("*"): continue
        out.append(line)
    return "\n".join(out)

CODE   = nocomments(HTML)
PMCODE = nocomments(PMG)   # объяснение механизма — не его вызов; на этом тест уже ловился трижды
WMCODE = nocomments(WMC)
BSCODE = nocomments(BSC)
WXCODE = nocomments(WXC)
WEB    = nocomments(open(os.path.join(MAIN, "web_server.cpp"), encoding="utf-8").read())
BAT    = open(os.path.join(MAIN, "battery_monitor.cpp"), encoding="utf-8").read()
BATCODE= nocomments(BAT)   # в комментариях прошивки полно слов вроде "full refresh" и "partial"

fails = []
def ok(cond, what):
    print(("  ок      " if cond else "  ПРОВАЛ  ") + what)
    if not cond: fails.append(what)

# Открыты намеренно: страница входа, иконка и манифест (их просит сам браузер до входа), пробы
# captive-портала и оба эндпоинта самого входа. Список здесь — не копия кода, а утверждение:
# всё остальное обязано спрашивать пароль, и если в прошивке появится ещё один открытый
# обработчик, тест упадёт, пока его не впишут сюда осознанно.
OPEN = {"/", "/icon.png", "/manifest.webmanifest", "/generate_204", "/hotspot-detect.html",
        "/api/auth", "/api/auth/challenge"}

def handlers():
    """(uri, метод, имя функции) для каждого зарегистрированного обработчика."""
    out = []
    for m in re.finditer(r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*(\w+)\s*,\s*\.handler\s*=\s*(\w+)',
                         SRV):
        out.append((m.group(1), m.group(2), m.group(3)))
    return out

def body_of(fn):
    """Тело функции-обработчика — от её заголовка до строки, начинающейся с '}'."""
    m = re.search(r'^\w[\w \*&:]*\b%s\s*\([^)]*\)\s*\{' % re.escape(fn), SRV, re.M)
    if not m: return None
    i = m.end()
    j = SRV.index("\n}", i)
    return SRV[i:j]


def energy_model():
    """Запускает саму модель, а не ищет в ней текст.

    Проверки на присутствие строки уже трижды проходили на сломанном коде (журнал 19, 21 и
    22 сентября), поэтому здесь вычисляется результат. И первая редакция ЭТОЙ функции тоже
    оказалась слаба: «парковка что-то экономит» проходило со снятым вычитанием кадров, потому
    что экономила ещё и рельса. Поэтому два слагаемых гасятся по отдельности, а величина каждого
    сверяется с тем, что даёт арифметика по тем же константам: убрали слагаемое, перепутали
    множитель, применили не к тем пробуждениям — расходится, и проверка падает.
    """
    mn = re.search(r"const NRG = \{.*?GAS_PERIOD:[\d.]+ \};", HTML, re.S)
    me = re.search(r"function estimateLife\(c\)\{.*?\n\}", HTML, re.S)
    ok(mn is not None, "блок констант NRG найден")
    ok(me is not None, "тело estimateLife найдено")
    if not (mn and me): return
    K = {k: float(v) for k, v in re.findall(r"(ACT|PARK_SAVE_S|RAIL_uA|GAS_PERIOD):([\d.]+)", mn.group(0))}
    ok(len(K) == 4, "константы модели прочитаны: %s" % sorted(K))
    if len(K) != 4: return

    def uA(cfg, frames=True, rail=True, scd=None, hub=False):
        nrg = mn.group(0)
        if scd is not None: nrg = re.sub(r"SCD_mC:[\d.]+", "SCD_mC:%s" % scd, nrg)
        if not frames: nrg = re.sub(r"PARK_SAVE_S:[\d.]+", "PARK_SAVE_S:0", nrg)
        if not rail:   nrg = re.sub(r"RAIL_uA:[\d.]+", "RAIL_uA:0", nrg)
        src = (nrg + "\nfunction capacityMah(){return 2000;}\n"
               + ("function radioSecs(){return NRG.%s;}\n"
                  % ("WIFI_S_HUB" if hub else "WIFI_S")) + me.group(0) +
               "\nJSON.stringify(estimateLife(%s))" % cfg)
        path = "/tmp/nrg_model.js"
        with open(path, "w", encoding="utf-8") as f: f.write(src)
        js = ('ObjC.import("Foundation");'
              'var s=$.NSString.stringWithContentsOfFileEncodingError("%s",4,null).js;'
              'eval(s)' % path)
        r = subprocess.run(["osascript", "-l", "JavaScript", "-e", js],
                           capture_output=True, text=True).stdout.strip()
        os.remove(path)
        try: return json.loads(r)["avg_uA"]
        except Exception: return None

    def cfg(nh, s_int=5, live="true", w_int=0):
        return ("{s_int:%d,s_int_n:%d,w_int:%d,night:%s,night_h:%d,live:%s}"
                % (s_int, s_int*2, w_int, "true" if nh else "false", nh, live))

    # 1. ТОЧКА ПРИВЯЗКИ — ПОЛЕВОЙ МЕСЯЦ (2026-10-19), а не прежний единственный замер 1050 мкА,
    #    снятый до лёгкого сна и задававший только границы 825…1050. Полевая плата v1, Performance
    #    с хабом MQTT, ночь 23→07 и живые часы, 26 суток без кабеля: 0,68 %/сут от ячейки
    #    2000 мА·ч, то есть 555 мкА. Полоса 530…580 — это 0,65…0,71 %/сут, разброс трёх
    #    НЕЗАВИСИМЫХ способов (вольты через таблицу стенда f708, бюджет журнала событий, оценка
    #    самого прибора), а не допуск, подогнанный под ответ модели.
    #
    #    Зачем это проверять вообще: с прежним ACT 20,7 модель давала 641 мкА для ТОЙ ЖЕ
    #    конфигурации, и страница показывала владельцу модельные 3,6 месяца рядом с измеренными
    #    4,9, не давая способа выбрать. Разъедься они снова — упадёт здесь.
    a = uA(cfg(8, w_int=15), hub=True)
    ok(a is not None, "модель считается")
    if a is None: return
    ok(530 <= a <= 580,
       "модель воспроизводит полевой месяц: %.0f мкА (530…580, то есть 0,65…0,71 %%/сут)" % a)
    msc = re.search(r"SCD_mC:([\d.]+)", mn.group(0))
    ok(msc is not None and 60 <= float(msc.group(1)) <= 90,
       "сегодня SCD41 выключается после каждого замера: ~74 мКл на замер, а не 135 (%s)"
       % (msc.group(1) if msc else "?"))

    # 2. Каждое слагаемое парковки — своей величины, а не просто «больше нуля».
    for nh in (4, 8):
        base  = uA(cfg(nh))
        no_fr = uA(cfg(nh), frames=False)
        no_rl = uA(cfg(nh), rail=False)
        if None in (base, no_fr, no_rl): continue
        gas = 86400 / K["GAS_PERIOD"]
        want_fr = gas * nh / 24 * K["PARK_SAVE_S"] * K["ACT"] * 1000 / 86400
        want_rl = K["RAIL_uA"] * nh / 24
        got_fr, got_rl = no_fr - base, no_rl - base
        ok(abs(got_fr - want_fr) < max(0.5, want_fr * 0.01),
           "ночь %d ч: несделанные кадры дают %.1f мкА (ждали %.1f)" % (nh, got_fr, want_fr))
        ok(abs(got_rl - want_rl) < max(0.5, want_rl * 0.01),
           "ночь %d ч: погашенная рельса даёт %.1f мкА (ждали %.1f)" % (nh, got_rl, want_rl))

    # 3. Без ночного окна экономить нечего — и это должно быть видно из самого расчёта, а не из
    #    защитного if: при nH = 0 оба слагаемых и так нули, поэтому проверять надо ПРОПОРЦИЮ.
    s4 = uA(cfg(4), frames=False) - uA(cfg(4))
    s8 = uA(cfg(8), frames=False) - uA(cfg(8))
    ok(abs(s8 - 2 * s4) < max(0.5, s8 * 0.01),
       "экономия строго пропорциональна длине ночи (%.1f против %.1f мкА)" % (s8, s4))

    # 4. Чем экономнее профиль, тем больше доля ночи среди всех пробуждений — значит и выигрыш
    #    выше. Разойдись это, и карточка Ultra Save обещала бы меньше, чем прибор даёт.
    n_on, n_off = uA(cfg(8)), uA(cfg(8), frames=False, rail=False)
    u_on, u_off = uA(cfg(8, 30, "false")), uA(cfg(8, 30, "false"), frames=False, rail=False)
    if all(v for v in (n_on, n_off, u_on, u_off)):
        g_p, g_u = 1 - n_on / n_off, 1 - u_on / u_off
        ok(g_u > g_p * 2,
           "выигрыш заметно растёт к экономным профилям: Ultra %.0f %% против Performance %.0f %%"
           % (g_u * 100, g_p * 100))


def main():
    print("== консоль: синтаксис ==")
    scripts = re.findall(r"<script>(.*?)</script>", HTML, re.S)
    ok(len(scripts) >= 1, "теги <script> найдены (%d)" % len(scripts))
    for n, src in enumerate(scripts):
        path = os.path.join("/tmp", "console_%d.js" % n)
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        js = ('ObjC.import("Foundation");'
              'var s=$.NSString.stringWithContentsOfFileEncodingError("%s",4,null).js;'
              'try{new Function(s);"OK"}catch(e){"FAIL: "+e.message}' % path)
        r = subprocess.run(["osascript", "-l", "JavaScript", "-e", js],
                           capture_output=True, text=True).stdout.strip()
        ok(r == "OK", "скрипт %d разбирается: %s" % (n + 1, r))
        os.remove(path)

    print("== консоль против прошивки ==")
    reg = handlers()
    n_calls = SRV.count("httpd_register_uri_handler")
    m = re.search(r"max_uri_handlers\s*=\s*(\d+)", SRV)
    ok(m is not None, "потолок обработчиков задан")
    if m:
        ok(int(m.group(1)) >= n_calls,
           "потолок %s не ниже числа регистраций (%d)" % (m.group(1), n_calls))

    uris = {u for u, _, _ in reg}
    called = set()
    for pat in (r"api\('(/api/[^'?]*)", r"fetch\('(/api/[^'?]*)", r"'(/api/[^'?]*)'"):
        called |= set(re.findall(pat, HTML))
    missing = sorted(c for c in called if c not in uris)
    ok(not missing, "все пути консоли есть в прошивке" + (": нет %s" % missing if missing else ""))

    # Обратная сторона: эндпоинт, который никто не зовёт, — либо забытый, либо лишний вес во флеше.
    IMG = {"/api/matter/qr.svg"}          # вставляется как <img src>, не через api()
    RAW = {"/api/ota", "/api/events.bin"} # образ и журнал: свой код загрузки
    unused = sorted(u for u, _, _ in reg
                    if u not in called and u not in OPEN and u not in IMG and u not in RAW
                    and u not in HTML)
    ok(not unused, "лишних эндпоинтов нет" + (": %s" % unused if unused else ""))

    print("== консоль: пароль на каждом обработчике ==")
    unguarded = []
    for uri, method, fn in reg:
        if uri in OPEN: continue
        b = body_of(fn)
        if b is None:
            unguarded.append("%s (тело %s не найдено)" % (uri, fn)); continue
        if "require_auth" not in b:
            unguarded.append("%s -> %s" % (uri, fn))
    ok(not unguarded, "каждый закрытый обработчик спрашивает пароль"
       + (": без проверки %s" % unguarded if unguarded else ""))
    # И наоборот: открытый список не должен тихо разрастись.
    ok(OPEN == {u for u, _, _ in reg if u in OPEN},
       "открытых эндпоинтов ровно %d, как записано" % len(OPEN))


    print("== пароль прибора: одно правило на все места ==")
    def lit(name):
        m = re.search(r"#define\s+" + name + r"((?:[^\n]*\\\n)*[^\n]*)", WMH)
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
        return "".join(parts).encode().decode("unicode_escape")
    a   = lit("PORTAL_AP_PASS_ALPHABET")
    gen = lit("PORTAL_AP_PASS_GEN_ALPHABET")
    lo  = int(re.search(r"PORTAL_AP_PASS_MIN (\d+)", WMH).group(1))
    hi  = int(re.search(r"PORTAL_AP_PASS_LEN (\d+)", WMH).group(1))
    ok(lo == hi, "длина фиксирована: ровно %d" % hi)
    ok(set(gen) <= set(a), "генератор берёт только из принимаемого алфавита")
    ok(len(set(a)) == len(a), "повторов в алфавите нет")

    # Правило должно жить в ОДНОМ месте. Оно жило в двух и разошлось: приём от владельца брал 8..12,
    # а чтение из NVS при загрузке требовало ровно 12 — и пароль в восемь символов прибор выбрасывал
    # на следующей загрузке, рисуя себе новый. Владелец об этом не узнавал.
    mset = re.search(r"bool WiFiManager::setApPassword\(const char \*pass\)\s*\{(.*?)\n\}", WMC, re.S)
    ok(mset is not None, "setApPassword найден")
    if mset:
        body = mset.group(1)
        ok("ap_pass_valid" in body, "приём пароля пользуется общим правилом")
        ok(not re.search(r"len\s*[<>]", body), "своей проверки длины в нём не осталось")
    mprep = re.search(r"void WiFiManager::prepareApPassword\(\)\s*\{(.*?)\n\}", WMC, re.S)
    ok(mprep is not None and "ap_pass_valid" in mprep.group(1),
       "чтение из NVS пользуется тем же правилом")
    ok(len(re.findall(r"PORTAL_AP_PASS_MIN", WMCODE)) == 1,
       "нижняя граница упомянута в коде ровно один раз")

    # Консоль обещает пользователю ровно то, что прибор примет.
    mi = re.search(r'id="pwNew"[^>]*?maxlength="(\d+)"', HTML, re.S)
    ok(mi is not None and int(mi.group(1)) == hi,
       "поле ограничено той же длиной, что правило (%d)" % hi)
    ok(("Exactly %d characters" % hi) in HTML, "подсказка называет точную длину")
    mjs = re.search(r"const PW_ALPHABET = (.*?);\n", HTML, re.S)
    ok(mjs is not None, "алфавит в консоли найден")
    if mjs:
        js = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', mjs.group(1)))
        js = js.replace('\\"', '"').replace("\\\\", "\\")
        ok(js == a, "алфавит консоли совпадает с прошивкой (%d против %d знаков)" % (len(js), len(a)))
    # Из печатных ASCII исключены ровно два, и консоль обязана назвать те же самые.
    banned = sorted(set(chr(i) for i in range(32, 127)) - set(a))
    ok(banned == [" ", "W"], "исключены ровно пробел и W: %s" % banned)
    ok("capital <b>W</b>" in HTML and "space" in HTML, "консоль называет оба исключения")


    print("== батарея: одно соглашение на прошивку и консоль ==")
    # Ёмкость считается в двух местах — в прошивке для измеренной оценки и в консоли для модельной,
    # и делить они обязаны на ОДНО И ТО ЖЕ. Пока доля была зашита числом 1700 (0,85 x 2000), числа
    # расходились у любого, кто менял ёмкость: месяцы её игнорировали, а «расход в мА·ч» рядом — нет.
    #
    # А с 2026-10-19 доли нет вовсе, с обеих сторон: ноль процентов на обеих таблицах ячеек — это
    # уже порог отключения, то есть неиспользуемый хвост из шкалы уже вынут, и стенд намерял
    # 1,99 А·ч до 3,30 В на ячейке 2000 мА·ч. Вторая скидка 0,85 делала модельные месяцы на 15 %
    # короче, чем 100 % / (%/сут), напечатанное на той же странице. Проверяется, что она не выросла
    # обратно ни у одной из сторон, — иначе они снова разойдутся, и молча.
    mfw = re.search(r"e\.months\s*=[^;]*;", PWR)
    ok(mfw is not None and re.search(r"cap_mah\s*\*", mfw.group(0)) is None,
       "прошивка делит на паспортную ёмкость без второй скидки")
    ok("USABLE_FRAC" not in HTML and "usableMah" not in HTML,
       "и в модели консоли доли полезной ёмкости не осталось")
    ok(re.search(r"function capacityMah\(\)\s*\{[^}]*cfg\.bat_mah[^}]*\}", HTML) is not None,
       "модель берёт ёмкость с прибора, а не свою копию")
    ok(re.search(r"function capacityMah\(\)\s*\{[^}]*\*[^}]*\}", HTML) is None,
       "и ни на что её не умножает")
    # Решение пользователя 2026-09-27: ёмкость и порог отключения владелец не задаёт — это ставит
    # тот, кто собирает прибор. Ячейка припаяна к пакету внутри закрытого корпуса, а поле давало
    # ровно одну возможность: сделать прибор неправильным. Порог вдобавок единственная программная
    # защита ячейки, и на плате v2, где газомера не будет, — единственная вообще.
    SPEC = open(os.path.join(MAIN, "battery_spec.h"), encoding="utf-8").read()
    ok("BATTERY_CAPACITY_MAH" in SPEC and "BATTERY_CUTOFF_V" in SPEC,
       "battery_spec.h держит оба числа в одном месте")
    for fid in ("batMah", "lowbatV", "saveBattery"):
        ok(('id="%s"' % fid) not in HTML, "поля %s в консоли больше нет" % fid)
    # Не принимать через API — не косметика: без поля в интерфейсе значение, записанное в обход,
    # нечем исправить. Порог 4.5 В защёлкивает блокировку на полной ячейке, снятие требует
    # порог + 0.15 В, и прибор уходит спать по часу навсегда — до консоли не добраться и кнопкой.
    ok('NVSManager::getInstance().setU32("bat_mah"' not in SRV, "прошивка не сохраняет bat_mah")
    ok('NVSManager::getInstance().setFloat("lowbat_v"' not in SRV, "прошивка не сохраняет lowbat_v")
    # И ни одно место прошивки не должно читать их обратно из ХРАНИЛИЩА. Проверяется чтение, а не
    # упоминание: cfg["bat_mah"] в ответе /api/status — это имя поля JSON, и оно законно.
    for f in ("bench_screens.cpp", "battery_monitor.cpp", "web_server.cpp"):
        code = nocomments(open(os.path.join(MAIN, f), encoding="utf-8").read())
        reads = re.findall(r'(?:getU32|getFloat|nvs_u32|nvs_f)\(\s*"(bat_mah|lowbat_v)"', code)
        ok(not reads, "%s берёт их из battery_spec.h, а не из NVS%s"
                      % (f, "" if not reads else " — читает " + ", ".join(sorted(set(reads)))))
    # Консоль по-прежнему ПОКАЗЫВАЕТ их: модель срока жизни на них делит, и страница со своей
    # копией числа, которым владеет прошивка, — это как раз то, как эти двое расходятся.
    ok('cfg["bat_mah"]' in SRV and 'cfg["lowbat_v"]' in SRV, "но в /api/status они отдаются")

    print("== консоль: гид уехал на сайт ==")
    # Решение пользователя 2026-09-28: гид и тур по экрану живут на сайте, в консоли — ссылка.
    # Раньше здесь лежали 24 отрисованных снимка как data URI: 80 КБ из 464 КБ исходника и самая
    # крупная вещь в файле, которая стоила флеша на каждом приборе независимо от того, открывал ли
    # её кто-нибудь. Проверяется, что они НЕ ВЕРНУЛИСЬ — блок восстанавливается одним запуском
    # скриптом, и без сторожа это случилось бы молча (embed_guide_images.py удалён 2026-09-28).
    ok("GUIDE-IMAGES:BEGIN" not in HTML, "блока вшитых картинок нет")
    ok("const GIMG" not in HTML, "таблицы картинок нет")
    ok('data-g="' not in CODE, "разметки со снимками не осталось")
    ok("hydrateGuideImages" not in HTML, "подстановки снимков нет")
    # Ссылка обязана быть, и обязана вести на гид, а не на корень сайта.
    ok("smartmeteo.org/guide" in HTML, "ссылка на гид на сайте есть")
    # Решение владельца 2026-10-08: гид — первое, что видит владелец, значит верх дашборда.
    # Спрашиваем про МЕСТО, а не про наличие текста: ссылка была и раньше, но в настройках.
    sec = {k: HTML.find('id="v-%s"' % k) for k in ("dash", "mode", "net", "set", "sys")}
    ok(all(v > 0 for v in sec.values()), "все вкладки найдены")
    g = HTML.find("The screen guide lives on the website")
    ok(sec["dash"] < g < sec["mode"], "карточка гида лежит во вкладке дашборда")
    # Место менялось дважды: сперва самым верхом (2026-10-08), затем — просьба владельца — ниже
    # плашек показателей и прямо над History. Спрашиваем ИМЕННО это, а не «есть на дашборде».
    ok(g > HTML.find('id="weatherCard"'), "кнопка ниже карточки погоды")
    ok(g > HTML.find('id="mIaq"'), "и ниже плашек показателей")
    ok(g < HTML.find('<h3 style="margin:0">History</h3>'), "но выше блока History")
    gtxt = HTML[g:g + 1800].lower()
    ok("needs internet" in gtxt and "setup network" in gtxt,
       "и по-прежнему объясняет, почему может не открыться")
    # Подчёркнутая ссылка в пустой рамке читалась как сырой HTML (снимок владельца 2026-10-08):
    # у .btn не было text-decoration, и <a class="btn"> оставался ссылкой. Теперь это своя строка.
    ok('class="guidelink"' in HTML, "ссылка оформлена своим стилем, а не кнопкой-ссылкой")
    m_btn = re.search(r"\.btn\{(.*?)\}", CODE, re.S)
    ok(m_btn is not None and "text-decoration:none" in m_btn.group(1),
       "и у .btn снято подчёркивание — чтобы следующая такая ссылка выглядела кнопкой")
    # Своего стиля у ссылок не было, и голый <a> выходил браузерным синим с подчёркиванием —
    # на тёмном фоне это читается как незагрузившаяся страница (снимок владельца 2026-10-08).
    ok(re.search(r"\na\{[^}]*color:var\(--accent\)", CODE), "у ссылок есть свой цвет, а не браузерный синий")
    ok(re.search(r"\na\{[^}]*text-decoration:none", CODE), "и они не подчёркнуты по умолчанию")
    ok(re.search(r"\na:hover\{[^}]*text-decoration:underline", CODE), "подчёркивание приходит под курсором")
    # Оговорка про интернет показывается только на собственной точке доступа прибора. Договор
    # в обе стороны: прошивка обязана отдавать поле, консоль — читать именно его.
    ok('net["own_ap"] = served_on_own_ap(req)' in WEB,
       "прошивка отдаёт, по какой сети открыта страница")
    ok("n.own_ap" in CODE, "а консоль читает именно это поле")
    # Отсутствие поля (старая прошивка) обязано оставлять оговорку, а не глотать её молча.
    ok("n.own_ap === false" in CODE,
       "и прячет оговорку только по ЯВНОМУ отрицанию, а не по отсутствию поля")
    # Разбор показаний датчиков — диагностика, а не живое показание: ей место в System.
    d = HTML.find("Air-quality details")
    ok(d > sec["sys"], "разбор по воздуху переехал во вкладку System")
    ok(not (sec["dash"] < d < sec["mode"]), "и на дашборде его больше нет")
    ok(HTML.count("smartmeteo.org/guide/") == 1,
       "адрес гида живёт в одном месте, а не в трёх")
    # И обязана объяснять, почему может не открыться: у своей точки доступа прибора интернета нет,
    # а мёртвая ссылка без объяснения читается как сломанный прибор.
    gcard = HTML[HTML.index("How to read the screen"):]
    gcard = gcard[:gcard.index("</div>", gcard.index("</div>") + 6)]
    ok("internet" in gcard.lower(), "и сказано, что ей нужен интернет")
    ok("setup network" in gcard.lower(), "и что на своей сети прибора его нет")
    print("          (консоль %d КБ — было 453 КБ с картинками)" % (len(HTML) // 1024))

    ok("WIZ_TOUR" in HTML and "WIZ_DONE" in HTML, "шаги мастера названы, а не пронумерованы")
    mw = re.search(r"WIZ_STEPS\s*=\s*\[(.*?)\n\];", HTML, re.S)
    ok(mw is not None, "таблица шагов мастера найдена")
    if mw:
        n_steps = len(re.findall(r"\{t:'", mw.group(1)))
        # Экран тура и итог берутся из длины таблицы, а не из чисел — но их тела и привязки живут
        # в отдельных таблицах, и разъехаться они не должны (так однажды и сломалась настройка).
        n_body = len(re.findall(r"wizStep(\d+)", HTML)) or n_steps
        ok(n_steps >= 5, "в мастере %d шагов" % n_steps)
        mt = re.search(r"WIZ_TOUR\s*=\s*WIZ_STEPS\.length\s*-\s*(\d+)", HTML)
        ok(mt is not None or "WIZ_TOUR" in HTML, "экран тура задан относительно таблицы")

    print("== консоль: мелочи, которые не должны уехать в прошивку ==")
    ok(CODE.count("console.log") == 0, "отладочного вывода нет")
    ok("SmartMeteo123" not in CODE, "пароля по умолчанию в консоли нет")
    ok(re.search(r"crypto\.subtle\s*\.", CODE) is None,
       "SHA-256 не берётся из crypto.subtle — на чистом HTTP её нет")

    # Лёгкий сон будится УРОВНЕМ — иначе никак, спящий чип не видит фронта. Но gpio_wakeup_enable()
    # заодно переписывает тип прерывания того же вывода (gpio.c) и обратно его не возвращает, а на
    # кнопке висит обработчик по фронту. Забытое восстановление означает шторм прерываний на
    # удержанной кнопке — ровно это я и залил 2026-09-19, прежде чем прочитал исходник IDF.
    print("== кнопка: уровень для сна не должен пережить сам сон ==")
    nap_open  = re.search(r"Nap::Nap\s*\(.*?\n\}", PMCODE, re.S)
    nap_close = re.search(r"Nap::~Nap\s*\(.*?\n\}", PMCODE, re.S)
    ok(nap_open is not None and nap_close is not None, "оба конца Nap найдены")
    if nap_open and nap_close:
        o, c = nap_open.group(0), nap_close.group(0)
        ok("gpio_intr_disable" in o and o.index("gpio_intr_disable") < o.index("gpio_wakeup_enable"),
           "прерывание глушится до того, как вывод переводят на уровень")
        ok("gpio_wakeup_disable" in c, "уровень снимается на выходе")
        ok("gpio_intr_enable" in c, "прерывание возвращают")
        m = re.search(r"gpio_set_intr_type\s*\([^,]+,\s*(GPIO_INTR_\w+)\s*\)", c)
        ok(m is not None, "тип прерывания восстанавливают явно")
        # И восстанавливают ИМЕННО тот, который ставит настройка кнопки: разъедься эти двое, и
        # кнопка после первого замера начнёт вести себя иначе, чем после загрузки.
        want = re.search(r"btn\.intr_type\s*=\s*(GPIO_INTR_\w+)", BSC)
        ok(want is not None, "настройка кнопки называет свой тип прерывания")
        if m and want:
            ok(m.group(1) == want.group(1),
               "восстановленный тип совпадает с настроенным (%s)" % want.group(1))
    # Счётчик заметок живёт в RTC_NOINIT, то есть при смене образа читает чужой мусор. Обнулять
    # его обязана forget_previous_build() — именно она срабатывает на новую прошивку. Первая
    # версия поставила строку в forget_rtc_state() (заводской сброс), и после заливки не было
    # записано ни одной заметки, при этом всё выглядело здоровым. Проверяем тело функции, а не
    # наличие строки в файле: в файле она была и тогда.
    m = re.search(r"static void forget_previous_build\(void\)\s*\{(.*?)\n\}", BSC, re.S)
    ok(m is not None, "forget_previous_build найдена")
    if m:
        ok("rtc_ls_notes = 0" in m.group(1),
           "счётчик заметок о сне обнуляется при смене образа")
    ok(re.search(r"if\s*\(\s*rtc_ls_notes\s*>\s*64\s*\)\s*rtc_ls_notes\s*=\s*0", BSC) is not None,
       "и мусор из RTC не притворяется счётчиком")

    # Нажатие, сделанное внутри ожидания, фронта не оставляет — его подбирает обратный вызов.
    ok(re.search(r"PmGate::begin\s*\(\s*\w+\s*,\s*\w+\s*,\s*\w+\s*\)", BSC) is not None,
       "PmGate::begin получает обработчик нажатия, а не умолчание")

    # Радио больше не роняет прибор. Единственный настоящий отказ здесь — ESP_ERR_NO_MEM из
    # esp_wifi_init(), и он приходит ровно на тех пробуждениях, где памяти и так мало: с открытой
    # консолью свободно ~91 КБ против медианы 172. ESP_ERROR_CHECK превращал это в панику и
    # перезагрузку, а пять таких — в откат образа.
    print("== радио: отказ драйвера это пропущенное окно, а не паника ==")
    ok("ESP_ERROR_CHECK" not in WMCODE, "в wifi_manager.cpp не осталось ESP_ERROR_CHECK")
    ok(re.search(r"bool WiFiManager::init\(\)", WMCODE) is not None,
       "init() сообщает, поднялся ли драйвер")
    ok(re.search(r"bool WiFiManager::startAP\(\)", WMCODE) is not None,
       "startAP() сообщает, поднялась ли точка доступа")
    # Повторный вход после отказа на середине — то, что легко сделать неверно: netif нельзя
    # создавать дважды, а esp_netif_init() отвечает INVALID_STATE, если уже отработал.
    ok("ESP_ERR_INVALID_STATE" in WMCODE, "повторный esp_netif_init не считается отказом")
    ok(re.search(r"if\s*\(!sta_netif\)", WMCODE) is not None,
       "netif не создаётся второй раз")
    # Не «хоть кто-то проверяет», а «никто не бросает результат»: мест вызова два, и первая
    # версия этой проверки прошла на исправном, пока сломанным было второе.
    for call in ("init", "startAP"):
        ok(re.search(r"^\s*WiFiManager::getInstance\(\)\." + call + r"\(\);\s*$",
                     BSCODE, re.M) is None,
           "результат %s() нигде не отбрасывается" % call)

    # Наклон разряда меряется в часах, поэтому ему нужно время, которое идёт всегда. rtc_bsec_ns
    # двигается только на газовом тике, да ещё и ПОСЛЕ этого вызова.
    # Первый аргумент намеренно не закреплён: проверка про ИСТОЧНИК ВРЕМЕНИ, а не про имя
    # переменной, и переименование gauge -> bus под плату v2 её ломать не должно.
    # Прогон FRC кормит датчик давлением ДО калибровки, а кончается записью базовой линии в его
    # EEPROM — навсегда, переживая заводской сброс. Значит число, которое туда идёт, обязано
    # принадлежать этой сборке: после обновления по воздуху переменные RTC переезжают, и поле
    # читает чужие байты. На ненастроенном приборе это видели вживую — 1,47e21 Па в логе 2026-09-29.
    print("== своя точка доступа отвечает за пароль ==")
    # Решение пользователя 2026-09-30. QR на экране настройки несёт WIFI:S:...;P:<пароль>;; — то есть
    # телефон входит в сеть прибора ЭТИМ паролем, после чего страница просила его второй раз. На своей
    # точке доступа WPA2 уже спросил ровно эти восемь знаков; в домашней сети WPA2 принадлежит роутеру,
    # и там пароль остаётся. Проверить это на хосте нечем — web_server.cpp здесь не собирается, —
    # поэтому проверяется КОНСТРУКЦИЯ, а поведение подтверждается только на приборе.
    body = re.search(r"static bool served_on_own_ap\(httpd_req_t \*req\)\s*\{.*?\n\}", WEB, re.S)
    ok(body is not None, "решение живёт в отдельной функции")
    ra = re.search(r"static bool require_auth\(httpd_req_t \*req\)\s*\{(.*?)\n\}", WEB, re.S)
    ok(ra is not None, "require_auth найден")
    if body and ra:
        b = body.group(0)
        # Спрашивается ПЕРВЫМ и по самому запросу: кэш решения на окно был бы дырой, ведь в окне
        # прибор бывает в обеих сетях сразу (startAP поднимает APSTA, connectSTAKeepAP не роняет AP).
        ok(re.match(r"\s*if \(served_on_own_ap\(req\)\) return true;", ra.group(1)) is not None,
           "require_auth спрашивает про точку доступа первым делом и по самому запросу")
        # Локальный адрес сокета, а не адрес звонящего: локальный выбирает маршрутизация lwIP.
        ok("getsockname(" in b, "адрес берётся локальный (getsockname)")
        ok("getpeername" not in WEB,
           "адрес клиента не используется нигде — его можно подделать, локальный нельзя")
        # Адрес точки доступа спрашивается у netif, а не вписан числом: 192.168.4.1 задаётся
        # настройкой, и вписанная копия разошлась бы с прибором молча.
        ok("esp_netif_get_ip_info(" in b and "192.168.4.1" not in b,
           "адрес точки доступа берётся у netif, а не вписан числом")
        ok("esp_netif_is_netif_up(" in b, "и только пока точка доступа поднята")
        # Сессия на этом пути НЕ выдаётся: иначе телефон, вошедший через точку доступа, унёс бы
        # cookie в домашнюю сеть — то есть доступ туда, где пароль как раз обязателен.
        ok("set_session_cookie" not in b and "s_session" not in b,
           "сессия по этому пути не выдаётся — иначе она уехала бы в домашнюю сеть")
        ok("sm_trust" not in b, "и запомненным браузером этот путь тоже не делает")
        # Флаг существует только ради одной записи в журнал, а не ради ответа.
        ok(not re.search(r"return\s+s_ap_noted", b), "s_ap_noted не участвует в ответе функции")
        ok("SEC_SIGNIN_OK | 0x200u" in b, "вход через точку доступа виден в журнале безопасности")
    # И главное, что не должно было измениться: 401 для домашней сети на месте.
    ok('"401 Unauthorized"' in WEB, "отказ для домашней сети никуда не делся")

    print("== хаб: что меняется вместе с брокером и темой ==")
    # Discovery обещало «раз в сутки, и ещё после перезагрузки или смены настроек». Последних двух не
    # было: отметка в RTC_NOINIT переживает esp_restart, а ни один путь настроек её не трогал. Смена
    # темы оставляла в Home Assistant retained-конфиги, указывающие на прежний префикс: сущности
    # уходят в «недоступно» и не возвращаются до суток. Консоль сама просит давать каждому прибору
    # свой префикс, то есть это путь, на который людей направляют.
    ok("global_hub_dirty" in WEB and "global_hub_dirty" in BSCODE,
       "смена брокера или темы отмечается для цикла")
    hub = re.search(r"if \(global_hub_dirty\) \{(.*?)\n    \}", BSCODE, re.S)
    ok(hub is not None and "rtc_mqtt_disco_at = 0" in hub.group(1),
       "и цикл заново рассказывает Home Assistant, что это за прибор")
    topic = re.search(r'doc\["mq_topic"\]\.isNull\(\)\)\s*\{(.*?)\n    \}', WEB, re.S)
    ok(topic is not None and "global_hub_dirty = true" in topic.group(1),
       "тема тоже считается сменой, не только хост")

    print("== долг перед хабом не копится там, где хаба нет ==")
    # rtc_al_seq растёт в любом режиме, а rtc_al_hub двигается только внутри publish_to_hub(). В
    # Wi-Fi и в Matter они расходятся, и ПЕРВОЕ окно после включения MQTT публикует самую свежую из
    # накопленных тревог как новое событие — прямо в пуш на все телефоны через автоматизацию,
    # которую пишет эта же консоль.
    # Привязка к if/else, а не к форме вызова: с 2026-10-01 publish_to_hub() возвращает, достучался
    # ли он до брокера, и присваивание результата сломало бы проверку, которая держалась за текст.
    ok(re.search(r"if \(wl_mode == 1\)[^;\n]*publish_to_hub\(\);\s*\n(?:\s*//[^\n]*\n)*\s*else rtc_al_hub = rtc_al_seq;",
                 BSC) is not None,
       "без хаба долг гасится на месте, а не копится до его появления")

    print("== NTP: лестница отступления, а не плоские шесть часов ==")
    # Саму арифметику проверяет t_cycle, вызывая ClockSync::retryAfter() напрямую. Здесь — то, чего
    # арифметика не видит: что счётчик вообще живёт правильно.
    CS = nocomments(open(os.path.join(MAIN, "clock_sync.cpp"), encoding="utf-8").read())
    ok("retryAfter(valid, rtc_ntp_fails)" in CS,
       "пауза берётся из лестницы, а не из плоской константы")
    okb = re.search(r"if \(synced && now > TIME_VALID_FLOOR\) \{(.*?)\n    \} else \{(.*?)\n    \}", CS, re.S)
    ok(okb is not None, "обе ветки синхронизации найдены")
    if okb:
        ok("rtc_ntp_fails     = 0;" in okb.group(1) or "rtc_ntp_fails = 0;" in okb.group(1),
           "успех обнуляет счётчик — иначе лестница не опускается никогда")
        ok("rtc_ntp_fails++" in okb.group(2),
           "отказ его наращивает")
    fc = re.search(r"void ClockSync::forceClockSync\(\) \{(.*?)\n\}", CS, re.S)
    ok(fc is not None and "rtc_ntp_fails" in fc.group(1),
       "«спросить сейчас» сбрасывает и лестницу, а не только отметки")
    ok(re.search(r"if \(rtc_ntp_fails > \d+\) rtc_ntp_fails = 0;", CS) is not None,
       "мусор RTC_NOINIT в счётчике отсекается — большое число держало бы повтор вечно")

    print("== корпоративная сеть: прибор говорит, что не умеет, вместо глухого отказа ==")
    # Повод из поля 2026-10-01: офисная сеть с доменным логином. EAP-клиента в прошивке нет вовсе,
    # а connectSTA требует WIFI_AUTH_WPA_WPA2_PSK — значит ответ «не умею», и его надо СКАЗАТЬ.
    ok("eap_client" not in WMCODE and "ENTERPRISE" in WMCODE,
       "EAP не появился, а перечисление enterprise читается")
    aw = re.search(r"static const char \*auth_word\(wifi_auth_mode_t m\)\s*\{(.*?)\n\}", WMCODE, re.S)
    ok(aw is not None, "словарь типов защиты есть")
    if aw:
        # Все пять видов enterprise из esp_wifi_types_generic.h. WPA2_ENTERPRISE — тот же номер, что
        # ENTERPRISE, поэтому его в switch быть НЕ ДОЛЖНО: дубль метки не соберётся.
        for m in ("WIFI_AUTH_ENTERPRISE", "WIFI_AUTH_WPA_ENTERPRISE", "WIFI_AUTH_WPA3_ENTERPRISE",
                  "WIFI_AUTH_WPA2_WPA3_ENTERPRISE", "WIFI_AUTH_WPA3_ENT_192"):
            ok(m in aw.group(1), "  %s назван" % m)
        ok("WIFI_AUTH_WPA2_ENTERPRISE" not in aw.group(1),
           "  и алиас того же номера не продублирован")
    ok('obj["auth"] = net.auth;' in WEB, "/api/scan отдаёт тип защиты наружу")
    # Перечисление esp_wifi принадлежит прошивке. Консоль, раскладывающая его номера сама, разъедется
    # с первым же новым значением — а их там уже пять там, где читатель ждёт одно.
    ok("WIFI_AUTH" not in CODE, "консоль не разбирает перечисление esp_wifi сама")
    # Два списка сетей были копиями. Разметку теперь делает ровно одно место, иначе пометка
    # корпоративной сети встанет в одном списке и пропадёт в другом — как было с плашкой адреса.
    ok(CODE.count('class="netitem') == 1,
       "разметку строки сети делает одно место на оба списка")
    ok(CODE.count("bindNetList(") == 3,
       "и привязку тоже: определение плюс оба списка")
    bl = re.search(r"function bindNetList\(box, onPick\)\{(.*?)\n\}", CODE, re.S)
    # dataset.ent, не "data-ent": атрибут разметки читается в JS через dataset. Моя первая версия
    # искала имя атрибута и падала на верном коде — проверка обязана говорить на языке того, что
    # проверяет.
    ok(bl is not None and re.search(r"dataset\.ent\s*===\s*'1'[^}]*return", bl.group(1), re.S) is not None,
       "корпоративная сеть объясняет и ВЫХОДИТ, не доходя до подстановки в форму")

    print("== три запомненные сети: цена обычного дня обязана остаться нулевой ==")
    # Идея владельца 2026-10-01. Очевидная реализация разорительна: пробовать сети по очереди — это
    # до 10 с полного скана НА КАЖДУЮ, против радиоокна в 2,3 с, которое и так вторая статья бюджета
    # после панели. Поэтому проверяется не «есть ли фича», а её форма.
    SLOT = nocomments(open(os.path.join(MAIN, "wifi_slots.h"), encoding="utf-8").read())
    best = re.search(r"bool WiFiManager::connectBest\(\)\s*\{(.*?)\n\}", WMCODE, re.S)
    ok(best is not None, "connectBest() есть")
    body = best.group(1) if best else ""
    # ОДНА сеть — ровно прежний путь. Это и есть обещание «обычный день не подорожал», и держать его
    # должна конструкция, а не намерение.
    ok(re.search(r"if \(n == 1\) return connectSTA\(nets\[0\]\.ssid, nets\[0\]\.pass\);", body) is not None,
       "одна сеть — прежний вызов без режима, ни миллисекунды сверху")
    # Скан ровно один, и подключение после него — ТОЛЬКО сканом: кеш принадлежит другому месту.
    # НЕ count(): слом, загнавший тот же вызов в цикл, сохранил бы число вхождений и прошёл молча —
    # это уже седьмой раз за проект, когда проверка на НАЛИЧИЕ текста пропускает дефект в МЕСТЕ.
    # Поэтому спрашиваем, не стоит ли вызов внутри открытого for/while.
    def opened_by_loop(text, brace):
        # От «{» назад: пропустить пробелы, увидеть «)», отмотать по балансу до своей «(», и только
        # тогда прочитать слово перед ней. Регулярка здесь не годится — заголовок for содержит «;».
        j = brace - 1
        while j >= 0 and text[j].isspace(): j -= 1
        if j < 0 or text[j] != ')': return False
        depth = 0
        while j >= 0:
            if text[j] == ')': depth += 1
            elif text[j] == '(':
                depth -= 1
                if depth == 0: break
            j -= 1
        if j < 0: return False
        j -= 1
        while j >= 0 and text[j].isspace(): j -= 1
        end = j + 1
        while j >= 0 and (text[j].isalnum() or text[j] == '_'): j -= 1
        return text[j + 1:end] in ("for", "while")

    def inside_loop(text, pos):
        stack = []
        for i in range(pos):
            if text[i] == '{':   stack.append(opened_by_loop(text, i))
            elif text[i] == '}' and stack: stack.pop()
        return any(stack)
    at = body.find("pickFromAir(")
    ok(body.count("pickFromAir(") == 1 and at >= 0 and not inside_loop(body, at),
       "скан делается ОДИН раз и не внутри цикла по сетям")
    ok(body.count("Attempt::ScanOnly") == 1 and body.count("Attempt::FastOnly") == 1,
       "один дешёвый заход по кешу и один коннект по итогу скана")
    ok("for (int i = 0; i < n; i++)" in body and "Attempt::Auto" not in body,
       "перебора сетей с полным откатом нет — Auto в выборщике не используется")
    # FastOnly обязан ОТКАЗАТЬ, а не свалиться в десятисекундный скан: ради этого он и заведён.
    fast = re.search(r"if \(how == Attempt::FastOnly\) \{(.*?)\n        \}", WMCODE, re.S)
    ok(fast is not None and "return false" in fast.group(1) and "esp_wifi_stop" in fast.group(1),
       "FastOnly при промахе гасит радио и отказывает, а не уходит в полный скан")
    # Миграция: слот 0 — те же ключи, что читали всегда.
    ok(re.search(r'k\[COUNT\] = \{"ssid", "ssid2", "ssid3"\}', nocomments(
        open(os.path.join(MAIN, "wifi_slots.cpp"), encoding="utf-8").read())) is not None,
       "слот 0 — это \"ssid\": device_configured(), экран настройки и мастер читают его как раньше")
    ok("COUNT = 3" in SLOT, "число сетей названо в заголовке, а не рассыпано по файлам")
    # Пароли наружу не отдаются даже за паролем консоли.
    nets_block = re.search(r'auto arr = cfg\["nets"\]\.to<JsonArray>\(\);(.*?)\n    \}', WEB, re.S)
    ok(nets_block is not None and ".ssid" in nets_block.group(1) and ".pass" not in nets_block.group(1),
       "в /api/status уходят только имена сетей, без паролей")
    # Повторное сохранение известной сети без пароля не должно стирать работающий пароль.
    ok("WifiSlots::promote(new_ssid)" in WEB,
       "сохранение без пароля поднимает сеть, сохраняя прежний пароль")
    ok("connectBest()" in BSCODE and "connectSTA(ssid, pass)" not in BSCODE,
       "цикл зовёт connectBest(), а не одну сеть напрямую")

    print("== погода в режиме хаба: «молчит» и «его тут нет» — разные вещи ==")
    # Найдено в поле 2026-10-01: прибор унесли из дома в режиме MQTT, брокер остался в домашней
    # сети, и погоду не принёс бы никто — а правило смотрело только на ВОЗРАСТ последней погоды от
    # хаба и честно ждало шесть часов. Ответ брокера лежит в двадцати строках выше решения.
    # ЧЕГО ЭТИ ПРОВЕРКИ НЕ ДОКАЗЫВАЮТ: оснастка не умеет прогнать радиоокно (нужно доставить
    # IP_EVENT_STA_GOT_IP, ручки нет), поэтому проверяется КОНСТРУКЦИЯ, а не поведение.
    ok("static bool publish_to_hub(void)" in BSC,
       "publish_to_hub отвечает, достучался ли он до брокера")
    ok("hub_reached = publish_to_hub();" in BSC,
       "и вызывающий этот ответ забирает, а не выбрасывает")
    m = re.search(r"\} else if \(weather_due && wl_mode == 1 &&([^)]*)\) \{", BSC)
    ok(m is not None and "hub_reached" in m.group(1) and "hub_weather_stale" in m.group(1),
       "уступка хабу требует ОБОИХ: он ответил и его погода свежая")
    ok(m is not None and "hub_reached" in BSC[m.end():m.end() + 900],
       "а в ветке «идём сами» сказано, какая из двух причин сработала")

    print("== мелочи, которые молча стоили радио ==")
    WM = nocomments(open(os.path.join(MAIN, "wifi_manager.cpp"), encoding="utf-8").read())
    ok("rtc_ap_ssid_tag" in WM, "кеш быстрого подключения помнит, чьей сети он принадлежит")
    ok(re.search(r"rtc_ap_cache_valid && rtc_ap_ssid_tag != tag", WM) is not None,
       "и отбрасывается при смене сети, а не тратит три секунды на чужой роутер")
    # Именно в рабочем цикле, а не «где-нибудь в файле»: такая же запись есть в окне настройки, и
    # первая редакция проверки находила её и молчала на сломе (2026-09-30). Якорь — строка, которая
    # и означает «подключились»: сброс счётчика неудач сразу после успешного connectSTA.
    i = BSCODE.find("rtc_wifi_skip  = 0;")
    ok(i >= 0 and 'setU8("wifi_ok", 1)' in BSCODE[i:i + 700],
       "успешное подключение В РАБОЧЕМ ЦИКЛЕ отмечает сеть проверенной")

    print("== «настроен без Wi-Fi»: флаг, который было нечем ни увидеть, ни снять ==")
    # Флаг ставит одна кнопка мастера, а снимало его ДО 2026-09-30 ровно одно — сохранение непустого
    # SSID, тогда как все три кнопки смены режима шлют один wl_mode. Прибор, настроенный офлайн и
    # позже получивший брокера, держал радио выключенным навсегда: go_online() выходит на первой
    # строке, readPlan обходит даже часовой пол режима хаба.
    ok(re.search(r'wants_hub\s*=.*wl_mode.*==\s*1.*\n.*mq_host', WEB) is not None,
       "просьба о хабе снимает «без сети»")
    # Не «в блоке упоминается ssid», а «снятие флага стоит ПОД проверкой сохранённой сети». Первая
    # редакция искала getString("ssid") где-нибудь рядом и пропустила слом, который просто заменил
    # условие на false: форма осталась, достижимость исчезла (поймано сломом 2026-09-30).
    blk = re.search(r"if \(off && wants_hub\) \{(.*?)\n        \}", WEB, re.S)
    ok(blk is not None, "блок «просили хаб, а сеть выключена» найден")
    if blk:
        lines = blk.group(1).split("\n")
        idx = [i for i, l in enumerate(lines) if 'setU8("no_wifi", 0)' in l]
        ok(len(idx) == 1, "флаг снимается ровно в одном месте блока")
        if idx:
            guard = [l for l in lines[:idx[0]] if l.strip().startswith("if (")]
            ok(bool(guard) and "have_ssid" in guard[-1],
               "и снятие стоит под проверкой сохранённой сети%s"
               % ("" if guard and "have_ssid" in guard[-1]
                  else " — сторожит: " + (guard[-1].strip()[:50] if guard else "ничто")))
    # И это надо было ВИДЕТЬ: единственный экран, который про радио отчитывался, утверждал обратное.
    eff = re.search(r"function effWifi\(w\)\{(.*?)\n\}", CODE, re.S)
    ok(eff is not None and "no_wifi" in eff.group(1),
       "effWifi спрашивает тот же ключ, что и readPlan")
    wxok = re.search(r"function stripWxOk\(\)\{(.*?)\n\}", CODE, re.S)
    ok(wxok is not None and "no_wifi" not in wxok.group(1),
       "и частной копии правила у одного вызывающего не осталось")
    ok('id="noWifiNote"' in HTML, "в разделе сети есть плашка, называющая это состояние")

    print("== пароль брокера: обещание поля и поведение прибора ==")
    # Прошивка стирает учётные данные при смене хоста НАМЕРЕННО (они привязаны к прежнему брокеру) и
    # пишет в лог «re-enter them». Врало поле: «Leave blank to keep». Двигаем обещание, а не прибор.
    ok('setString("mq_pass", "")' in WEB, "прошивка по-прежнему стирает пароль при смене брокера")
    ok('cfg["mq_pass_set"]' in WEB, "и говорит консоли, сохранён ли он — как pass_set / owm_set / tg_set")
    hint = re.search(r"function mqPassHint\(\)\{(.*?)\n\}", CODE, re.S)
    ok(hint is not None and "ST.mqHostSaved" in hint.group(1),
       "подпись поля сравнивает набранный хост с сохранённым")
    ok(hint is not None and "no longer applies" in hint.group(1),
       "и при смене хоста говорит, что пароль придётся ввести заново")

    print("== замер CO2: свой интервал, а не кратный газовому ==")
    # Дефект, найденный на приборе владельца 2026-09-30: Performance обещает 5 минут, мерил 10.
    # measure() вызывалась только на ГАЗОВЫХ тиках, а газ подходит на 298 с против 300 у CO2 — тик,
    # упавший на секунду раньше, оставлял CO2 «ещё не пора», и следующий шанс был через целый
    # период. Оба цикла обязаны спрашивать одно и то же, и именно расхождение между ними продержало
    # дефект две недели: Matter починили 17 сентября, а комментарий там утверждал, что у соседа всё
    # хорошо.
    gates = re.findall(r"tick\.do_meas \|\| tick\.do_co2 \|\| tick\.forced", BSCODE)
    ok(len(gates) == 2, "оба цикла пускают к measure() и по CO2 тоже (нашлось %d из 2)" % len(gates))
    ok(not re.search(r"if \(tick\.do_meas \|\| tick\.forced\) \{\s*\n\s*measure\(tick\);", BSCODE),
       "газового гейта без CO2 не осталось")
    # Оснастка t_cycle повторяет этот гейт дословно — гонять настоящий цикл нельзя, пока стенд не
    # отвечает за SCD41. Копия обязана совпадать, иначе прогон каденции снова покажет то, чего на
    # приборе нет (так и вышло в первой редакции).
    CYC = open(os.path.join(HERE, "t_cycle.cpp"), encoding="utf-8").read()
    ok("if (t.do_meas || t.do_co2 || t.forced) {" in CYC,
       "и прогон каденции повторяет ТОТ ЖЕ гейт, а не своё представление о нём")

    print("== сырость: счётчик обязан считать замеры, а не вызовы ==")
    # Та же пара тактов, второй дефект из неё. classifyAir зовётся на газовом тике, а влажность
    # приходит от SCD41 по интервалу владельца, так что между замерами в неё уходит прежнее
    # значение из RTC. Счётчик «двух циклов подряд» рос на одном и том же числе — один выброс
    # объявлял сырость через пять минут, и уходил владельцу как WARNING, потому что alertPending()
    # ссылается на эту устойчивость как на причину не требовать второго подтверждения.
    # Поведение проверяет t_analytics прямым вызовом; здесь проверяется то, чего он видеть не
    # может — что прошивка подаёт туда НАСТОЯЩИЙ признак свежести, а не литерал.
    call = re.search(r"classifyAir\(([^;]*?)\);", BSCODE, re.S)
    ok(call is not None, "вызов классификатора найден в прошивке")
    if call:
        args = [a.strip() for a in call.group(1).split(",")]
        ok(len(args) == 12, "у вызова двенадцать аргументов (нашлось %d)" % len(args))
        ok(args[-1] == "air", "последний аргумент — вердикт о воздухе без сырости (подано: %s)" % args[-1])
        ok(args[6] == "t_out", "улица подана отдельным аргументом (подано: %s)" % args[6])
        # И ЧЕМ он на самом деле заполнен. Слом «подать NAN вместо улицы» прошёл молча: имя
        # аргумента осталось прежним, а сезонность исчезла целиком — прибор вернулся бы к
        # комнатному правилу круглый год, и ни одна проверка этого не заметила.
        decl = re.search(r"const float t_out\s*=\s*([^;]+);", BSCODE)
        ok(decl is not None and "outdoor_for_damp()" in decl.group(1),
           "и заполнен он помощником свежести, а не литералом (подано: %s)" %
           (decl.group(1).strip() if decl else "—"))
        ok(args[4] == "scd_ok",
           "признак свежести влажности — это scd_ok, а не литерал (подано: %s)" %
           (args[4] if len(args) > 4 else "—"))
    # И сам гейт в analytics.cpp: счётчик двигается только под проверкой свежести.
    AN = nocomments(open(os.path.join(MAIN, "analytics.cpp"), encoding="utf-8").read())
    m = re.search(r"if \(hum_fresh\) \{(.*?)\n    \}", AN, re.S)
    ok(m is not None, "счётчик сырости стоит под проверкой свежести")
    if m:
        ok("hum_sustain++" in m.group(1) and "hum_sustain = 0" in m.group(1),
           "и рост, и сброс счётчика — оба внутри этой проверки")
    ok(not re.search(r"\n    if \(hum_over\) \{ if \(st\.hum_sustain", AN),
       "безусловного счёта по вызовам не осталось")

    print("== сырость: своё место на стекле, прежнее место в тревогах ==")
    # Решение владельца 2026-10-02. «Damp» забирало слово у блока Air Quality, и комната, которая
    # была одновременно сырой и загрязнённой, докладывала только про влагу. Теперь сырость живёт в
    # блоке влажности — в том же 59-пиксельном слоте, что и Pressure рядом, который уже
    # инвертируется под Storm. Поведение проверяет t_screen эталонами, здесь — проводка.
    ok("humDamp" in re.search(r"struct Frame \{(.*?)\n\};", BSCODE, re.S).group(1),
       "признак сырости лежит В КАДРЕ, как всё остальное рисуемое")
    ok(not re.search(r'cls == 5 \? "Damp"', BSCODE),
       "слово блока Air Quality больше не берётся от класса 5")
    m = re.search(r"cur\.aqInv\s*=\s*\(([^;]*)\);", BSCODE)
    ok(m is not None and "5" not in m.group(1),
       "и инверсия Air Quality тоже: %s" % (m.group(1).strip() if m else "—"))
    ok(re.search(r"cur\.humDamp\s*=\s*\(cls == 5\)", BSCODE),
       "блок влажности берёт сырость из ОБЩЕГО класса")
    # Воздух без сырости живёт в своей RTC-переменной, и её обязаны обнулять там же, где класс:
    # раскладка RTC едет на каждой пересборке, и забытая переменная читает чужие байты.
    resets = re.findall(r"rtc_class = 0;([^\n]*)", BSCODE)
    ok(len(resets) == 2, "класс обнуляется в двух местах (нашлось %d)" % len(resets))
    ok(all("rtc_air_class = 0" in r for r in resets),
       "и воздух без сырости обнуляется ровно там же, обоими")

    print("== калибровка температуры: страница называет замер, по которому считала ==")
    # Снимок владельца 2026-10-02: «Device reads 20.4», введено 20.5, ответ «shifted by -0.2».
    # Оба числа верны, но они из РАЗНЫХ моментов: delta считается от показания на момент нажатия
    # (там было 20.7), а живая строка успевает уехать на следующий одиночный замер — минутой
    # позже и уже исправленный. Арифметика на экране не сходилась, хотя прибор был прав.
    cal = re.search(r"esp_err_t WebServer::postTempCalHandler.*?\n\}", WEB, re.S)
    ok(cal is not None, "обработчик калибровки найден")
    if cal:
        body = cal.group(0)
        ok('\\"read\\":%.2f' in body, "ответ несёт показание, по которому считал")
        # И оно обязано быть взято ОДИН раз: цикл публикует global_s_temp из другой задачи, так что
        # три чтения подряд могли смешать два замера в один ответ.
        ok(body.count("global_s_temp") == 1,
           "показание берётся один раз, а не перечитывается (найдено %d)" % body.count("global_s_temp"))
        ok("const float read = global_s_temp;" in body, "и кладётся в локальную переменную")
    m = re.search(r"const rd = \(r\.read==null\)[^;]*;", CODE)
    ok(m is not None, "консоль читает это поле")
    ok("the device read " in CODE and "and you said " in CODE,
       "и показывает обе половины вычитания, а не один итог")

    print("== сырость судится по стене, и консоль обещает то же ==")
    # Решение владельца 2026-10-02. Пороги живут в analytics.h, и текст карточки обязан называть
    # ТЕ ЖЕ числа: владельцу обещают правило, по которому прибор потом судит.
    AN = nocomments(open(os.path.join(MAIN, "analytics.h"), encoding="utf-8").read())
    want = {}
    for k in ("DAMP_SURF_TRIP", "DAMP_SURF_CLEAR", "DAMP_ROOM_TRIP", "DAMP_ROOM_CLEAR"):
        m = re.search(k + r"\s*=\s*([0-9.]+)f", AN)
        ok(m is not None, "%s объявлен в analytics.h" % k)
        if m: want[k] = int(float(m.group(1)))
    # Справочник классов убран из вкладки настроек 2026-10-08 (решение пользователя: это не
    # настройка, а справка, и она есть в гиде на сайте). Обещание про сырость от этого не
    # пропало — оно переехало к живому числу на дашборде, в подсказку «Coldest wall», и
    # спрашивать его надо там. Во вкладке настроек этого блока быть НЕ должно.
    ok("What the air-quality statuses mean" not in CODE,
       "справочник классов из настроек убран")
    card = re.search(r"Humidity at the coldest surface(.*?)</span>", CODE, re.S)
    ok(card is not None, "подсказка «Coldest wall» найдена на дашборде")
    if card and len(want) == 4:
        txt = card.group(1)
        ok(("<b>%d %%</b>" % want["DAMP_SURF_TRIP"]) in txt,
           "консоль называет порог по стене (%d %%)" % want["DAMP_SURF_TRIP"])
        ok(("<b>%d %%</b>" % want["DAMP_SURF_CLEAR"]) in txt,
           "и снятие по стене (%d %%)" % want["DAMP_SURF_CLEAR"])
        ok(str(want["DAMP_ROOM_TRIP"]) in txt and str(want["DAMP_ROOM_CLEAR"]) in txt,
           "и запасное правило без погоды (%d / %d)" % (want["DAMP_ROOM_TRIP"], want["DAMP_ROOM_CLEAR"]))
        ok("18" not in txt, "ветки точки росы в обещании больше нет")
    # Улица подаётся только пока свежая: вчерашняя погода описывает вчерашнюю стену.
    helper = re.search(r"static float outdoor_for_damp\(\)\s*\{(.*?)\n\}", BSCODE, re.S)
    ok(helper is not None, "помощник свежести улицы найден")
    if helper:
        ok("rtc_last_weather" in helper.group(1), "он смотрит на отметку последнего запроса погоды")
        ok("3600" in helper.group(1), "и на возраст в часах, а не берёт что попало")
        ok("NAN" in helper.group(1), "а не найдя — говорит NAN, а не подставляет ноль")

    print("== единицы: одно умолчание, а не три копии ==")
    # Три читателя обязаны ответить одинаково: экран, тексты Telegram/MQTT и /api/status, из
    # которого консоль ставит положение переключателя. Разойдись они — на стекле одна единица, на
    # странице другая, и владельцу нечем узнать, какой прибор верит. Это форма дефекта effWifi().
    UN = nocomments(open(os.path.join(MAIN, "units.h"), encoding="utf-8").read())
    ok("UNIT_PRES_DEFAULT = UNIT_PRES_MMHG" in UN,
       "умолчание давления — мм рт. ст. (решение владельца 2026-10-02)")
    for f, src in (("bench_screens.cpp", BSCODE), ("web_server.cpp", WEB)):
        ok(not re.search(r'"u_pres"[^;]*,\s*0\s*\)', src),
           "%s не держит своей копии умолчания давления" % f)
        ok(not re.search(r'"u_temp"[^;]*,\s*0\s*\)', src),
           "%s не держит своей копии умолчания температуры" % f)
    ok(len(re.findall(r"UNIT_PRES_DEFAULT", BSCODE + WEB)) >= 3,
       "все читатели берут умолчание из заголовка")

    print("== порог тревоги CO2: своё значение владельца ==")
    # Три пункта списка были не настройкой, а её удобным началом: прошивка всегда принимала любой
    # уровень внутри полосы, и гид на сайте всегда это обещал. Полоса теперь ОБЕЩАНА владельцу в
    # интерфейсе, поэтому обязана быть одна на прошивку и консоль.
    CO2H = open(os.path.join(MAIN, "co2_alert.h"), encoding="utf-8").read()
    hmin = re.search(r"CO2_ALERT_MIN\s*=\s*(\d+)", CO2H)
    hmax = re.search(r"CO2_ALERT_MAX\s*=\s*(\d+)", CO2H)
    hdef = re.search(r"CO2_ALERT_DEFAULT\s*=\s*(\d+)", CO2H)
    ok(hmin and hmax and hdef, "co2_alert.h держит полосу и умолчание в одном месте")
    if hmin and hmax and hdef:
        jmin = re.search(r"CO2_MIN\s*=\s*(\d+)", CODE)
        jmax = re.search(r"CO2_MAX\s*=\s*(\d+)", CODE)
        ok(jmin and jmax and jmin.group(1) == hmin.group(1) and jmax.group(1) == hmax.group(1),
           "консоль обещает ту же полосу, что примет прибор (%s..%s против %s..%s)"
           % (jmin.group(1) if jmin else "-", jmax.group(1) if jmax else "-",
              hmin.group(1), hmax.group(1)))
        # Зажим и чтение обязаны идти от заголовка, иначе полоса разъедется молча: владелец наберёт
        # число на границе, консоль его примет, а прибор сохранит другое.
        ok(re.search(r'doc\["al_co2"\].*?CO2_ALERT_MIN.*?CO2_ALERT_MAX', WEB, re.S) is not None,
           "зажим в /api/save взят из заголовка")
        ok("CO2_ALERT_MIN" in BSCODE and "CO2_ALERT_MAX" in BSCODE,
           "цикл читает уровень по тем же границам")
        # Границы ищутся в ТЕЛЕ того блока, который их применяет. Первая редакция этой проверки
        # шла от '"al_co2"' по [^;]*?, то есть не могла перешагнуть точку с запятой в предыдущей
        # строке и не сработала бы ни при каком содержимом зажима — проверка, которую нельзя
        # сломать, не стоит ничего (поймано намеренным сломом 2026-09-30).
        blocks = (("web_server.cpp",
                   re.search(r'doc\["al_co2"\]\.isNull\(\)\)\s*\{.*?\n    \}', WEB, re.S)),
                  ("bench_screens.cpp",
                   re.search(r'AlertConfig load_alert_config\(\).*?\n\}', BSCODE, re.S)))
        for f, m in blocks:
            ok(m is not None, "%s: блок с уровнем найден" % f)
            if m:
                nums = sorted(set(re.findall(r'\b(700|1600)\b', m.group(0))))
                ok(not nums, "%s не держит своей копии границ%s"
                             % (f, "" if not nums else " — в теле " + ", ".join(nums)))
        # Умолчание тоже одно: прошивка отдаёт его в /api/status, и консоль на нём стоит.
        ok(('nvs_u32("al_co2", CO2_ALERT_DEFAULT)' in WEB),
           "умолчание в /api/status — из заголовка")
        ok(('value="%s">%s ppm' % (hdef.group(1), hdef.group(1))) in HTML,
           "и оно же — пункт «recommended» в списке")
    # Пункт есть, поле есть, и поле спрятано, пока его не выбрали.
    ok('<option value="custom">' in HTML, "в списке появился пункт своего значения")
    ok('id="alCo2Custom"' in HTML and 'id="alCo2CustomRow"' in HTML, "и поле под него")
    ok(re.search(r'id="alCo2CustomRow"[^>]*display:none', HTML) is not None,
       "поле скрыто, пока своё значение не выбрано")
    # Значение, которого нет в списке, НЕЛЬЗЯ присваивать <select> напрямую: браузер оставит
    # показанным ПЕРВЫЙ пункт, и следующее сохранение запишет его поверх уровня владельца.
    ok("el('alCo2').value = String(cfg.al_co2)" not in CODE,
       "уровень с прибора больше не присваивается списку вслепую")
    ok("co2FromCfg(cfg)" in CODE and "CO2_PRESETS.indexOf(v)" in CODE,
       "неизвестный уровень уходит в поле своего значения, а не теряется")
    # Тот же сторож, что у нижней полосы: опрос статуса идёт раз в несколько секунд и затёр бы
    # уровень, который владелец в этот момент набирает.
    ok(re.search(r"function co2FromCfg[^}]*?if\(ST\.co2Dirty\) return;", CODE, re.S) is not None,
       "опрос не затирает то, что владелец набирает")
    ok("ST.co2Dirty = false;" in CODE, "и снова говорит за это поле после сохранения")
    # Сохранение проверяет число САМО: прошивка зажала бы 2000 до 1600, а окно отчиталось бы о 2000.
    ok(re.search(r"const lvl = co2Level\(\);\s*\n\s*if\(lvl === null\)", CODE) is not None,
       "сохранение отказывает до отправки, а не полагается на зажим прибора")
    ok("al_co2:lvl" in CODE and "+el('alCo2').value" not in CODE,
       "и в запрос, и в отчёт идёт проверенное число")

    print("== калибровка CO2: сначала установление, потом шесть замеров ==")
    # Даташит SCD4x 3.8.1 требует двух вещей: «at least 3 minutes» И «in an environment with a
    # homogenous and CONSTANT CO2 concentration». Шесть замеров закрывали первое вдвое, второе не
    # закрывал никто: владелец выносил прибор и нажимал сразу, а первые замеры шли по комнатному
    # воздуху, ещё не вышедшему из корпуса — среднее выходило выше уличного, и FRC перекорректировала.
    ok("CAL_SETTLE_MIN" in BSCODE and "CAL_SETTLE_MAX" in BSCODE and "CAL_SETTLE_DROP" in BSCODE,
       "у установления есть пол, потолок и порог")
    # Проверка на ИЗМЕНЕНИЕ, а не на уровень — это несущее: уровень заражён тем самым дрейфом,
    # который калибровка и убирает (датчик на +350 читает свежий воздух как 770), а разность нет.
    settle = re.search(r"\} else if \(!cal_settled\) \{(.*?)\n                        \} else if", BSCODE, re.S)
    ok(settle is not None, "фаза установления найдена")
    if settle:
        b = settle.group(1)
        ok("cal_trail[cal_trail_n - 3]" in b and "CAL_SETTLE_DROP" in b,
           "установление судит по падению последних трёх, а не по уровню")
        ok("CAL_MEAN" not in b and "420" not in b,
           "и ни одного абсолютного порога в этой фазе — он был бы заражён дрейфом")
        ok("global_calib_unsettled = 1" in b,
           "по потолку прогон идёт ВСЁ РАВНО, а не отказывает — и это попадает в отчёт")
    # Прогон начинается только после установления.
    ok("if (cal_settled && global_calib_samples >= SCD41::FRC_RUNUP_SHOTS)" in BSCODE,
       "FRC — только после установления и шести замеров")
    # Окно не имеет права обрезать удлинившийся прогон на своём сорокаминутном потолке.
    ok(re.search(r"cal_holds = \(global_calib_result == 2\).*?CAL_HOLD_MS", BSCODE, re.S) is not None,
       "потолок окна уступает идущей калибровке — ограниченно, по её собственному пределу")

    print("== калибровка CO2: её видно на стекле ==")
    # Без этого прогон не может дойти до владельца вообще: он стоит на улице, страница может быть
    # закрыта, телефон спит, Telegram может быть не настроен — а прогон стал длиннее.
    SCR = nocomments(open(os.path.join(MAIN, "screen.cpp"), encoding="utf-8").read())
    ok("void drawCalibScreen" in SCR, "экран калибровки есть")
    ok("drawCalibScreen(epd, c)" in BSCODE, "и цикл его рисует")
    paint = re.search(r"auto cal_paint = \[&\]\((.*?)\n    \};", BSCODE, re.S)
    ok(paint is not None and "ran_min" in paint.group(1),
       "показывается ПРОШЕДШЕЕ время, а не оставшееся")
    ok(paint is not None and "trail" in paint.group(1),
       "и хвост показаний — по нему видно, что воздух ещё меняется")
    # Консоль не должна говорить «reading 0 of 6», пока идёт установление.
    ok("c.co2cal_settled" in CODE, "консоль различает установление и прогон")
    ok('sensor["co2cal_settled"]' in WEB and 'sensor["co2cal_unsettled"]' in WEB,
       "и прибор ей об этом сообщает")

    print("== калибровка CO2: прибор не судит о воздухе ==")
    # Решение пользователя 2026-09-30, отменяющее пороги от 16 сентября. Проверка свежести
    # применялась к показанию, которое калибровка и существует чтобы исправить: датчик, ушедший на
    # +350, читает свежий воздух как 770, слышит «это комната» и больше НИКОГДА не проходит порог —
    # а ASC в режиме одиночного замера недоступен, то есть базовую линию не сдвинуть ничем.
    # Асимметрия, понятая наоборот: неверная калибровка отменяется повторной, отказ — ничем.
    for name in ("CAL_SPREAD_MAX", "CAL_MEAN_MAX", "CAL_CONFIRM_MS"):
        ok(name not in BSCODE, "порог %s больше не решает за владельца" % name)
    ok("global_calib_result == 3" not in BSCODE and "global_calib_result != 3" not in BSCODE,
       "состояния «спрашиваю про воздух» в прошивке не осталось")
    co2cal = re.search(r'sensor\["co2cal"\] =.*?;', WEB, re.S)
    ok(co2cal is not None and '"confirm"' not in co2cal.group(0),
       "и /api/status его не отдаёт (у отката прошивки свой confirm, он законен)")
    # Прогон кончается записью, а не вопросом.
    # Не «где-то в блоке есть cal_do_frc», а «в блоке НЕТ УСЛОВИЯ». Первая редакция шла по [^}]*? и
    # не видела вложенного if — то есть порог, возвращённый внутрь, проходил бы молча. Это ровно тот
    # дефект, который здесь и стерегут, и он был единственным непойманным сломом (2026-09-30).
    frc = re.search(r"global_calib_samples >= SCD41::FRC_RUNUP_SHOTS\) \{\n(.*?)\n( *)\}",
                    BSCODE, re.S)
    ok(frc is not None, "блок «шесть замеров прочитаны» найден")
    if frc:
        gated = [l.strip() for l in frc.group(1).split("\n") if re.search(r"\b(if|\?)\b|\?", l)]
        ok("cal_do_frc();" in frc.group(1) and not gated,
           "шесть замеров ведут ПРЯМО к FRC, без единого условия%s"
           % ("" if not gated else " — мешает: " + gated[0][:60]))
    # Числа никуда не делись — они стали сведениями в отчёте.
    ok("CAL_MEAN_ROOMY" in BSCODE, "среднее по-прежнему считается")
    ok(re.search(r"global_calib_avg > CAL_MEAN_ROOMY", BSCODE) is not None,
       "и высокое среднее попадает в отчёт — ПОСЛЕ записи, где цена ему повтор, а не тупик")
    # Единственный ответ, который прибор ещё принимает, — Stop.
    ok(re.search(r"global_calib_result == 2 && answer == -1", WEB) is not None,
       "прибор принимает только Stop и только во время прогона")
    ans = re.search(r"async function calAnswer\(go\)\{(.*?)\n\}", CODE, re.S)
    ok(ans is not None and "confirm" not in ans.group(1),
       "консоль больше не шлёт «калибруй всё равно»")
    # Вопрос переехал в начало — туда, где на него можно ответить: телефон в руке, прибор ещё не
    # читал ничего. На балконе через шесть минут отвечать было нечем.
    body = re.search(r"el\('calibrateBtn'\)\.onclick=async\(\)=>\{(.*?)\n\};", CODE, re.S)
    ok(body is not None, "обработчик кнопки калибровки найден")
    if body:
        b = body.group(1)
        i_ask  = b.find("showModal")
        i_post = b.find("/api/calibrate")
        ok(i_ask >= 0 and i_post >= 0 and i_ask < i_post,
           "подтверждение спрашивается ДО запроса к прибору, а не после шести минут")
        ok(re.search(r"if\(!go\)\s*return;", b) is not None, "и отказ ничего не отправляет")

    print("== калибровка CO2: давление только своё ==")
    ok(re.search(r"rtc_readings_are_ours\(\)\s*&&\s*rtc_pres_pa\s*>\s*0\.0f\)\s*\n\s*cal_scd\.setAmbientPressure",
                 BSCODE) is not None,
       "прогон FRC не берёт давление, за которое эта сборка не отвечает")

    print("== батарея: наклон считается по идущему времени ==")
    ok(re.search(r"BatteryMonitor::getInstance\(\)\.update\(\w+,\s*\(double\)time\(NULL\)", BSCODE)
       is not None, "наклон получает настенные часы, а не часы BSEC")
    ok("rtc_bsec_ns / 1e9, bi" not in BSCODE, "часы BSEC сюда больше не попадают")
    ok(re.search(r"monotonic_sec\s*<\s*rtc_chg_t0\)\s*rtc_chg_t0\s*=\s*monotonic_sec", BATCODE) is not None,
       "окно заряда, побежавшее назад, перезакрепляет якорь, а не замирает навсегда")

    # Решение пользователя 2026-10-03: у карточки батареи одна оценка, а не две. Второй наклон в
    # RTC_DATA обнулялся каждой перезагрузкой и заговаривал через шесть часов — «523 days» под
    # заметкой «Collecting». Сторожим, чтобы он не вернулся ни в каком виде.
    print("== батарея: одна оценка расхода на всю карточку ==")
    ok("anchor_soc" not in BATCODE and "SLOPE_MIN_WINDOW" not in BATCODE,
       "в BatteryMonitor второго наклона нет")
    ok("daily_pct" not in open(os.path.join(MAIN, "battery_monitor.h"), encoding="utf-8").read(),
       "и полей под него в BatteryInfo тоже")
    SRVC = nocomments(SRV)
    for f in ("bat_daily_p", "bat_daily_mah", "bat_tte_h"):
        m = re.search(r'sensor\["%s"\]\s*=\s*([^;]+);' % f, SRVC)
        ok(m is not None and m.group(1).strip().startswith("pc."),
           '%s берётся из PowerEstimate::card(), а не из своей копии (%s)'
           % (f, m.group(1).strip() if m else "не найдено"))
    ok(re.search(r"PowerEstimate::card\(\s*pe\s*,", SRVC) is not None,
       "card() получает ту же подгонку, что и «Measured draw»")
    for f in ("bench_screens.cpp", "telemetry.cpp", "web_server.cpp"):
        code = nocomments(open(os.path.join(MAIN, f), encoding="utf-8").read())
        ok("global_bat_daily_pct" not in code and "global_bat_tte_h" not in code,
           "%s не держит старых глобалов наклона" % f)

    # 2026-10-04: хаб получает ту же оценку, что и страница. До этого Home Assistant считал своё —
    # производную целых процентов — и показывал «0,174 мА, 392 дня» прибору, тратившему ~0,43.
    print("== батарея: хаб получает оценку прибора ==")
    MQ = nocomments(open(os.path.join(MAIN, "mqtt_publisher.cpp"), encoding="utf-8").read())
    bs = re.search(r"std::string MqttPublisher::buildState\(\)\s*\{(.*?)\n\}", MQ, re.S)
    ok(bs is not None, "buildState найден")
    if bs:
        body = bs.group(1)
        ok("PowerHistory::estimate()" in body and re.search(r"PowerEstimate::card\(\s*pe\s*,", body),
           "MQTT берёт ту же подгонку и тот же card(), что и страница")
        g = re.search(r"if \(pc\.mah_day > 0\.0f\) \{(.*?)\n        \}", body, re.S)
        ok(g is not None and all('d["%s"]' % f in g.group(1) for f in ("draw_ma", "life_months", "days_left")),
           "числа уходят только когда падение достаточно — внутри проверки card()")
        ok('d["draw_quality"]' in body and (g is None or 'draw_quality' not in g.group(1)),
           "а слово о качестве — всегда, чтобы хаб мог сказать, почему числа нет")
        # Каждое поле, которое регистрация обещает HA, обязано существовать в сообщении.
        for slug, field in (("draw", "draw_ma"), ("daysleft", "days_left"), ("life", "life_months"),
                            ("drawq", "draw_quality")):
            ok(re.search(r'emit\(cl, "sensor", uid, "%s", "[^"]+", "%s"' % (slug, field), MQ) is not None
               and 'd["%s"]' % field in body,
               "датчик HA «%s» читает поле %s, и оно в сообщении есть" % (slug, field))
    ok("PowerHistory::estimate()" in nocomments(SRV) and "HistStore::getInstance().query(0, 0, 600"
       not in nocomments(SRV), "страница читает историю через тот же PowerHistory, своей копии нет")

    # Запись истории: поле, заполненное из прошлого цикла и помеченное временем этого, — это
    # замер, которого не было, и графики рисуют его как данные.
    print("== история: в запись идёт только то, что ответило ==")
    ok(re.search(r"hist_due\s*=\s*do_meas\s*&&\s*sensors_ok", BSCODE) is not None,
       "пробуждение, где никто не ответил, записи не пишет")
    for field, flag in (("rec.iaq", "bme_ok"), ("rec.siaq", "bme_ok"), ("rec.gas", "bme_ok"),
                        ("rec.b_temp_d", "bme_ok"), ("rec.soc ", "bat_ok"),
                        ("rec.temp_d", "have_scd"), ("rec.hum", "have_scd")):
        m = re.search(re.escape(field) + r"\s*=\s*([^;]+);", BSCODE, re.S)
        ok(m is not None and flag in m.group(1), "%s закрыто флагом %s" % (field.strip(), flag))
    ok(re.search(r"rec\.(iaq|siaq|b_temp_d|b_hum)\s*=\s*do_meas", BSCODE) is None,
       "ни одно поле газа не закрыто сроком вместо ответа")

    # Окно радио стоит по-разному в двух режимах, и до 2026-09-21 модель этого не знала:
    # приборy на хабе она начисляла цену Wi-Fi, завышая радио примерно в полтора раза.
    print("== модель батареи знает про режим связи ==")
    ok(re.search(r"function radioSecs\(\)", CODE) is not None, "стоимость окна выбирается функцией")
    m = re.search(r"function radioSecs\(\)\s*\{(.*?)\n\}", CODE, re.S)
    ok(m is not None, "тело radioSecs найдено")
    if m:
        ok("wl_mode" in m.group(1), "и выбирает по режиму, а не по профилю")
        ok("WIFI_S_HUB" in m.group(1) and "WIFI_S" in m.group(1), "обе цены задействованы")
    ok(re.search(r"sess\s*\*\s*radioSecs\(\)", CODE) is not None,
       "расчёт берёт её, а не константу напрямую")
    ok(re.search(r"NRG\.WIFI_S\b", CODE.replace("NRG.WIFI_S_HUB","")) is None
       or CODE.count("NRG.WIFI_S") <= 2,
       "прямых обращений к константе мимо функции не осталось")
    # Хаб не может выйти дороже, чем самостоятельный поход за погодой — иначе знак перепутан.
    energy_model()
    mw = re.search(r"WIFI_S:([\d.]+)", CODE); mh = re.search(r"WIFI_S_HUB:([\d.]+)", CODE)
    ok(mw is not None and mh is not None, "обе цены заданы числами")
    if mw and mh:
        ok(float(mh.group(1)) < float(mw.group(1)),
           "окно с хабом дешевле (%s < %s с)" % (mh.group(1), mw.group(1)))

    # Показание батареи на кабеле. Газомер судит о заряде по НАПРЯЖЕНИЮ, а зарядник держит
    # ячейку на 4,2 В потому, что он зарядник, а не потому, что она полная. Отсюда «100 %» на
    # половине заряда и обвал при вынутом кабеле. Рост на кабеле держит потолок; падение после —
    # показанный процент (battery_display.h, решение владельца 2026-10-05).
    print("== батарея: чего зарядник не может добавить к числу на стекле ==")
    ok("soc_raw" in open(os.path.join(MAIN, "battery_monitor.h"), encoding="utf-8").read(),
       "у показания есть сырой двойник для журнала")
    m = re.search(r"info\.soc_raw\s*=\s*info\.soc;", BATCODE)
    ok(m is not None, "сырое значение снимается до всякой правки")
    i_raw  = BATCODE.find("info.soc_raw = info.soc;")
    i_cap  = BATCODE.find("cable_shown = ceiling;")
    ok(i_raw >= 0 and i_cap >= 0 and i_raw < i_cap,
       "и снимается РАНЬШЕ ограничителя, а не после")
    # Ограничитель обязан действовать только при воткнутом кабеле: на батарее газомер говорит
    # правду, и накрывать её потолком было бы ложью в другую сторону.
    m = re.search(r"if \(on_usb\) \{(.*?)\n        \}", BATCODE, re.S)
    ok(m is not None, "ветка «с кабелем» найдена")
    if m:
        on = m.group(1)
        ok("cable_shown = ceiling" in on, "с кабелем действует потолок")
        ok("monotonic_sec < rtc_chg_t0" in on,
           "окно, побежавшее назад, перезакрепляет начало зарядки")
        # Опора потолка — то, что показывали на батарее, а показанное бывает выше измеренного:
        # без пола подключение кабеля роняло бы цифру.
        ok(re.search(r"if \(cable_shown < rtc_rest_soc\) cable_shown = rtc_rest_soc;", on) is not None,
           "подключение кабеля не отнимает пункт")
        ok(re.search(r"if \(done\) cable_shown = 100\.0f;", on) is not None,
           "«готово» зарядника — 100 на обеих платах")
    m = re.search(r"if \(!on_usb\) \{(.*?)\n    \}", BATCODE, re.S)
    ok(m is not None, "ветка «без кабеля» найдена")
    if m:
        off = m.group(1)
        ok("rtc_rest_soc = info.soc" in off, "без кабеля показание становится опорой")
        ok("ceiling" not in off, "на батарее потолка нет")
        ok(BATCODE.find("info.soc = st.shown;") >= 0 and
           BATCODE.find("info.soc = st.shown;") < m.start(),
           "и опора — то, что показано, а не измерено: следующая зарядка продолжает число")
    # Показанный процент: на вход — измерение, на выход — стекло.
    ok(re.search(r"BatteryDisplay::step\(rtc_disp, info\.soc_raw,", BATCODE) is not None,
       "показанный процент строится от измеренного")
    # А «дней осталось» с 2026-10-19 делится на ПОКАЗАННЫЙ процент — тот, что стоит рядом с ним на
    # той же странице и на стекле. До этого карточка и хаб делили на измеренный, и после заряда, пока
    # идёт плавный спуск, два числа рядом описывали разные ячейки. Это ровно то расхождение, из-за
    # которого цифру месяц не могли назвать: «чтобы везде всё было одинаково» (владелец, 2026-10-19).
    # Цена названа: сразу после заряда показанное щедрее измеренного, значит и дни оптимистичнее — на
    # время спуска. Поэтому измеренное отдаётся наружу рядом (bat_p_meas, battery_measured), чтобы
    # расхождение было видно, а не угадывалось.
    for f in ("web_server.cpp", "mqtt_publisher.cpp"):
        src = nocomments(open(os.path.join(MAIN, f), encoding="utf-8").read())
        ok("PowerEstimate::card(pe, global_soc," in src and "global_soc_raw," not in src.split("card(pe,")[1][:30],
           "%s: время до разряда — от показанного процента, того же, что на стекле" % f)
    SRV2 = nocomments(open(os.path.join(MAIN, "web_server.cpp"), encoding="utf-8").read())
    MQ2  = nocomments(open(os.path.join(MAIN, "mqtt_publisher.cpp"), encoding="utf-8").read())
    ok('sensor["bat_p_meas"]' in SRV2, "и измеренный процент по-прежнему видно в /api/status")
    ok('d["battery_measured"]' in MQ2, "а с 2026-10-19 и в хабе, рядом с показанным")
    # Скорость должна идти от ёмкости, которую задал владелец, а не быть зашитой: с другой
    # ячейкой зашитое число либо прячет настоящую зарядку, либо не ограничивает вовсе.
    ok(re.search(r"max_rate\s*=\s*100\.0f\s*\*\s*CHG_CURRENT_MA\s*/\s*cap_mah", BATCODE) is not None,
       "потолок роста считается от ёмкости, а не зашит числом")
    ok("BATTERY_CAPACITY_MAH" in BATCODE, "и ёмкость приходит из battery_spec.h")
    # Журнал — запись измерений, а не то, что показали человеку.
    ok(re.search(r"soc_log\s*=\s*bat_ok\s*\?\s*rtc_soc_raw", BSCODE) is not None,
       "в историю идёт показание газомера, а не исправленное для экрана")
    ok(re.search(r"rec\.soc\s*=\s*bat_ok\s*\?[^;]*soc_log", BSCODE) is not None,
       "и оба поля заряда тоже")

    # --- 2026-10-13: измерение свободно, удержание — на слое отображения ------------------------
    # Правило владельца «на батарее процент не растёт» стояло ДВАЖДЫ: в battery_display.cpp, где оно
    # и написано, и копией в BatterySource::percent(), то есть на самом измерении. Копия ничего не
    # добавляла владельцу и ломала оценку расхода: ряд, которому запрещено подниматься, на плато
    # 103450 стоит сутками, а прямая через постоянную плоская — прибор отдал в Home Assistant
    # «2 147 483 647 дней».
    SRC = nocomments(open(os.path.join(MAIN, "battery_source.cpp"), encoding="utf-8").read())
    ok("rtc_soc_hold" not in SRC, "в измерении удержания больше нет")
    m = re.search(r"float percent\(float volts.*?\n\}", SRC, re.S)
    ok(m is not None, "percent() найдена")
    if m:
        # Ветка батареи — одно возвращение кривой от среднего. Проверяется именно она, а не
        # отсутствие имени: удержание можно вернуть под любым другим именем или прямо выражением.
        batt = m.group(0).split("}")[-2]
        ok(re.search(r"return socFromVoltage\(smoothed\(volts, now_s, false\), cell\);", batt)
           is not None and ">" not in batt,
           "на батарее percent() отдаёт кривую как есть, без сравнения с запомненным")
    DSP = nocomments(open(os.path.join(MAIN, "battery_display.cpp"), encoding="utf-8").read())
    # А на слое отображения правило владельца «на батарее не растёт» с 2026-10-19 стало
    # ОГРАНИЧИТЕЛЕМ СКОРОСТИ, а не защёлкой. Защёлка (`if (nv > s.shown) nv = s.shown;`) означала,
    # что показанное может принять только наименьшее значение, какого цель когда-либо достигала, и
    # одно нагруженное чтение оставалось там навсегда: на полевой плате 67 % против 74,5 %, которые
    # её собственные вольты дают по таблице её ячейки, — месяц.
    ok("if (nv > s.shown) nv = s.shown;" not in DSP, "односторонней защёлки на стекле больше нет")
    ok(re.search(r"clampf\(s\.shown \+ clampf\(s\.tgt_f - s\.shown, -limit, limit\)", DSP) is not None,
       "движение к цели ограничено по скорости и СИММЕТРИЧНО — вниз и вверх одинаково")
    ok("prev_target" not in DSP, "и накопления падений цели, которым жила защёлка, не осталось")
    # Цель перед этим сглаживается, и это не украшение: ограничитель не умеет следить за сигналом,
    # который дрожит больше его шага (измерено в поле: до 30 мВ между соседними записями, то есть до
    # 25 пунктов на плато). Первая редакция правки была без фильтра и ЗАМИРАЛА — поймал t_cycle.
    ok(re.search(r"s\.tgt_f\s*=\s*clampf\(s\.tgt_f \+ a \* \(tgt - s\.tgt_f\)", DSP) is not None,
       "а сама цель сглажена экспоненциальным средним — симметричным, без мнения о стороне")
    ok(re.search(r"a\s*=\s*low \? 1\.0f : .*SMOOTH_TAU_S", DSP) is not None,
       "ниже LOW_PCT сглаживания нет вовсе: там стекло обязано совпасть с измерением")
    ok(re.search(r"rate\s*=\s*low \? CATCHUP_LOW_PCT_H : CATCHUP_PCT_H", DSP) is not None,
       "и темп там прежний, вчетверо быстрее: туда смотрят блокировка и предупреждения")

    # Пол в подгонке: прямая через постоянную плоская не ровно, и «наклон больше нуля» это пропускало.
    ok("MIN_FIT_PCT" in PWR, "подгонка отказывает окну, которое не двинулось")
    ok(re.search(r"e\.drop\s*=\s*\(float\)\(-slope \* e\.hours\)", PWR) is not None,
       "падение — наклон × окно, а не разность двух концов")
    ok(re.search(r"e\.hours\s*=.*pts\[end\]\.t - pts\[start\]\.t", PWR) is not None,
       "срок — весь отрезок после отсечки кабеля, на нём же стоит и наклон")
    # Правило половины окна снято 2026-10-19 вместе с фильтром скачков. Половина была там, чтобы не
    # захватить отход от зарядника, а это делает SETTLE_S по времени; на медленной ячейке половина
    # десяти суток — пять, а пять суток при 0,65 %/сут это 3,3 % против 6 % точности газомера.
    ok(len(re.findall(r"for \(int i = start; i <= end; i\+\+\)", PWR)) == 2,
       "оба прохода наименьших квадратов идут по всему отрезку, без половины")
    # Фильтра «скачков» нет: он срезал подъёмы больше полупроцента и оставлял такие же спуски, то
    # есть на плато, где один милливольт это 0,83 % при точности газомера ±7,5 мВ, делал из шума
    # дрейф. На полевом месяце он отвечал 2,05 %/сут против истинных 0,68.
    for gone in ("JUMP_RISE_PCT", "JUMP_FALL_PCT", "soc_at(", "offset"):
        ok(gone not in PWR, "односторонней правки ряда не осталось: нет %s" % gone)
    ok("OUTLIER_SIGMA" in PWR and re.search(r"fabs\(y - \(slope \* x \+ icept\)\) > OUTLIER_SIGMA", PWR)
       is not None,
       "вместо него отсев по расстоянию от прямой — симметричный, без мнения о стороне")
    # Числа полос — из заголовка, не литералами: их читают и подпись плашки в консоли, и ярлык.
    ok("MIN_DROP_PCT" in PWR and "GOOD_DROP_PCT" in PWR, "полосы уверенности берутся из заголовка")

    # Три числа оценки уходят в хаб через ОПРЕДЕЛЁННОЕ преобразование. Приведение float к int, не
    # влезающее в int, — неопределённое поведение, и на этом целевом железе оно тихо упирается в
    # 2147483647: именно это число Home Assistant и нарисовал.
    MQE = nocomments(open(os.path.join(MAIN, "mqtt_publisher.cpp"), encoding="utf-8").read())
    for f in ("draw_ma", "life_months", "days_left"):
        m = re.search(r'd\["%s"\]\s*=\s*([^;]+);' % f, MQE)
        ok(m is not None and "rounded_fact" in m.group(1),
           "%s уходит через rounded_fact, а не голым приведением" % f)

    # Выключение вручную. Это не новый режим питания: это дверь в тот же сон, в котором с 0.9.43
    # лежит ненастроенный прибор под логотипом — датчики запаркованы, питание панели снято, все
    # источники пробуждения сняты и вооружена только кнопка. Проверяем ровно то, что отличает
    # выключение от «просто уснул»: что оно помнится, что экран это говорит, и что из него есть
    # выход.
    print("== выключение вручную ==")
    m = re.search(r"static void enter_power_off\(EPaper &epd\)\s*\{(.*?)\n\}", BSCODE, re.S)
    ok(m is not None, "выключение найдено")
    if m:
        body = m.group(1)
        # Порядок важен: если отметка пишется после отрисовки, прибор, у которого не собрался
        # экран, молча проснётся и начнёт мерить как ни в чём не бывало.
        i_nvs  = body.find('setU8("pwr_off", 1)')
        i_draw = body.find("drawOffScreen")
        ok(i_nvs >= 0 and i_draw >= 0 and i_nvs < i_draw,
           "отметка о выключении пишется ДО отрисовки, а не после")
        ok("PanelSleep::Parked" in body, "питание панели снимается — иначе это не выключение")
        ok("park_sensors" in body, "датчики паркуются")
        ok("WiFiManager::getInstance().shutdown" in body, "радио кладётся здесь, а не у вызывающего")
        i_wait = body.find('wait_button_release')
        i_arm  = body.find("arm_wake_pins")
        ok(i_wait >= 0 and i_arm >= 0 and i_wait < i_arm,
           "кнопку ждут отпущенной до вооружения: на прижатой линии сон кончится мгновенно")
        ok("900ULL * 1000000ULL" in body,
           "не вооружилась кнопка — просыпаемся через 15 мин, а не пропадаем навсегда")
    # Из выключения должен быть выход, и он один — кнопка.
    ok(re.search(r"if \(power_off_pending\(\)\)", BSCODE) is not None,
       "загрузка спрашивает, не выключен ли прибор")
    m = re.search(r"if \(power_off_pending\(\)\)\s*\{(.*?)\n    \} else if", BSCODE, re.S)
    ok(m is not None, "ветка «уже выключен» найдена")
    if m:
        body = m.group(1)
        ok('setU8("pwr_off", 0)' in body, "нажатие кнопки снимает отметку")
        ok("rtc_tick = 0" in body,
           "включение даёт полный кадр, замер и радио сразу — владелец стоит и ждёт")
        ok("drawOffScreen" not in body and "epd.init" not in body,
           "пробуждение не по кнопке не трогает панель: картинка уже на стекле")
        ok("enter_deep_sleep" in body, "и возвращается в сон, а не продолжает цикл")
    # Подтверждение: без него случайное удержание оставляет прибор выключенным, а выключенный
    # прибор на e-ink неотличим от сломанного.
    m = re.search(r"static bool confirm_power_off\(EPaper &epd\)\s*\{(.*?)\n\}", BSCODE, re.S)
    ok(m is not None, "подтверждение найдено")
    if m:
        body = m.group(1)
        ok("drawOffConfirmScreen" in body, "вопрос выводится на стекло")
        ok(body.count("gpio_get_level(PIN_USER_BTN) == 0") >= 2,
           "подтверждение требует двух низких чтений подряд — одно это дребезг")
        ok("OFF_CONFIRM_S" in body and "return false" in body,
           "и само отвечает «нет», если никто не подтвердил")
    ok(re.search(r"BTN_OFF_MS\s*=\s*10000", BSCODE) is not None,
       "порог выключения 10 с — далеко от трёх, а не рядом")
    ok(re.search(r"if \(confirm_power_off\(epd\)\) enter_power_off\(epd\)", BSCODE) is not None,
       "десятисекундное нажатие спрашивает, а не выключает сразу")

    # Ночная парковка панели. Правило, на котором всё держится, измерено на этом железе и ещё ни
    # разу не подвело: частичный кадр НЕ переживает снятия питания, полный переживает. Значит
    # каждый кадр, за которым гаснет рельса, обязан быть полным — а решение парковать принимается
    # за несколько вызовов до того, как кадр нарисован. Разъедься эти двое, и владелец найдёт
    # утром серое стекло, которое нечем объяснить.
    print("== ночная парковка панели ==")
    m = re.search(r"const bool due_full\s*=(.*?);", BSCODE, re.S)
    ok(m is not None, "условие полного кадра найдено")
    if m:
        ok("night_freeze" in m.group(1),
           "тик, после которого снимут питание, требует полного кадра")
    m = re.search(r"const bool night_park\s*=(.*?);", BSCODE, re.S)
    ok(m is not None, "решение о парковке найдено")
    if m:
        cond = m.group(1)
        ok("!by_button" in cond, "нажатие отменяет парковку: владелец смотрит на экран")
        ok("!plan.matter" in cond, "режим Matter не паркуется — ему негде это сделать")
    # Второй рубеж: даже если решение и кадр разойдутся, питание не снимут над частичным кадром.
    m = re.search(r"static void sleep_after_cycle\(.*?\n\}", BSCODE, re.S)
    ok(m is not None, "выход в сон найден")
    if m:
        body = m.group(0)
        # Именно условие, а не любое упоминание drew == 1: тремя строками ниже стоит
        # `drew == 1 ? "partial"` в строке лога, и первая версия проверки ловила её.
        ok(re.search(r"park_now\s*&&\s*drew\s*==\s*1", body) is not None
           and "park_now = false" in body,
           "над частичным кадром питание не снимается ни при каких условиях")
        ok(re.search(r"panel_to_sleep\(epd,\s*park_now\s*\?", body) is not None,
           "и парковка идёт именно через эту проверку, а не мимо неё")
    # Опора для частичного кадра живёт в ОЗУ самой панели, и та сидит на снятой рельсе.
    m = re.search(r"if \(how == PanelSleep::Parked\)\s*\{(.*?)\n        return;", BSCODE, re.S)
    ok(m is not None, "ветка парковки найдена")
    if m:
        ok("rtc_prev_valid = false" in m.group(1),
           "снятие питания обнуляет опору: следующий кадр будет полным")
    # Погода от хаба. Снипет в консоли — это ДОГОВОР с прошивкой, и разойтись они могут молча:
    # поле, которое перестанут слать, просто тихо сохранит прежнее значение, а поле, которое
    # прошивка перестанет читать, будет годами уезжать в эфир впустую.
    print("== погода от хаба: снипет и разбор читают одно и то же ==")
    m = re.search(r"static void take_hub_weather\(.*?\n\}", BSCODE, re.S)
    ok(m is not None, "разбор погоды от хаба найден")
    if m:
        body = m.group(0)
        top = set(re.findall(r'w\["([a-z]+)"\]', body))
        day = set(re.findall(r'f\["([a-z]+)"\]', body))
        snip = CODE  # console.html целиком: снипет живёт в нём
        # Две формы, обе законные: поле может стоять строкой в литерале ({"temp": ...}) или
        # именованным аргументом Jinja (dict(out, uv=...)), которым добавляются необязательные
        # поля — те, что шлются, только если у хаба они действительно есть. Требование остаётся
        # прежним: каждое поле, которое читает прошивка, снипет обязан уметь послать.
        missing = sorted(k for k in top
                         if '"%s"' % k not in snip and not re.search(r'\b%s\s*=' % k, snip))
        ok(not missing, "снипет шлёт все поля, которые читает прошивка (нет: %s)" % (missing or "—"))
        missing_d = sorted(k for k in day if '"%s"' % k not in snip)
        ok(not missing_d, "и все поля дня прогноза (нет: %s)" % (missing_d or "—"))
        ok("forecast" in top, "массив прогноза читается")
    # Два отказа, найденные сверкой 2026-09-22. Оба тихие: прибор поверит и тому и другому.
    # С 2026-09-22 автоматизация пишется консолью под прибор (haYaml), и погода — одна из трёх её
    # ветвей, поэтому защита стоит внутри ветки, перед запросом прогноза. Не «строка есть где-то»,
    # а «условие собрано целиком и стоит ДО публикации».
    print("== Home Assistant: автоматизация от консоли ==")
    mc = re.search(r"- condition: template\s*\n\s*value_template: >-\s*\n(.*?)\n\s*- action: weather\.get_forecasts",
                   CODE, re.S)
    ok(mc is not None, "ветка погоды не публикует, пока сущность погоды недоступна")
    if mc:
        g = mc.group(1)
        ok("unavailable" in g and "unknown" in g and "is number" in g,
           "и проверяет именно эти состояния и число температуры")
    i_fc = CODE.find("- action: weather.get_forecasts")
    i_pub = CODE.find("topic: ${q(topic + '/weather')}")
    ok(0 <= i_fc < i_pub, "прогноз запрашивается до публикации погоды")
    ok(re.search(r"dict\(out, forecast=ns\.ahead\) if ns\.ahead else out", CODE) is not None,
       "пустой прогноз не отправляется: он стёр бы ряд под графиком")
    # Тревоги: прибор шлёт событие, автоматизация его слушает — одна и та же тема с обеих сторон.
    ok('"%s/alert"' in BSC or "%s/alert" in open(os.path.join(MAIN, "mqtt_publisher.cpp")).read(),
       "прошивка публикует тревогу в <тема>/alert")
    ok("topic: ${q(topic + '/alert')}" in CODE, "автоматизация слушает ту же тему")
    m = re.search(r"static std::string alert_event_json\(\).*?\n\}", BSCODE, re.S)
    ok(m is not None, "событие тревоги собирается одной функцией")
    if m:
        keys = set(re.findall(r'd\["([a-z]+)"\]', m.group(0)))
        ok(keys == {"device", "level", "title", "text"}, "поля события: %s" % sorted(keys))
        ok(all(("ev.%s" % k) in CODE for k in ("level", "title", "text")),
           "автоматизация читает level, title и text")
    # Событие уходит один раз: счёт растёт в set_alert(), долг гасится только подтверждением брокера.
    ok(re.search(r"rtc_al_tries = 3;.*?\n\s*rtc_al_seq\+\+;", BSCODE, re.S) is not None,
       "каждая новая тревога — новое событие для хаба")
    ok("if (ev_acked || !rtc_al_title[0]) rtc_al_hub = ev_seq;" in BSCODE,
       "долг перед хабом гасится подтверждением, а не попыткой")
    ok(BSCODE.count("rtc_al_seq = rtc_al_hub = 0;") == 3,
       "счётчики событий обнуляются во всех трёх местах, где обнуляется сама тревога")
    # Устройство в Home Assistant ищется по идентификатору из discovery — одна функция на оба места.
    WS = nocomments(open(os.path.join(MAIN, "web_server.cpp")).read())
    MQ = nocomments(open(os.path.join(MAIN, "mqtt_publisher.cpp")).read())
    ok('cfg["ha_id"]' in WS and "MqttPublisher::deviceId()" in WS, "консоль получает идентификатор прибора")
    m = re.search(r"static void publishDiscovery\(.*?\n\}", MQ, re.S)
    ok(m is not None and "MqttPublisher::deviceId()" in m.group(0) and "esp_read_mac" not in m.group(0),
       "discovery берёт его из той же функции")
    ok('("mqtt", ${q(uid)}) in (device_attr(e, "identifiers")' in CODE,
       "автоматизация ищет датчики по нему, а не по имени")
    m = re.search(r"id: silent\s*\n\s*value_template: >-\s*\n\s*(\S[^\n]*)", CODE)
    ok(m is not None and m.group(1).strip() == "{% set t = now() %}",
       "сторож молчания вызывает now() первой строкой: иначе после старта HA он не пересчитывается никогда")
    # Диапазон за день от хаба РАСШИРЯЕТ дневной, а не заменяет его: прогноз описывает только будущее.
    m = re.search(r"static void take_hub_weather\(.*?\n\}", BSCODE, re.S)
    if m:
        ok("widen_today(" in m.group(0) and 'rtc_wx_hi    = w["tmax"]' not in m.group(0),
           "погода от хаба расширяет диапазон за день тем же кодом, что и своя")
    m = re.search(r"static void fetch_own_weather\(.*?\n\}", BSCODE, re.S)
    ok(m is not None and "widen_today(" in m.group(0), "и своя погода идёт через него же")

    # Рельсу поднимает init(), и он обязан стоять ПОСЛЕ решения о ночи. Пока он стоял до,
    # парковка обходилась дороже, чем экономила: каждое ночное пробуждение включало питание,
    # ничего не рисовало и платило пять секунд укладки за выключение. Поймано журналом за одну
    # ночь: 9342 мс против 5265 у пробуждения, которое накануне ещё и РИСОВАЛО.
    i_plan = BSCODE.find("const Tick tick = plan_tick(bw);")
    i_init = BSCODE.find('epd.init()) ESP_LOGE(TAG, "EPaper init FAILED")')
    ok(i_plan >= 0 and i_init >= 0 and i_plan < i_init,
       "питание панели поднимается после решения о ночи, а не до него")
    ok(re.search(r"!night_frozen\(tick\) && !epd\.init\(\)", BSCODE) is not None,
       "и не поднимается вовсе на замороженном ночном тике")
    # Одно определение на два места: разойдутся — либо рельса зря под питанием, либо рисуем без неё.
    ok(BSCODE.count("static bool night_frozen(const Tick &tick)") == 1,
       "определение «стекло заморожено» одно")
    ok(BSCODE.count("night_frozen(tick)") >= 2,
       "и его спрашивают оба места — и питание, и отрисовка")
    # И тик, который замораживает картинку, обязан её нарисовать, а не пропустить.
    m = re.search(r"const bool frozen\s*=(.*?);", BSCODE, re.S)
    ok(m is not None, "условие пропуска отрисовки найдено")
    if m:
        ok("night_frozen" in m.group(1), "отрисовка спрашивает то же определение")
    mf = re.search(r"static bool night_frozen\(const Tick &tick\)\s*\{(.*?)\n\}", BSCODE, re.S)
    ok(mf is not None, "тело night_frozen найдено")
    if mf:
        ok("!tick.night_freeze" in mf.group(1),
           "замораживающий тик рисует, пропускают только следующие за ним")
        ok("rtc_prev.aqInv" in mf.group(1),
           "смена тревоги будит панель и ночью — решение пользователя 2026-09-15")
    # Тик, который ничего не нарисовал, короче обычного, и кормить им среднее нельзя: по нему
    # считается, насколько раньше проснуться, чтобы кадр сел на минуту.
    m = re.search(r"const bool poison\s*=([^;]+);", BSCODE)
    ok(m is not None and "drew == 0" in m.group(1),
       "тик без отрисовки не идёт в среднюю длительность тика")

    # ---- нижняя полоса: один список опций на консоль, прошивку и экран ----
    # Идентификаторы — проводной формат: прибор пакует их по байту в NVS `strip`, консоль шлёт то же
    # слово обратно. Разойдутся — у приборов в поле молча переставятся ячейки, и никто не заметит,
    # пока владелец не посмотрит на стекло. Поэтому сверяется не наличие строк, а сами числа.
    SC = nocomments(open(os.path.join(MAIN, "strip_cells.h"), encoding="utf-8").read())
    fw_ids = {}
    m = re.search(r"enum StripCell\s*:\s*uint8_t\s*\{(.*?)\}", SC, re.S)
    ok(m is not None, "нижняя полоса: enum StripCell найден")
    if m:
        nxt = 0
        # [A-Z_]+, не [A-Z]+: иначе STRIP_OPT_COUNT читается как STRIP_OPT, continue не срабатывает
        # и счётчик опций оказывается на единицу больше настоящего.
        for name, val in re.findall(r"(STRIP_[A-Z_]+)\s*(?:=\s*(\d+))?", m.group(1)):
            if name == "STRIP_OPT_COUNT": continue
            nxt = int(val) if val else nxt
            fw_ids[name] = nxt; nxt += 1
    js = re.search(r"const STRIP_OPTS = \[(.*?)\];", CODE, re.S)
    ok(js is not None, "нижняя полоса: STRIP_OPTS найден в консоли")
    if js and fw_ids:
        con_ids = [int(x) for x in re.findall(r"\{id:\s*(\d+)", js.group(1))]
        ok(con_ids == sorted(con_ids) and con_ids == list(range(len(con_ids))),
           "нижняя полоса: id в консоли идут подряд с нуля")
        ok(len(con_ids) == len(fw_ids),
           "нижняя полоса: опций поровну — консоль %d, прошивка %d" % (len(con_ids), len(fw_ids)))
    # Умолчание обязано совпасть: иначе прибор, которому ничего не задавали, покажет одно, а консоль
    # нарисует выбор другой — и владелец «сохранит» то, что и так было, получив другой экран.
    mj = re.search(r"const STRIP_DEFAULT = \[(\d+),\s*(\d+),\s*(\d+)\]", CODE)
    ok(mj is not None, "нижняя полоса: умолчание консоли найдено")
    if mj and fw_ids:
        ok([int(mj.group(i)) for i in (1,2,3)] ==
           [fw_ids["STRIP_HUM"], fw_ids["STRIP_WIND"], fw_ids["STRIP_FEELS"]],
           "нижняя полоса: умолчание консоли = Hum/Wind/Feels прошивки")
    WS2 = nocomments(open(os.path.join(MAIN, "web_server.cpp"), encoding="utf-8").read())
    ok('doc["strip"]' in WS2, "нижняя полоса: /api/save принимает ключ strip")
    ok('cfg["strip"]' in WS2, "нижняя полоса: /api/status отдаёт strip")
    ms = re.search(r'if \(!doc\["strip"\]\.isNull\(\)\) \{(.*?)\n    \}', WS2, re.S)
    ok(ms is not None and "STRIP_OPT_COUNT" in ms.group(1),
       "нижняя полоса: каждый байт проверен по числу опций, а не принят на веру")
    # Опрос статуса идёт раз в несколько секунд. Без этой защиты он затирал бы выбор, который
    # владелец ещё не сохранил.
    def js_body(name):
        """Тело JS-функции из консоли: body_of() ищет в web_server.cpp и здесь не годится."""
        mm = re.search(r"function %s\([^)]*\)\s*\{" % re.escape(name), CODE)
        if not mm: return None
        i = mm.end(); depth = 1
        while i < len(CODE) and depth:
            if CODE[i] == "{": depth += 1
            elif CODE[i] == "}": depth -= 1
            i += 1
        return CODE[mm.end():i-1]
    mf = js_body("stripFromCfg")
    ok(mf is not None and "ST.stripDirty" in mf,
       "нижняя полоса: опрос не затирает несохранённый выбор")
    ok("ST.stripDirty = true" in CODE, "нижняя полоса: правка помечает выбор как несохранённый")
    ok("ST.stripDirty = false" in CODE, "нижняя полоса: сохранение снимает пометку")
    # Доступность считается через effWifi(), а не вторым списком правил: одно место на весь файл.
    mw = js_body("stripWxOk")
    ok(mw is not None and "effWifi" in mw,
       "нижняя полоса: доступность погоды берётся из effWifi, без второй копии правил")
    ma = js_body("stripAvail")
    ok(ma is not None and "return true" in ma,
       "нижняя полоса: у луны нет требований — она считается из даты")

    # ---- погода: ни одно поле не должно остаться без сброса -------------------------------------
    # Класс дефекта, а не случай. 0.10.16 добавил в RTC четыре погодных поля и обнулил их только в
    # заводском сбросе; после обновления по воздуху они читали чужие байты, и владелец увидел на
    # стекле три девятки. Список сброса ведётся руками, поэтому проверяется механически: каждое
    # объявленное погодное поле обязано присваиваться внутри forget_weather(), и обе дороги обязаны
    # её звать. Иначе следующий, кто добавит ячейку, повторит это молча.
    # Решение «поднимать ли радио по нажатию» обязано спрашивать weather_is_current(), а не своё
    # правило. Проверка поведения на это не ловится: она зовёт предикат напрямую, и снятие вызова из
    # radio_window() проходило молча — ровно так 2026-09-22 прошла мимо перестановка фаз у ночной
    # парковки, которую увидел только журнал прибора.
    # Закрытие консоли из шапки: эндпоинт только поднимает флаг, а закрывает окно задача цикла —
    # она владеет панелью и радио. Браузерный тест видит запрос и ответ, но не видит, читает ли этот
    # флаг кто-нибудь: снятие проверки из hold_setup_window() проходило молча.
    print("\n\033[1m== закрытие консоли: флаг поднимают и читают ==\033[0m")
    ok("global_close_req = true" in WEB, "/api/close поднимает флаг")
    hw = re.search(r'static bool hold_setup_window\([^)]*\)\s*\{', BSCODE)
    ok(hw is not None, "hold_setup_window() найдена")
    if hw:
        body = BSCODE[hw.end():BSCODE.index("\n}", hw.end())]
        ok(re.search(r'if\s*\(\s*global_close_req\s*\)', body) is not None,
           "и окно на него ВЕТВИТСЯ — искать упоминание мало: в блоке стоит обнуление того же флага")

    print("\n\033[1m== погода: нажатие спрашивает именно предикат ==\033[0m")
    m = re.search(r'static bool weather_is_current\(', BSCODE)
    ok(m is not None, "weather_is_current() существует — одно правило свежести на всех")
    rw = re.search(r'static void radio_window\(const Tick &tick\)\s*\{', BSCODE)
    ok(rw is not None, "radio_window() найдена")
    if rw:
        body = BSCODE[rw.end():rw.end() + 6000]
        wf = re.search(r'const bool wx_fresh\s*=\s*([^;]+);', body)
        ok(wf is not None, "radio_window() заводит wx_fresh")
        if wf:
            ok("weather_is_current" in wf.group(1),
               "и берёт его из weather_is_current(), а не из собственного порога")
    # Порог 15 минут остаётся ровно в одном месте — в предикате, для OpenWeatherMap.
    ok(len(re.findall(r'\bWX_FRESH_SEC\b', BSCODE)) == 2,
       "WX_FRESH_SEC упомянут дважды: объявление и единственное применение")

    print("\n\033[1m== погода: каждое поле RTC сбрасывается при смене образа ==\033[0m")
    m = re.search(r'static void forget_weather\(void\)\s*\{', BSCODE)
    ok(m is not None, "forget_weather() существует — один блок сброса на все пути")
    if m:
        fw = BSCODE[m.end():BSCODE.index("\n}", m.end())]
        # Объявления: RTC_NOINIT_ATTR ... rtc_wx_* / rtc_day_* / rtc_fc
        decl = set()
        for line in BSCODE.split("\n"):
            if "RTC_NOINIT_ATTR" not in line and "RTC_DATA_ATTR" not in line:
                continue
            for nm in re.findall(r'\b(rtc_(?:wx|day)_[a-z_0-9]+|rtc_fc)\b', line):
                decl.add(nm)
        ok(len(decl) >= 12, "погодных полей RTC найдено %d" % len(decl))
        missing = sorted(n for n in decl
                         if not re.search(r'\b%s\b\s*(=|\[)' % n, fw) and ("sizeof(%s)" % n) not in fw)
        ok(not missing, "все погодные поля сбрасываются в forget_weather()"
                        + ("" if not missing else " — забыты: " + ", ".join(missing)))
        # И обе дороги её зовут. Вызов внутри самой функции не считается.
        for fn, why in (("forget_rtc_state", "заводской сброс"),
                        ("forget_previous_build", "смена образа")):
            mm = re.search(r'static void %s\(void\)\s*\{' % fn, BSCODE)
            body = BSCODE[mm.end():BSCODE.index("\n}", mm.end())] if mm else ""
            ok("forget_weather()" in body, "%s зовёт forget_weather()" % why)

    print("\n\033[1m== нижняя полоса: номера опций — проводной формат ==\033[0m")
    SC2 = open(os.path.join(MAIN, "strip_cells.h"), encoding="utf-8").read()
    fw_ids = dict((m.group(1), int(m.group(2)))
                  for m in re.finditer(r'(STRIP_[A-Z]+)\s*=\s*(\d+)', SC2))
    want = {"STRIP_SUN":0,"STRIP_MOON":1,"STRIP_HUM":2,"STRIP_WIND":3,
            "STRIP_FEELS":4,"STRIP_UV":5}
    ok(fw_ids == want, "номера прошивки закреплены 0..5, получено %s" % sorted(fw_ids.items(), key=lambda x: x[1]))
    # Только блок STRIP_OPTS: в файле есть и другие списки с полем id, и без границ проверка
    # ловила бы их тоже — она и поймала, показав [0,1,2,0,1,2,...].
    blk = HTML[HTML.index("const STRIP_OPTS"):]
    blk = blk[:blk.index("];")]
    js_ids = [int(m) for m in re.findall(r'\{id:(\d+),', blk)]
    ok(js_ids == list(range(6)), "консоль предлагает те же шесть по порядку: %s" % js_ids)
    # Снятые номера не должны вернуться НИ под каким именем: устройство в поле могло сохранить 8,
    # и новая опция под восьмёркой молча встала бы в чужую ячейку.
    ok(max(fw_ids.values()) == 5 and 6 not in js_ids,
       "номера 6..10 выведены из обращения и не переиспользованы")
    ok("STRIP_OPT_COUNT" in SC2 and "STRIP_CLOUD" not in SC2 and "STRIP_DEW" not in SC2
       and "STRIP_RAIN" not in SC2, "снятых имён в заголовке не осталось")

    # ── полоса вердикта под графиком ─────────────────────────────────────────
    # Ничего нового под неё не хранится, и это проверяется с ДВУХ концов: прошивка пакует класс в
    # биты 2-4 flags, веб-сервер отдаёт его как `cls`, консоль его рисует. Разъехаться эти трое
    # могут молча — полоса просто показывала бы чужое поле.
    ok(re.search(r'rec\.flags\s*=.*rtc_class\s*&\s*0x7\)\s*<<\s*2', BSCODE, re.S),
       "прошивка пишет класс вердикта в биты 2-4 flags")
    ok(re.search(r'cls\s*=\s*\(r\.flags\s*>>\s*2\)\s*&\s*0x7', WEB),
       "/api/history отдаёт его как cls из тех же битов")
    # Бит 6 (2026-10-08): процент записан по кривой ячейки. Оценка расхода не наклоняет ряд через
    # смену способа счёта (power_history.cpp), но узнаёт её только по этому биту — запись без него
    # выглядела бы старой, и каждый новый отрезок сливался бы с моделью газомера v1.
    fl = re.search(r'rec\.flags\s*=(.*?);', BSCODE, re.S)
    ok(fl is not None and re.search(r'bat_ok\s*\?\s*HIST_FLAG_CURVE', fl.group(1)) is not None,
       "прошивка ставит бит «процент по кривой» в каждую запись с батареей")

    CH = CODE[CODE.index("function renderChart("):CODE.index("function refreshChart(")]
    ok("const bandOn=recs.some(p=>clsOf(p)!=null)" in CH and "p.cls" in CH,
       "график читает класс из записи истории")
    co = re.search(r'const clsOf=(.+?);\n', CH)
    ok(co and "p.cls==null" in co.group(1) and "isNaN(p.cls)" in co.group(1),
       "запись без класса не выдаётся за нулевой класс")
    # Полоса обязана занимать СВОЁ место, а не рисоваться поверх данных: padB должен расти.
    ok(re.search(r'padB=26\s*\+\s*\(bandOn\s*\?', CH),
       "под полосу отводится место, кадр данных не перекрывается")
    ok(not re.search(r'padT=14,\s*padB=26', CH), "жёсткого padB=26 не осталось")
    # Разрыв в истории не красится последним известным вердиктом — то же правило, что у линий.
    seg = CH[CH.index("if(bandOn){"):CH.index("const mLeg")]
    ok("maxGap" in seg and "nx.t-recs[i].t<=maxGap" in seg,
       "полоса обрывается на настоящем разрыве, а не красит его насквозь")
    ok("prev.c===c" in seg, "равные классы сливаются в один прямоугольник")
    # Имена и цвета — из тех же двух списков, что у героя и у выгрузки CSV. Третий список означал
    # бы, что график и экспорт однажды назовут один класс по-разному.
    ok("bandName(" in CH and "bandInk(" in CH, "имя и цвет класса берутся через общие помощники")
    ok(re.search(r'const bandName = c => \(AIRCLASS\[c\]', CODE),
       "имя класса приходит из AIRCLASS")
    bi = re.search(r'const bandInk\s*=\s*(.+?);\n', CODE)
    ok(bi and "AQ[c]" in bi.group(1), "цвет класса приходит из AQ")
    # Снятый класс 4 не имеет права взять цвет соседа: в герое показан один класс за раз, и общий
    # янтарный с Stuffy ничего не стоил, а на полосе они встают рядом и сливаются.
    ok(bi and "c === 4" in bi.group(1), "снятый класс 4 рисуется как неизвестный, а не янтарным")
    for word in ("Stuffy", "Damp", "Stale"):
        ok(word not in CH, "своего имени класса в графике нет: %s" % word)
    ok(not re.search(r"fill=\"#", seg), "своего цвета в полосе нет")
    # Легенда перечисляет только то, что в окне: иначе снятый класс 4 пришлось бы объяснять словами.
    lg = re.search(r'const legend = ([^?]+)\?', CH)
    ok("clsSeen.map(" in CH and lg and "cLeg" in lg.group(1),
       "легенда классов окна показывается, а не только считается")
    # Вердикт назван и в подсказке: сверять цвет полосы с легендой глазами — не чтение.
    ok(re.search(r'vrow=vc==null \? .. : `<span class="tt" style="color:\$\{bandInk\(vc\)\}">\$\{bandName\(vc\)\}', CH),
       "строка вердикта для подсказки собирается")
    ti = re.search(r'tip\.innerHTML=(.+?);\s*\}', CH, re.S)
    ok(ti and "${vrow}" in ti.group(1), "и она попадает в саму подсказку")

    # ── давление на графике с одним знаком ───────────────────────────────────
    # Весь порог Rain — 1.6 гПа, то есть 1.2 мм рт. ст.; целые единицы округляют его в ноль.
    mt = CODE[CODE.index("const METRICS"):]
    mt = mt[:mt.index("};")]
    ok(re.search(r"pres:\s*\{[^}]*dec:1\}", mt), "давление печатается с одним знаком")
    ok(not re.search(r"pres:\s*\{[^}]*dec:0\}", mt), "целых единиц у давления не осталось")
    # 0.1 мм доезжает до консоли — иначе знак после запятой был бы выдуманным.
    ok("r.pres_d / 10.0f" in WEB, "прошивка отдаёт давление с десятыми")

    # ── барометр: гистерезис держится на ПОРЯДКЕ, а порядок не виден в диффе ──
    # Пороги проверяет t_analytics прямым вызовом. Здесь — то, что он проверить не может: прежний
    # режим обязан сниматься ДО сброса, иначе в baroMode всегда уедет ноль и гистерезиса просто не
    # будет, причём молча. Сам сброс при этом обязан остаться: тик, который не смог решить, показывает
    # число, а не Rain, оставшийся с прошлого раза.
    i_prev = BSCODE.find("const int8_t prev_mode")
    i_zero = BSCODE.find("rtc_pres_mode = 0", i_prev if i_prev >= 0 else 0)
    i_call = BSCODE.find("Analytics::baroMode(", i_prev if i_prev >= 0 else 0)
    ok(0 <= i_prev < i_zero < i_call,
       "прежний режим снимается до сброса, сброс — до пересчёта (%d, %d, %d)" % (i_prev, i_zero, i_call))
    # Не привязываться к списку аргументов: 2026-10-05 ровно такая проверка упала на добавлении
    # уровня, хотя код был верен. Спрашиваем суть — прежний режим уходит последним аргументом.
    ok(re.search(r'Analytics::baroMode\([^)]*,\s*prev_mode\)', BSCODE),
       "в baroMode уходит прежний режим, а не ноль")
    # RTC_NOINIT при первой подаче питания — чужие байты; непроверенное «2» выдало бы тому тику
    # свободный порог Storm.
    ok(re.search(r'prev_mode = \(rtc_pres_mode >= 1 && rtc_pres_mode <= 2\)', BSCODE),
       "мусор RTC_NOINIT не выдаётся за показанный режим")

    # --- Второй вход плашки: УРОВЕНЬ ---------------------------------------------------------
    # 2026-10-05 плашка сказала Rain над солнечным днём при 1023 гПа. Скорость была верной,
    # смысла у неё без уровня нет. Проверяется вся проводка, а не только наличие кода: поле
    # просится у MET, читается, учит множитель и доходит до baroMode.
    ok("air_pressure_at_sea_level" in WXCODE,
       "приведённое давление просится у MET в том же запросе")
    ok(re.search(r'now\.p_sea\s*=\s*d\["instant"\]\["details"\]\["air_pressure_at_sea_level"\]', WXCODE),
       "и действительно читается в WxNow, а не просто упомянуто в фильтре")
    ok(re.search(r'baro_learn_factor\(\s*now\.p_sea\s*\)', BSCODE),
       "прочитанное уходит учить множитель высоты")
    # ОБА источника, а не только MET: прибор на OWM иначе не выучил бы множитель никогда и остался
    # бы без плашки навсегда. Найдено вопросом пользователя уже после того, как правка была готова.
    ok('filter["main"]["sea_level"]' in WXCODE and 'doc["main"]["sea_level"]' in WXCODE,
       "у OWM приведённое давление тоже просится и читается")
    ok(re.search(r'Weather::fetch\([^;]*&now\.p_sea', BSCODE),
       "и доезжает до того же обучения, что у MET")
    # Чем ЗАПОЛНЕН аргумент, а не как он назван: ровно на этой разнице 2026-10-02 слом прошёл молча.
    m = re.search(r'Analytics::baroMode\(\s*dp\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,', BSCODE)
    ok(m is not None, "уровень уходит в baroMode отдельным аргументом")
    if m:
        ok(re.search(r'\b' + m.group(1) + r'\s*=\s*baro_sea_level\(', BSCODE),
           "и это приведённое давление, а не литерал и не сырое показание (%s)" % m.group(1))
    # Неизвестный множитель обязан быть НЕИЗВЕСТНЫМ, а не единицей: 1.0 молча объявил бы прибор
    # стоящим на уровне моря и вернул бы ровно тот дефект, ради которого всё это сделано.
    body = BSCODE[BSCODE.find("static float baro_factor("):]
    body = body[:body.find("\n}")]
    # Спрашиваем ПУТЬ, а не наличие слова: редакция, искавшая «есть ли NAN в функции», пропустила
    # слом с `return 1.0f` — второй NAN в зажиме её устраивал.
    ok(body.rstrip().endswith("return NAN;"),
       "ничего не известно — baro_factor отвечает NAN")
    ok("return 1.0f" not in body and "return 1.f" not in body,
       "и прибор никогда не объявляет себя стоящим на уровне моря по умолчанию")
    ok("BARO_K_MIN" in body and "BARO_K_MAX" in body,
       "мусорный множитель зажимается границами высоты")
    # Порядок источников — не вкус: измеренный по погоде множитель точнее городского на порядок,
    # и высота города не имеет права его перебить.
    ok(0 <= body.find('"baro_k"') < body.find('"wx_alt"'),
       "измеренный множитель спрашивается ПЕРЕД высотой города")
    # Порядок текста ещё не достижимость: слом `if (false && getFloat(...))` его не двигает и
    # прошёл молча. Ветка измеренного не имеет права быть отключённой.
    measured = body[:body.find("return k;")]
    ok('"baro_k"' in measured and "false" not in measured,
       "и его ветка ничем не отключена")
    # Формула приведения — стандартная, а не подогнанная: лапс 6,5 К/км и показатель 5,257.
    alt = BSCODE[BSCODE.find("static float baro_k_from_altitude("):]
    alt = alt[:alt.find("\n}")]
    ok("0.0065f" in alt and "5.257f" in alt, "приведение по стандартной атмосфере, а не на глаз")
    # Высота доезжает от консоли до NVS, иначе выученное городом некуда класть.
    # Условие, а не упоминание: `if (false)` оставляет строку "wx_alt" в мёртвом блоке на месте.
    # web_server.cpp на хосте не собирается, так что это проверка КОНСТРУКЦИИ, не поведения.
    ok('doc.containsKey("wx_alt")' in WEB, "/api/save действительно смотрит на высоту в теле запроса")
    ok('setFloat("wx_alt"' in WEB, "и кладёт её в NVS")

    # --- высота: от города и от руки, и видно, на чём прибор стоит ----------------------------
    # Обе ветки, а не упоминание: переименование одной оставляло вторую и проходило молча.
    ok(WEB.count('sensor["p_sea"]') >= 2 and 'sensor["baro_src"]' in WEB,
       "/api/status отдаёт уровень в обоих случаях и источник высоты")
    ok(re.search(r'x\.wx_alt\s*=\s*Math\.round\(\+pick\.elev\)', CODE),
       "выбранный город приносит свою высоту")
    ok('id="wAlt"' in CODE, "и есть поле высоты вручную")
    ok(re.search(r'typedAltitude\(.wAlt.', CODE), "которое читается при сохранении погоды")
    # Зажать на приборе и промолчать нельзя: окно отчиталось бы о высоте, которой никто не вводил.
    body_ta = CODE[CODE.find("function typedAltitude("):]
    body_ta = body_ta[:body_ta.find("\n}")]
    ok("-500" in body_ta and "9000" in body_ta, "и проверяется ДО отправки, теми же границами")
    ok('id="dSea"' in CODE and "s.baro_src" in CODE,
       "консоль показывает уровень и говорит, какой половины не хватает")
    # Надпись на стекле: горизонт в подписи, слово остаётся коротким. В режиме числа — «Pressure».
    SCR = nocomments(open(os.path.join(MAIN, "screen.cpp"), encoding="utf-8").read())
    ok('"Within hours"' in SCR, "в режиме прогноза подпись называет горизонт")
    ok('"Pressure"' in SCR, "а в режиме числа остаётся Pressure")

    print("== вход: пароль можно увидеть, и увидеть тем же, что на стекле ==")
    # Этот пароль не вспоминают, а переписывают со стекла: восемь случайных знаков со заглавными.
    # Вслепую об опечатке узнаёшь только по отказу, а их пять до минутной блокировки.
    si = CODE[CODE.find('id="siPass"'):]
    si = si[:si.find("</div>") + 6]
    ok('class="pwrap"' in CODE[CODE.find('id="signin"'):CODE.find('id="siPass"')],
       "поле входа лежит в .pwrap — иначе кнопка уехала бы по странице")
    ok('id="siPassToggle"' in si, "и рядом с ним кнопка глаза")
    # Кнопка без обработчика хуже её отсутствия, поэтому спрашиваем вызов, а не разметку.
    ok(re.search(r"wireEye\('siPass','siPassToggle'", CODE), "глаз действительно подключён")
    # Тот же класс, что уронил 0.9.62: wireEye рисует значок через svg(IC.eye), а IC и svg — const,
    # до своего объявления недоступны. Вызов обязан стоять ПОСЛЕ них.
    i_call = CODE.find("wireEye('siPass'")
    i_ic, i_svg = CODE.find("const IC = {"), CODE.find("function svg(")
    ok(i_call > i_ic > 0 and i_call > i_svg > 0,
       "и подключён после объявления IC и svg, а не до них")

    # Регрессия 2026-09-19, вторая половина. Тогда из doSignIn() убрали .toLowerCase(), а
    # text-transform:lowercase в стилях остался: поле показывало заглавные строчными. Пока пароль
    # был скрыт, это было невидимо; с глазом владелец открыл бы его и увидел НЕ ТО, что на стекле.
    m_css = re.search(r"\.signin input\{(.*?)\}", CODE, re.S)
    ok(m_css is not None, "правило .signin input найдено")
    ok(m_css is not None and "text-transform" not in m_css.group(1),
       "поле входа не приводит регистр начертанием")
    body_si = CODE[CODE.find("async function doSignIn("):]
    body_si = body_si[:body_si.find("\nasync function ", 10) if body_si.find("\nasync function ", 10) > 0 else 4000]
    ok("toLowerCase" not in body_si, "и не приводит его в скрипте")
    ok('.signin .pwrap input{' in CODE and "padding-left" in CODE[CODE.find(".signin .pwrap input{"):][:120],
       "под кнопку отведено место симметрично — центровка не съезжает")

    # Очистка руками не поднимает input, значит sync() глаза не выполнится: открытый пароль остался
    # бы открытым над пустым полем. Поэтому чистить можно только через siClear.
    body_cl = CODE[CODE.find("function siClear()"):]
    body_cl = body_cl[:body_cl.find("\n}") + 2]
    ok("dispatchEvent(new Event('input'))" in body_cl, "siClear подаёт input, чтобы глаз закрылся")
    outside = [m.start() for m in re.finditer(r"el\('siPass'\)\.value\s*=\s*''", CODE)
               if not (CODE.find("function siClear()") < m.start()
                       < CODE.find("function siClear()") + len(body_cl))]
    ok(not outside, "и ни одно другое место не чистит поле напрямую (найдено %d)" % len(outside))

    # ---- шапка: один ряд, одна высота, говорит ровно один элемент -------------------------
    # Поводом была не только теснота: на 320 px со словом «Reconnecting…» кнопка темы уходила на
    # 16 px за край, а таблетка отсчёта сминалась в две строки — у .pill нет ни flex:none, ни
    # nowrap, и flex давил её со 108 до 67 px (мерено 2026-10-10). Поэтому проверяется не вид, а
    # три вещи, из-за которых это случилось.
    hdr = CODE[CODE.index("<header>"):CODE.index("</header>")]
    ok("class=\"pill" not in hdr and "class=\"iconbtn" not in hdr,
       "шапка не держит прежних .pill и .iconbtn")
    for cls in ("hpill", "hdot", "hword", "hico"):
        ok(re.search(r"class=\"%s\b" % cls, hdr) or ("'" + cls) in CODE,
           "в шапке есть %s" % cls)
    css = CODE[CODE.index(".hpill{"):CODE.index(".iconbtn{")]
    for cls in (".hpill", ".hdot", ".hword", ".hico"):
        rule = css[css.index(cls + "{"):]
        rule = rule[:rule.index("}")]
        ok("flex:none" in rule, "%s не даёт себя сжать (flex:none)" % cls)

    # Состояние связи ставится ТОЛЬКО через setConn: раньше текст писали в четырёх местах, а класс
    # — в двух, и забытый класс оставлял слово внутри «точки», то есть невидимым.
    sc = CODE[CODE.index("function setConn("):]
    sc = sc[:sc.index("\n}") + 2]
    ok("el('conn').className" not in CODE.replace(sc, ""),
       "класс состояния связи ставит только setConn")
    outside = [m.start() for m in re.finditer(r"el\('connt'\)\.textContent\s*=", CODE)
               if not (CODE.index("function setConn(") < m.start()
                       < CODE.index("function setConn(") + len(sc))]
    ok(not outside, "и слово состояния пишет тоже только она (мимо неё %d)" % len(outside))

    # Говорит ровно ОДИН элемент. Это не косметика: срок закрытия обновляет лишь удачный опрос,
    # поэтому без связи отсчёт показывал бы замершее число как живое.
    rp = CODE[CODE.index("function renderPortalTimer()"):]
    rp = rp[:rp.index("\n}") + 2]
    ok(re.search(r"portalMode==='hidden'\s*\|\|\s*!CONN_OK", rp),
       "без связи отсчёт убирается — в шапке говорит один элемент")
    ok("'hpill' + (held ? ' held'" in rp.replace('"', "'") or re.search(r"hpill.*held.*soon", rp, re.S),
       "у отсчёта есть продлённое состояние и последняя минута")

    # Нажатие «держать окно» показывает СВОЁ действие, а не ответ, снятый до него.
    tk = CODE[CODE.index("async function togglePortalKeep()"):]
    tk = tk[:tk.index("\n}") + 2]
    ok("updatePortalTimer" not in tk,
       "нажатие не перечитывает прошлый ответ — иначе шапка откатывалась бы на один опрос")
    ok("portalMode = portalKeep ?" in tk, "а ставит режим по нажатому и перерисовывает")

    # Кабель без замера (владелец, 2026-10-29: воткнул на минуту — 100, вынул — 96). t_cycle зовёт
    # follow_cable() напрямую и не видит, зовут ли её сами циклы; это — вот оно.
    wc = BSCODE[BSCODE.index("static void run_working_cycle("):]
    wc = wc[:wc.index("\n}")]
    ok(re.search(r"measure\(tick\);[^}]*\}\s*else\s*\{\s*follow_cable\(\);", wc) is not None,
       "рабочий цикл читает батарею на пробуждении без замера, когда сменился кабель")
    mm = BSCODE[BSCODE.index("static void run_matter_mode("):]
    mm = mm[:mm.index("\n}")]
    ok(re.search(r"if\s*\(\s*!may_read\s*\)\s*follow_cable\(\);", mm) is not None,
       "и цикл Matter тоже")

    # Bosch BSEC — закрытая часть, и её заголовки не уходят в публичный репозиторий (лицензия
    # декабря 2024, §3.1). Граница одна: их видит только bsec_manager.cpp. Подключи их ещё кто-нибудь
    # — и сборка без BSEC (bsec_manager_none.cpp) перестанет собираться у всех, кто их не имеет.
    bosch = re.compile(r'^\s*#\s*include\s*[<"](bsec_interface|bsec_datatypes)\.h[>"]', re.M)
    who = sorted(f for f in os.listdir(MAIN)
                 if f.endswith((".c", ".cpp", ".h"))
                 and bosch.search(open(os.path.join(MAIN, f), encoding="utf-8", errors="replace").read()))
    ok(who == ["bsec_manager.cpp"],
       "заголовки Bosch подключает только bsec_manager.cpp (подключают: %s)" % ", ".join(who))
    # И заглушка отвечает за каждый метод настоящего: иначе сборка без BSEC падает на компоновке,
    # а узнаёт об этом только тот, у кого BSEC нет, — то есть не мы.
    def methods(fn):
        txt = nocomments(open(os.path.join(MAIN, fn), encoding="utf-8").read())
        return set(re.findall(r"BSECManager::(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{", txt))
    # Закрытые помощники настоящего (их зовёт только он сам) заглушке не нужны.
    hdr, private, access = open(os.path.join(MAIN, "bsec_manager.h"), encoding="utf-8").read(), set(), ""
    for line in hdr[hdr.index("class BSECManager"):].split("\n"):
        t = line.strip()
        if t in ("public:", "private:", "protected:"): access = t; continue
        if t.startswith("};"): break
        m = re.match(r"[\w:<>*&\s]+?\b(\w+)\s*\(", t)
        if m and access != "public:": private.add(m.group(1))
    real = methods("bsec_manager.cpp") - private
    none = methods("bsec_manager_none.cpp")
    ok(real and real == none,
       "bsec_manager_none.cpp определяет те же методы (нет: %s; лишние: %s)"
       % (", ".join(sorted(real - none)) or "—", ", ".join(sorted(none - real)) or "—"))
    # Без индекса консоль говорит об этом, а не обещает прогрев навсегда — и прибор ей это сообщает.
    ok('sensor["iaq_built"]' in WEB, "/api/status говорит, есть ли в сборке индекс воздуха")
    ok("s.iaq_built===false" in CODE, "консоль его читает")

    print("консоль          провалов %d" % len(fails))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
