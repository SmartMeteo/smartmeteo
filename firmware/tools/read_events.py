#!/usr/bin/env python3
"""Read and analyse the device's flight recorder.

The device is awake for about a second at a time, so its serial port does not meaningfully exist
while it is doing its job. Everything here therefore comes over the console:

    python3 tools/read_events.py smartmeteo.local            # pull and summarise
    python3 tools/read_events.py smartmeteo.local --raw ev.bin   # keep the bytes for later
    python3 tools/read_events.py --file ev.bin            # analyse a saved dump
    python3 tools/read_events.py smartmeteo.local --since 12345   # only what is new

Open the console first with a long press; it stays up for three minutes.

Since 2026-09-18 the log sits behind the console password, so this signs in the way the page does:
ask for a nonce, answer with SHA-256 of "<nonce>:<password>", keep the cookie. The password never
leaves this machine in the clear — same reason the page does it this way, since the same secret
opens the device's own Wi-Fi. It is read from --password, then SMARTMETEO_PASSWORD, then a prompt;
the device prints it on its screen while the console window is open.
"""
import argparse, getpass, hashlib, http.cookiejar, json, os, struct, sys, time, urllib.request
from collections import Counter, defaultdict
from datetime import datetime, timezone

REC = struct.Struct("<IIHBBBBBBHHHHHbbI")     # 32 bytes, mirrors EvtRec in main/evt_log.h
HDR = struct.Struct("<4sHHIII")               # 20 bytes, written by /api/events.bin

KIND = {0: "wake", 1: "boot", 2: "sensor", 3: "gas", 4: "radio",
        5: "draw", 6: "button", 7: "alert", 8: "power", 9: "ota", 10: "note", 11: "sec"}

# EVT_SEC detail (evt_log.h, EvtSec). Added 2026-09-18 for EN 18031-2 [LGM]: who got into the
# console, whether they failed first, and what they changed. The low byte carries the code.
SEC = {1: "вход выполнен", 2: "неверный пароль", 3: "вход заблокирован на минуту",
       4: "пароль прибора изменён", 5: "запрошен заводской сброс", 6: "обновление установлено",
       7: "откат на предыдущий образ", 8: "запрошено удаление из умного дома",
       9: "открыто окно консоли"}


def sec_text(detail):
    code = detail & 0xFF
    name = SEC.get(code, "код %d" % code)
    extra = detail >> 8
    if code == 1 and extra:
        name += " — запомненный браузер, пароль не спрашивали"
    elif code == 4 and extra:
        name = "запомненные браузеры забыты"
    elif code == 2 and extra:
        name += " (%d-й подряд)" % extra
    elif code == 6 and extra:
        name += " (seq %d)" % extra
    elif code == 9:
        name += " — своя точка доступа" if extra else " — домашняя сеть"
    return name
WAKE = {0: "?", 1: "cold", 2: "timer", 3: "button", 4: "usb", 5: "restart"}
FLAG = [(1, "usb"), (2, "night"), (4, "measure"), (8, "radio"),
        (16, "forced"), (32, "first"), (64, "drew"), (128, "full")]
SENS = [(1, "scd"), (2, "bme"), (4, "bsec"), (8, "gauge"),
        (16, "nvs"), (32, "hist"), (64, "wifi"), (128, "mqtt")]
# esp_reset_reason_t целиком. Коды 10-15 отсутствовали, и сброс от открытия порта (USB, 11) —
# самый частый на столе — печатался голым числом, которое приходилось искать в заголовках IDF.
RESET = {0: "UNKNOWN", 1: "POWERON", 2: "EXT", 3: "SW", 4: "PANIC", 5: "INT_WDT", 6: "TASK_WDT",
         7: "WDT", 8: "DEEPSLEEP", 9: "BROWNOUT", 10: "SDIO", 11: "USB", 12: "JTAG",
         13: "EFUSE", 14: "PWR_GLITCH", 15: "CPU_LOCKUP"}
NOTE = {1: "MQTT не отдал", 2: "калибровка сохранена", 3: "калибровка стёрта",
        4: "ЗАВОДСКОЙ СБРОС: стёрто всё, кроме калибровки CO2 в датчике"}


def sign_in(host, password, timeout=15):
    """Returns an opener carrying the session cookie, or exits saying why it could not."""
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    def post(path, payload):
        req = urllib.request.Request("http://%s%s" % (host, path),
                                     data=json.dumps(payload).encode(),
                                     headers={"Content-Type": "application/json"})
        return opener.open(req, timeout=timeout)
    try:
        with post("/api/auth/challenge", {}) as r:
            nonce = json.loads(r.read()).get("nonce", "")
    except urllib.error.HTTPError as e:
        if e.code == 429:
            sys.exit("прибор заперт на минуту после пяти неверных попыток — подождите и повторите")
        sys.exit("не удалось получить nonce: %s" % e)
    except OSError as e:
        sys.exit("прибор не отвечает (%s). Консоль открыта? Она живёт три минуты после нажатия" % e)
    if len(nonce) != 32:
        sys.exit("прибор вернул некорректный nonce")
    digest = hashlib.sha256(("%s:%s" % (nonce, password)).encode()).hexdigest()
    try:
        with post("/api/auth", {"nonce": nonce, "resp": digest}) as r:
            if not json.loads(r.read()).get("ok"):
                sys.exit("пароль не подошёл")
    except urllib.error.HTTPError as e:
        sys.exit("вход отклонён: HTTP %d%s" % (e.code, " — заперто на минуту" if e.code == 429 else ""))
    return opener


def fetch(host, since, password, timeout=120):
    opener = sign_in(host, password)
    url = f"http://{host}/api/events.bin" + (f"?since={since}" if since else "")
    try:
        with opener.open(url, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 401:
            sys.exit("прибор ответил 401 уже после входа — окно консоли закрылось, откройте заново")
        raise


def wrap_partition(raw):
    """A raw dump of the `events` partition, given the header /api/events.bin would have added.

    On flash the log is nothing but an array of 32-byte records — the frame is built by the web
    handler. Reading the partition over the cable therefore needs no console window and no
    password, which matters: the window lives three minutes and the password is only on the glass.

        esptool.py --port /dev/cu.usbmodem* read_flash 0x7C0000 0x40000 ev.bin
        python3 tools/read_events.py --file ev.bin --partition
    """
    if len(raw) % REC.size:
        raw = raw[:len(raw) - (len(raw) % REC.size)]
    seqs = [struct.unpack_from("<I", raw, o)[0] for o in range(0, len(raw), REC.size)]
    live = [q for q in seqs if q != 0xFFFFFFFF and q != 0]
    return HDR.pack(b"SMEV", 1, REC.size, len(raw) // REC.size, 0,
                    (max(live) + 1) if live else 1) + raw


def decode(blob):
    if len(blob) < HDR.size:
        sys.exit("ответ короче заголовка — консоль закрыта?")
    magic, ver, rec, cap, head, next_seq = HDR.unpack_from(blob, 0)
    if magic != b"SMEV":
        sys.exit(f"не тот формат: {magic!r}")
    if rec != REC.size:
        sys.exit(f"запись {rec} Б, а скрипт знает {REC.size} — обнови одно под другое")
    body = blob[HDR.size:]
    out = []
    for off in range(0, len(body) - rec + 1, rec):
        (seq, t, awake, kind, wake, flags, gas, sens, boot_inc,
         heap_kb, draw_ms, radio_ms, vcell, soc_d, rssi, temp_c, detail) = REC.unpack_from(body, off)
        if seq == 0xFFFFFFFF:
            continue
        out.append(dict(seq=seq, t=t, awake=awake, kind=kind, wake=wake, flags=flags,
                        acc=gas & 3, cls=(gas >> 2) & 7, gasout=bool(gas & 0x20), sens=sens,
                        boot_inc=boot_inc, heap_kb=heap_kb, draw_ms=draw_ms, radio_ms=radio_ms,
                        vcell=vcell, soc=soc_d / 100.0, rssi=rssi, temp=temp_c, detail=detail))
    out.sort(key=lambda r: r["seq"])
    return dict(cap=cap, head=head, next_seq=next_seq, ver=ver), out


def ts(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%d.%m %H:%M:%S") if t else "  без часов "


def bits(v, table):
    return ",".join(n for m, n in table if v & m) or "-"


def report(meta, recs):
    if not recs:
        print("записей нет"); return
    wakes = [r for r in recs if r["kind"] == 0]
    span_t = [r["t"] for r in recs if r["t"]]
    print(f"кольцо: {meta['cap']} слотов, следующий seq {meta['next_seq']}")
    print(f"получено {len(recs)} записей, из них пробуждений {len(wakes)}")
    if span_t:
        hours = (max(span_t) - min(span_t)) / 3600
        print(f"окно: {ts(min(span_t))} .. {ts(max(span_t))}  ({hours:.1f} ч)")
        if hours > 0:
            print(f"пробуждений в сутки: {len(wakes) / hours * 24:.0f}")
    print(f"без отметки времени: {sum(1 for r in recs if not r['t'])} "
          f"(часы ещё не были выставлены)")

    print("\n— ЧЕМ РАЗБУЖЕН —")
    for k, n in Counter(WAKE.get(r["wake"], "?") for r in wakes).most_common():
        print(f"   {k:8} {n:6}  {n*100/max(1,len(wakes)):5.1f} %")

    # EF_MEASURE (флаг 4) прошивка ставит ТОЛЬКО в режиме Matter — рабочий цикл Wi-Fi его не
    # выставляет никогда. Поэтому по одному флагу замеры терялись внутри «часового тика» (за
    # 52,7 ч было видно 12 замеров вместо шестисот, а p90 у тика равнялся 6129 мс — это они и
    # были). Опознаём по тому, кто ответил на шине: газовый путь — это BME или BSEC, и на
    # незамерочных пробуждениях не отвечает никто, кроме NVS. Флаг по-прежнему уважаем, если он
    # есть, чтобы записи режима Matter читались как раньше.
    def measuring(r):
        return bool(r["flags"] & 4) or bool(r["sens"] & 0x07)   # ES_SCD | ES_BME | ES_BSEC

    print("\n— ВРЕМЯ БОДРСТВОВАНИЯ (мс) —")
    for lab, sel in (("часовой тик", lambda r: not measuring(r)),
                     ("замер",        lambda r: measuring(r) and not (r["flags"] & 8)),
                     ("с радио",      lambda r: r["flags"] & 8)):
        v = sorted(r["awake"] for r in wakes if sel(r))
        if not v: continue
        med = v[len(v)//2]
        print(f"   {lab:12} n={len(v):5}  медиана {med:6}  p90 {v[int(len(v)*0.9)]:6}  макс {v[-1]:6}")

    print("\n— ЧТО ОТВЕЧАЛО НА ШИНЕ (по замерочным тикам) —")
    meas = [r for r in wakes if measuring(r)]
    for m, n in SENS:
        if not meas: break
        ok = sum(1 for r in meas if r["sens"] & m)
        if ok or n in ("scd", "bme", "gauge"):
            print(f"   {n:6} {ok:5}/{len(meas)}  {ok*100/len(meas):5.1f} %")

    print("\n— ЭКРАН —")
    drew = [r for r in wakes if r["flags"] & 64 and r["draw_ms"]]
    if drew:
        full = [r["draw_ms"] for r in drew if r["flags"] & 128]
        part = [r["draw_ms"] for r in drew if not (r["flags"] & 128)]
        for lab, v in (("полных", full), ("частичных", part)):
            if v:
                v = sorted(v)
                print(f"   {lab:10} n={len(v):5}  медиана {v[len(v)//2]:5} мс  макс {v[-1]:5} мс")
        fast = [r for r in drew if r["draw_ms"] < 150]
        if fast:
            print(f"   ПОДОЗРИТЕЛЬНО БЫСТРЫХ (<150 мс, кадр мог не уйти): {len(fast)}")

    print("\n— КАЛИБРОВКА ГАЗА —")
    accs = [r["acc"] for r in wakes if r["flags"] & 4]
    if accs:
        for a, n in sorted(Counter(accs).items()):
            print(f"   acc {a}/3: {n:6}  {n*100/len(accs):5.1f} %")

    print("\n— ЛОЖНЫЕ ФРОНТЫ КНОПКИ —")
    # Cumulative in the record, so the interesting quantity is the per-wake increment: it says
    # which wakes collect edges, which a running total never can.
    nz = [(r["wake"], r["flags"], r["detail"]) for r in wakes if r["detail"]]
    if len(nz) > 2:
        inc = [b[2] - a[2] for a, b in zip(nz, nz[1:]) if b[2] >= a[2]]
        if inc:
            print(f"   счётчик: {nz[0][2]} -> {nz[-1][2]}  (+{nz[-1][2]-nz[0][2]} за окно)")
            print(f"   прирост на пробуждение: медиана {sorted(inc)[len(inc)//2]}, "
                  f"макс {max(inc)}, нулевых {sum(1 for i in inc if i == 0)}/{len(inc)}")
            by = defaultdict(list)
            for (w, f, _), i in zip(nz[1:], inc): by[WAKE.get(w, "?")].append(i)
            for k, v in sorted(by.items()):
                print(f"     {k:8} среднее {sum(v)/len(v):5.2f} на пробуждение (n={len(v)})")

    # Свой раздел, а не строчки в общем хвосте: хвост печатает последние тридцать событий, и
    # несколько заметок там тонут среди радио и тиков — ровно тогда, когда их и смотрят.
    ls = [r for r in recs if r["kind"] == 10 and (r["detail"] & 0xFF) == 5]
    if ls:
        ms = sorted((r["detail"] >> 8) & 0x3FFF for r in ls)
        ns = sorted((r["detail"] >> 22) & 0x3FF for r in ls)
        zero = sum(1 for n in ns if n == 0)
        print("\n— ЛЁГКИЙ СОН НА ОЖИДАНИЯХ ДАТЧИКОВ —")
        print(f"   замеров с заметкой: {len(ls)}")
        print(f"   проспано: медиана {ms[len(ms)//2]} мс, мин {ms[0]}, макс {ms[-1]}")
        print(f"   засыпаний: медиана {ns[len(ns)//2]}, макс {ns[-1]}")
        if zero:
            print(f"   НЕ СПАЛ ни разу: {zero} из {len(ls)}"
                  + ("  — на кабеле у компьютера это норма (USJ_NO_AUTO_LS_ON_CONNECTION)"
                     if zero == len(ls) else ""))

    # Подключение отдельно от полезной работы. До 2026-09-20 писался только итог окна, и по нему
    # нельзя было решить, куда целиться: в связь, в DHCP или в размер того, что качаем.
    rc = [r for r in recs if r["kind"] == 4 and (r["detail"] & 0x80000000)]
    if rc:
        A = sorted(r["detail"] & 0xFFF for r in rc)
        D = sorted((r["detail"] >> 12) & 0xFFF for r in rc)
        T = sorted(r["radio_ms"] for r in rc)
        fast_used = sum(1 for r in rc if r["detail"] & (1 << 24))
        fast_ok   = sum(1 for r in rc if r["detail"] & (1 << 25))
        med = lambda v: v[len(v) // 2]
        print("\n— ПОДКЛЮЧЕНИЕ К WI-FI —")
        print(f"   окон с разбивкой: {len(rc)}")
        print(f"   связь   : медиана {med(A):5} мс   p90 {A[int(len(A)*0.9)]:5}   макс {A[-1]:5}")
        print(f"   DHCP    : медиана {med(D):5} мс   p90 {D[int(len(D)*0.9)]:5}   макс {D[-1]:5}")
        # Доля считается по каждому окну отдельно, потом берётся медиана: складывать медианы
        # трёх разных распределений и делить одну на другую — значит получить число, которого
        # не было ни в одном окне.
        share = sorted(((r["detail"] & 0xFFF) + ((r["detail"] >> 12) & 0xFFF))
                       * 100 // max(1, r["radio_ms"]) for r in rc)
        print(f"   всё окно: медиана {med(T):5} мс — на подключение уходит {med(share)} % окна "
              f"(медиана по окнам)")
        print(f"   быстрый путь: пробован {fast_used}, удался {fast_ok}"
              + (f"  ← ПРОМАХИ: {fast_used-fast_ok}" if fast_used > fast_ok else ""))

    # awake_ms считается от EvtLog::init(), а он — восьмой шаг пробуждения. Всё, что до него
    # (загрузчик, старт ESP-IDF, сторож, менеджер питания, кнопка, разбор причины), в awake_ms не
    # входит, поэтому любое «прибор бодрствует N минут в сутки» по этому журналу — нижняя граница.
    pre = [r["detail"] >> 8 for r in recs if r["kind"] == 10 and (r["detail"] & 0xFF) == 6]
    if pre:
        p_ = sorted(pre)
        m = p_[len(p_) // 2]
        print("\n— ПРОЛОГ ПРОБУЖДЕНИЯ (не входит в awake_ms) —")
        print(f"   замеров: {len(p_)}")
        print(f"   до регистратора: медиана {m} мс, p10 {p_[len(p_)//10]}, p90 {p_[9*len(p_)//10]}, макс {p_[-1]}")
        aw = [r["awake"] for r in recs if r["kind"] == 0 and r["awake"] < 60000]
        if aw:
            a = sorted(aw)[len(aw)//2]
            print(f"   то есть настоящее пробуждение ~{a + m} мс против {a} мс по awake_ms "
                  f"(+{m*100//max(1,a)} %)")

    print("\n— ПАМЯТЬ —")
    hp = [r["heap_kb"] for r in wakes if r["heap_kb"]]
    if hp:
        print(f"   свободно: мин {min(hp)} КБ, медиана {sorted(hp)[len(hp)//2]} КБ, макс {max(hp)} КБ")
        # A ring where every wake starts from a fresh heap cannot leak; a downward trend here would
        # mean something survives sleep that should not.
        half = len(hp) // 2
        if half > 20:
            d = sum(hp[half:]) / (len(hp)-half) - sum(hp[:half]) / half
            print(f"   тренд (вторая половина минус первая): {d:+.1f} КБ"
                  + ("  <- присмотреться" if d < -8 else ""))

    print("\n— БАТАРЕЯ —")
    bat = [(r["t"], r["soc"], r["vcell"]) for r in wakes if r["vcell"] > 2000]
    if len(bat) > 2:
        print(f"   {bat[0][1]:.2f} % / {bat[0][2]} мВ  ->  {bat[-1][1]:.2f} % / {bat[-1][2]} мВ")

    ev = [r for r in recs if r["kind"] != 0]
    print(f"\n— СОБЫТИЯ ({len(ev)}) —")
    for k, n in Counter(KIND.get(r["kind"], "?") for r in ev).most_common():
        print(f"   {k:8} {n}")
    for r in ev[-25:]:
        k = KIND.get(r["kind"], "?")
        extra = ""
        if k == "boot":
            extra = RESET.get(r["detail"] & 0xFF, str(r["detail"] & 0xFF))
            if r["detail"] & 0x100: extra += " (не дошёл до сна)"
        elif k == "alert":
            extra = f"class {r['detail'] & 0xFF} tier {(r['detail'] >> 8) & 0xFF} co2 {r['detail'] >> 16}"
        elif k == "button":
            d = r["detail"]
            if d & 0x20000:
                if d & 0xFFFF:     # 0.9.40–0.9.41 also recorded how long the device had slept
                    why = ("окна настройки подряд без людей — нужно держать кнопку" if d & 0x40000
                           else "сразу после засыпания, кнопка не нажата")
                    extra = f"ФАНТОМ отброшен: спал {d & 0xFFFF} мс, {why}"
                else:
                    extra = "нажатие без удержания после неиспользованных окон настройки — отброшено"
            else:
                extra = f"{d & 0xFFFF} мс, {'ДОЛГОЕ' if d & 0x10000 else 'короткое'}"
        elif k == "power":
            extra = (f"ШТОРМ на пине USB: {r['detail'] & 0xFF} ложных пробуждений, пин не армируется"
                     if r["detail"] & 0x20000 else
                     f"{r['detail'] & 0xFFFF} мВ — {'БЛОКИРОВКА' if r['detail'] & 0x10000 else 'разблокирован'}")
        elif k == "sensor":
            d = r["detail"]
            if d & 0x10000:
                extra = f"SCD41 не дал замера — reinit {'принят' if d & 1 else 'НЕ принят'}"
            elif d & 0x20000:
                corr = d & 0xFFFF
                corr = corr - 0x10000 if corr & 0x8000 else corr
                extra = f"SCD41 откалиброван по свежему воздуху, поправка {corr:+d} ppm"
            elif d & 0x40000:
                extra = "SCD41 отклонил калибровку (0xFFFF)"
            else:   # до 0.9.44: автоматический заводской сброс
                extra = f"SCD41 сброшен к заводским, попытка {d & 0xFF}, self-test 0x{(d >> 8) & 0xFFFF:04X}"
        elif k == "ota":
            extra = "УСТАНОВЛЕНО, перезагрузка" if r["detail"] == 0xFFFFFFFF else (
                    f"проверено, доступна сборка {r['detail']}" if r["detail"] else "проверено, нового нет")
        elif k == "note":
            # Код — младший байт: старшие байты некоторые заметки занимают под данные, и раньше
            # такая запись не находилась в таблице и печаталась числом.
            code = r["detail"] & 0xFF
            if code == 6:      # NOTE_WAKE_PRE
                extra = "до регистратора: %d мс" % (r["detail"] >> 8)
            elif code == 5:    # NOTE_LIGHT_SLEEP
                ms = (r["detail"] >> 8) & 0x3FFF
                n  = (r["detail"] >> 22) & 0x3FF
                extra = "лёгкий сон: %d мс за %d раз%s" % (
                    ms, n, "  ← НЕ СПАЛ" if n == 0 else "")
            else:
                extra = NOTE.get(code, str(r["detail"]))
        elif k == "sec":
            extra = sec_text(r["detail"])
        elif k == "gas":
            d = r["detail"]
            code = d & 0xFFFF
            code = code - 0x10000 if code & 0x8000 else code
            if d & 0x10000:   extra = f"предупреждение bsec_sensor_control {code}"
            elif d & 0x20000: extra = f"предупреждение bsec_do_steps {code}"
            elif d & 0x40000: extra = f"предупреждение bsec_update_subscription {code}"
            else:             extra = "BSEC вернул OK и НОЛЬ выходов"
        elif k == "draw":
            extra = f"кадр {r['detail']} мс — слишком быстро, мог не уйти"
        elif k == "radio":
            d = r["detail"]
            if d & 0x80000000:   # с 2026-09-20: разбивка подключения, итог — в поле radio_ms
                a, dh = d & 0xFFF, (d >> 12) & 0xFFF
                path = ("быстрый путь: удался" if d & (1 << 25)
                        else "быстрый путь: ПРОМАХ, полный скан" if d & (1 << 24)
                        else "кеша не было, полный скан")
                extra = f"{r['radio_ms']} мс в эфире | связь {a} мс, DHCP {dh} мс | {path}"
            else:
                extra = f"{d} мс в эфире"
        print(f"   seq {r['seq']:7} {ts(r['t'])} {k:7} {extra}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", help="IP прибора (консоль поднимается долгим нажатием)")
    ap.add_argument("--file", help="разобрать сохранённый дамп вместо запроса")
    ap.add_argument("--raw", help="сохранить полученные байты в этот файл")
    ap.add_argument("--partition", action="store_true",
                    help="--file это сырой дамп раздела events (esptool), а не ответ API")
    ap.add_argument("--since", type=int, default=0, help="только записи новее этого seq")
    ap.add_argument("--password", help="пароль консоли; иначе SMARTMETEO_PASSWORD или запрос")
    a = ap.parse_args()
    if a.file:
        blob = open(a.file, "rb").read()
        if a.partition: blob = wrap_partition(blob)
    elif a.host:
        t0 = time.time()
        pw = a.password or os.environ.get("SMARTMETEO_PASSWORD") or getpass.getpass("пароль прибора (он на экране): ")
        blob = fetch(a.host, a.since, pw)
        print(f"получено {len(blob)} Б за {time.time()-t0:.1f} с")
        if a.raw:
            open(a.raw, "wb").write(blob); print(f"сохранено в {a.raw}")
    else:
        ap.error("нужен host или --file")
    meta, recs = decode(blob)
    report(meta, recs)


if __name__ == "__main__":
    main()
