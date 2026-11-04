#!/usr/bin/env python3
"""Сверка восхода и заката со ВТОРОЙ, независимой реализацией.

t_sunmoon.cpp проверяет инварианты — равноденствие, симметрию, полярные день и ночь. Они ловят
грубые ошибки, но не ловят систематический сдвиг: формула, уехавшая на пять минут, все инварианты
проходит. Поэтому здесь та же задача решается другим способом — рядами Спенсера для склонения и
уравнения времени (формулировка солнечного калькулятора NOAA), а не через юлианские даты и часовой
угол, как в прошивке. Две независимые формулировки, сошедшиеся в пределах пары минут, — это уже
довод; одна, сверенная с таблицей, которую я же и выбрал, — нет.

Допуск 3 минуты: обе формулировки — приближения того же порядка, и ряд Спенсера сам по себе даёт
около минуты. Больше трёх значит, что разошлись не приближения, а логика.
"""
import math, os, subprocess, sys, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
fails = []


def ok(cond, what):
    print(("  ок      " if cond else "  ПРОВАЛ  ") + what)
    if not cond:
        fails.append(what)


def noaa(lat, lon, date, tz_s):
    """Восход и закат в минутах от местной полуночи, рядами Спенсера. None, если события нет."""
    doy = date.timetuple().tm_yday
    g = 2 * math.pi / 365.0 * (doy - 1 + 0.5)          # середина суток
    eqtime = 229.18 * (0.000075 + 0.001868 * math.cos(g) - 0.032077 * math.sin(g)
                       - 0.014615 * math.cos(2 * g) - 0.040849 * math.sin(2 * g))
    decl = (0.006918 - 0.399912 * math.cos(g) + 0.070257 * math.sin(g)
            - 0.006758 * math.cos(2 * g) + 0.000907 * math.sin(2 * g)
            - 0.002697 * math.cos(3 * g) + 0.00148 * math.sin(3 * g))
    phi = math.radians(lat)
    c = (math.cos(math.radians(90.833)) / (math.cos(phi) * math.cos(decl))
         - math.tan(phi) * math.tan(decl))
    if c > 1 or c < -1:
        return None
    ha = math.degrees(math.acos(c))
    rise_utc = 720 - 4 * (lon + ha) - eqtime
    set_utc = 720 - 4 * (lon - ha) - eqtime
    off = tz_s / 60.0
    return rise_utc + off, set_utc + off


def main():
    exe = os.path.join(HERE, "t_sunmoon")
    if not os.path.exists(exe):
        print("  ПРОВАЛ  сверка: двоичник t_sunmoon не собран")
        return 1
    out = subprocess.run([exe, "--dump"], capture_output=True, text=True).stdout.strip().split("\n")
    ok(len(out) > 100, "сверка: прошивка выдала %d случаев" % len(out))

    worst, worst_case, compared, skipped = 0.0, "", 0, 0
    for line in out:
        lat, lon, tz, d, valid, rise, sset = line.split(",")
        lat, lon, tz = float(lat), float(lon), int(tz)
        date = datetime.date.fromisoformat(d)
        ref = noaa(lat, lon, date, tz)
        if int(valid) == 0 or ref is None:
            # Обе стороны обязаны согласиться, что события нет: расхождение здесь значит, что одна
            # из них рисует восход в полярную ночь.
            ok(int(valid) == 0 and ref is None,
               "сверка: полярный случай совпал (%s, %.1f°)" % (d, lat))
            skipped += 1
            continue
        for got, want, name in ((int(rise), ref[0], "восход"), (int(sset), ref[1], "закат")):
            diff = abs(got - want)
            compared += 1
            if diff > worst:
                worst, worst_case = diff, "%s %s %.2f°,%.2f° — прошивка %d, эталон %.1f" % (
                    name, d, lat, lon, got, want)
    ok(compared > 200, "сверка: сопоставлено %d времён (%d полярных пропущено)" % (compared, skipped))
    ok(worst <= 3.0, "сверка: наибольшее расхождение %.1f мин — %s" % (worst, worst_case or "—"))

    print("солнце и луна (сверка) провалов %d" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
