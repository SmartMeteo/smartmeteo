#!/usr/bin/env python3
"""Discharge bench, the computer's side: tools/curve_fit.py reads the logs that t_cycle wrote with
the FIRMWARE's own code (main/curve_log.cpp) and must recover the curve they were built from.

The format is described once (main/curve_log.h) and checked from both ends here: a field moved on
one side and not the other shows up as a wrong curve, not as a silent success. Run t_cycle first —
run.sh does.
"""
import importlib.util, json, math, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'tools', 'curve_fit.py')
OBJ = os.path.join(HERE, '.cycle-obj')
MAIN = os.path.join(HERE, '..', 'main')

spec = importlib.util.spec_from_file_location('curve_fit', TOOL)
cf = importlib.util.module_from_spec(spec); spec.loader.exec_module(cf)

fails = 0; count = 0
def ok(cond, what):
    global fails, count
    count += 1
    print(('  ок      ' if cond else '  ПРОВАЛ  ') + what)
    if not cond: fails += 1

def truth(s):  # the same curve t_cycle wrote
    return 3.30 + 0.35 * (1 - math.exp(-s / 6.0)) + 0.53 * s / 100.0 + 0.02 * math.sin(math.pi * s / 100.0)

run_bin, void_bin = os.path.join(OBJ, 'curve_run.bin'), os.path.join(OBJ, 'curve_void.bin')
if not (os.path.exists(run_bin) and os.path.exists(void_bin)):
    print('  ПРОВАЛ  нет образов из t_cycle — запустите tests/run.sh целиком'); sys.exit(1)

recs = cf.parse_bin(open(run_bin, 'rb').read())
run = cf.last_run(recs)
ok(run is not None and run[0]['kind'] == 1, 'образ прошивки читается, прогон начинается с заголовка')
samples = [r for r in run if r['kind'] == 2]
ok(len(samples) > 90, 'все замеры прочитаны (%d); разорванная запись посреди пропущена' % len(samples))
ok(all(b['seq'] == a['seq'] + 1 for a, b in zip(run, run[1:])),
   'номера записей идут подряд — ни одна верная не потеряна вокруг разорванной')

res = cf.fit(run)
ok(res['ok'] and res['ended'], 'прогон полный, таблица построена')
worst = max(abs(v - truth(s)) for v, s in res['table'])
ok(worst < 0.003, 'кривая восстановлена: худшее расхождение %.1f мВ (допуск 3)' % (worst * 1000))
ok(abs(res['q0'] - 100000.0) / 100000.0 < 0.001,
   'точка 0 %% найдена с ошибкой %.3f %% заряда' % (100 * abs(res['q0'] - 100000.0) / 100000.0))
ok(abs(res['table'][-1][1] - 1.0) < 1e-9 and res['table'][0][1] == 100.0, 'сетка от 100 до 1 %')
ok(all(b[0] < a[0] for a, b in zip(res['table'], res['table'][1:])),
   'напряжения строго убывают — прошивка интерполирует между соседями')
ok(abs(res['v_full'] - truth(100.0 * (1 - 0.0125 * 300 / 100000.0))) < 0.001,
   'напряжение полной ячейки — первый отдых, %.3f В' % res['v_full'])
ok(res['load_sag_mv'] is not None and abs(res['load_sag_mv'] - 15.0) < 0.5,
   'просадка под нагрузкой посчитана: %.1f мВ (записано 15)' % res['load_sag_mv'])

# The rest correction is not decorative: without it the charge axis is short by the rests' share.
res0 = cf.fit(run, rest_ratio=0.0)
e1, e0 = abs(res['q0'] - 100000.0), abs(res0['q0'] - 100000.0)
ok(e0 > 3 * e1, 'поправка на отдых нужна: без неё ось заряда короче на %.2f %%, с ней %.3f %%'
   % (100 * e0 / 100000.0, 100 * e1 / 100000.0))

vres = cf.fit(cf.last_run(cf.parse_bin(open(void_bin, 'rb').read())))
ok(not vres['ok'] and 'void' in vres.get('error', ''), 'прогон, в который воткнули кабель, отвергнут')

# A calibration made after the run: the raw readings are scaled, and the whole curve moves with them.
g = cf.fit(run, gain=1.01)
ok(g['ok'] and abs(g['table'][0][0] - 1.01 * res['table'][0][0]) < 0.002,
   'поправка делителя, заданная после прогона, применяется ко всем замерам')
g2 = cf.fit(run, gain=1.02)          # 3,25 В x 1,02 = 3,315: с такой поправкой прогон до порога не дошёл
ok(not g2['ok'] and 'ended above' in g2.get('error', ''),
   'а если с поправкой прогон кончился выше порога — отказ, а не выдуманный хвост')

# Progress over UDP reads the same way as the flash.
lines = '\n'.join('2026-10-01 12:00:00 192.168.1.114 CURVE,%d,%d,%d,%d,%.4f,%.4f,%.5f,%d,%d'
                  % (r['seq'], r['kind'], r['t'], r['load_s'], r['v_rest'], r['v_load'], r['gain'],
                     r['spread'], r['flags']) for r in run[1:])
u = cf.fit(cf.parse_udp(lines + '\n' + lines))          # every line twice: resends are expected
ok(u['ok'] and max(abs(a[0] - b[0]) for a, b in zip(u['table'], res['table'])) < 0.0015,
   'журнал по сети даёт ту же кривую, повторы не мешают')
partial = cf.fit(cf.parse_udp('\n'.join(lines.splitlines()[:40])))
ok(not partial['ok'] and 'not reached' in partial.get('error', ''), 'незаконченный прогон честно назван незаконченным')

# --write: into a copy of the firmware source, between the markers, and readable back.
tmp = tempfile.mkdtemp()
try:
    src = os.path.join(tmp, 'battery_source.cpp')
    shutil.copy(os.path.join(HERE, '..', 'main', 'battery_source.cpp'), src)
    before = open(src).read()
    out = subprocess.run([sys.executable, TOOL, '--bin', run_bin, '--write', '--source', src],
                         capture_output=True, text=True)
    ok(out.returncode == 0, 'скрипт с --write отработал')
    after = open(src).read()
    back = cf.current_table(after)
    ok(len(back) == len(res['table']) + 1 and abs(back[-1][0] - cf.cutoff_v()) < 1e-9 and back[-1][1] == 0.0,
       'таблица вписана целиком и кончается порогом при 0 %')
    ok(all(abs(a[0] - b[0]) < 0.0006 and a[1] == b[1] for a, b in zip(back, res['table'])),
       'и читается обратно той же')
    head = before[:before.index('// CURVE-TABLE BEGIN')]
    tail = before[before.index('// CURVE-TABLE END'):]
    ok(after.startswith(head.rstrip(' ')) and after.endswith(tail[len('// CURVE-TABLE END'):]),
       'вне меток файл не тронут')
    j = json.loads(subprocess.run([sys.executable, TOOL, '--bin', run_bin, '--json'],
                                  capture_output=True, text=True).stdout)
    ok(j['ok'] and 'vs_current_max' in j, 'машинный вывод: и сравнение с таблицей, что стоит сейчас')

    # Две ячейки, две таблицы (2026-10-08): --cell 103450 пишет в СВОИ метки и только в них.
    # Перепутай метки — и прогон одной ячейки молча встанет на место таблицы другой.
    src2 = os.path.join(tmp, 'battery_source_103450.cpp')
    shutil.copy(os.path.join(HERE, '..', 'main', 'battery_source.cpp'), src2)
    before2 = open(src2).read()
    out = subprocess.run([sys.executable, TOOL, '--bin', run_bin, '--write', '--source', src2,
                          '--cell', '103450'], capture_output=True, text=True)
    ok(out.returncode == 0, 'скрипт с --cell 103450 --write отработал')
    after2 = open(src2).read()
    back2 = cf.current_table(after2, 'CURVE-TABLE-103450')
    ok(len(back2) == len(res['table']) + 1
       and all(abs(a[0] - b[0]) < 0.0006 and a[1] == b[1] for a, b in zip(back2, res['table'])),
       'таблица 103450 вписана между своими метками и читается обратно той же')
    ok(cf.current_table(after2) == cf.current_table(before2),
       'а таблица 804050 не тронута')
    h2 = before2[:before2.index('// CURVE-TABLE-103450 BEGIN')]
    t2 = before2[before2.index('// CURVE-TABLE-103450 END'):]
    ok(after2.startswith(h2) and after2.endswith(t2[len('// CURVE-TABLE-103450 END'):]),
       'вне меток 103450 файл не тронут, отступ сохранён')
finally:
    shutil.rmtree(tmp)


# ---- v2: a whole test, C0 → D1 → C1 → D2 → C2, written by t_cycle through the firmware's code -----
ta_bin, tb_bin = os.path.join(OBJ, 'battery_test_a.bin'), os.path.join(OBJ, 'battery_test_b.bin')
if not (os.path.exists(ta_bin) and os.path.exists(tb_bin)):
    ok(False, 'нет образов теста АКБ из t_cycle')
else:
    tru = json.load(open(os.path.join(OBJ, 'battery_test_a.json')))
    fw = tru['fw']                                   # what the firmware's own summary computed
    A = cf.analyse(cf.parse_bin(open(ta_bin, 'rb').read()))
    ok(A['test'] and A['complete'], 'тест A прочитан и полный')
    kinds = [(x['type'], x['cycle'], x.get('recharge', False)) for x in A['segments']]
    ok(kinds == [('charge', 0, False), ('charge', 0, False), ('discharge', 1, False), ('charge', 0, True),
                 ('discharge', 1, False), ('charge', 1, False), ('discharge', 2, False), ('charge', 2, False)],
       'отрезки по порядку: «пустое готово», C0, испорченный D1, дозаряд, D1, C1, D2, C2')
    ok(A['segments'][0]['idle'] and A['segments'][2]['voided'], 'простой зарядника и испорченный разряд названы')
    for k in (1, 2):
        d = A['D'][k]
        if not d.get('q0_hours'):
            ok(False, 'D%d: разряд в таблице не дошёл до порога (%s)' % (k, d.get('error'))); continue
        e = abs(d['q0_hours'] - tru['d_q0_h'][k]) / tru['d_q0_h'][k]
        ok(d['ok'] and e < 0.001, 'D%d: часы до порога %.3f (правда %.3f), ошибка %.3f %%'
           % (k, d['q0_hours'], tru['d_q0_h'][k], 100 * e))
        ok(abs(d['q0_hours'] - fw['d_q0_h'][k]) < 2e-4 and abs(d['mah'] - fw['d_mah'][k]) < 0.05
           and abs(d['wh'] - fw['d_wh'][k]) < 0.002,
           'D%d: скрипт и прошивка сходятся — часы, мА·ч (%.1f / %.1f), Вт·ч' % (k, d['mah'], fw['d_mah'][k]))
        ok(abs(d['pulse_last_mv'] - fw['d_pulse'][k]) < 0.01 and abs(d['pulse_mv'] - 41.0) < 0.1,
           'D%d: импульс 10 с — %.1f мВ, как у прошивки' % (k, d['pulse_mv']))
    ok(A['D'][1]['samples'] > 90, 'D1 в таблице — повторный, полный (испорченный отброшен)')
    for k in (0, 1, 2):
        c = A['C'][k]
        ok(c['done'] and c['t_done'] == tru['c_done'][k] == fw['c_done'][k],
           'C%d: время заряда %s, как у прошивки и правды' % (k, cf.hm(c['t_done'])))
        ok(c['t_cv'] == tru['c_cv'][k] == fw['c_cv'][k], 'C%d: конец постоянного тока %s' % (k, cf.hm(c['t_cv'])))
        ok(abs(c['relaxed_v'] - fw['c_relaxed'][k]) < 1e-3, 'C%d: напряжение после заряда %.3f, как у прошивки'
           % (k, c['relaxed_v']))
    for k in (1, 2):
        c = A['C'][k]
        ok(abs(c['i_est_ma'] - fw['c_iest'][k]) < 0.5, 'C%d: ток в начале заряда ~%.0f мА, как у прошивки'
           % (k, c['i_est_ma']))
    ok('i_est_ma' not in A['C'][0] and 'i_est_ma' not in [x for x in A['segments'] if x.get('recharge')][0],
       'у C0 и у дозаряда тока нет — перед ними не было законченного разряда')

    # The charge current read off R7: the tool integrates it like the firmware, and recovers the load.
    for k in (1, 2):
        c = A['C'][k]
        ok(c['prog_covers'] and abs(c['prog_mah'] - fw['c_prog'][k]) < 0.5,
           'C%d: заряд по замерам R7 %.1f мА·ч, как у прошивки (%.1f)' % (k, c.get('prog_mah', 0), fw['c_prog'][k]))
        ok(abs(c['prog_mah'] - tru['c_prog_true'][k]) / tru['c_prog_true'][k] < 0.02,
           'C%d: и против правды %.1f — ошибка %.2f %%' % (k, tru['c_prog_true'][k],
           100 * abs(c['prog_mah'] - tru['c_prog_true'][k]) / tru['c_prog_true'][k]))
    # The coverage rule is held twice — firmware and tool — so the two numbers are compared, and the
    # tool is fed board f708's real C0 (2026-10-03): readings at 0:02, 0:10, 1:24, 1:30, an hour
    # without one in the middle. Not counted, and the report says why in words a person can act on.
    hdr = open(os.path.join(MAIN, 'curve_summary.h'), encoding='utf-8').read()
    for name in ('PROG_FIRST_BY_S', 'PROG_MAX_GAP_S'):
        m = re.search(r'%s\s*=\s*(\d+)' % name, hdr)
        ok(m is not None and int(m.group(1)) == getattr(cf, name),
           '%s один у прошивки и скрипта (%s против %s)' % (name, m.group(1) if m else '?', getattr(cf, name)))
    def rec(kind, t, v_rest=0.0, v_load=0.0):
        return dict(kind=kind, t=t, load_s=0, v_rest=v_rest, v_load=v_load, gain=1.0, spread=0, flags=0, detail=0)
    seg = dict(cycle=0, recharge=False, recs=[rec(7, 0, 4.00, 4.10)] +
               [rec(13, t, 4.14, a) for t, a in ((159, 0.59), (647, 0.57), (5040, 0.15), (5400, 0.11))] +
               [rec(9, 5520, 4.15, 4.20)])
    c0 = cf.charge_stats(seg)
    ok(c0['done'] and c0['prog_gap'] == 5040 - 647 and not c0['prog_covers'],
       'скрипт: ряд C0 платы f708 с дырой %s не считается (%s)' % (cf.hm(c0.get('prog_gap')), c0.get('prog_covers')))
    ok('1:13 gap' in cf.prog_why(c0), 'и называет причину: «%s»' % cf.prog_why(c0))
    lm = A['load_ma_measured']
    ok(lm and abs(lm - fw['load_meas']) < 0.05 and abs(lm - tru['load_true']) / tru['load_true'] < 0.02,
       'ток разряда, измеренный через заряд: %.2f мА (прошивка %.2f, правда %.2f)' % (lm or 0, fw['load_meas'], tru['load_true']))
    out = subprocess.run([sys.executable, TOOL, '--bin', ta_bin], capture_output=True, text=True)
    ok('MEASURED from the charges' in out.stdout and 'R7 readings' in out.stdout, 'отчёт называет измеренный ток и замеры R7')

    # The table comes from the last complete discharge unless one is asked for.
    j = json.loads(subprocess.run([sys.executable, TOOL, '--bin', ta_bin, '--json'], capture_output=True, text=True).stdout)
    ok(j['ok'] and j['cycle'] == 2, 'таблица по умолчанию — из последнего полного разряда (D2)')
    worst = max(abs(v - truth(s)) for v, s in j['table'])
    ok(worst < 0.003, 'и восстанавливает кривую: худшее расхождение %.1f мВ' % (worst * 1000))
    j1 = json.loads(subprocess.run([sys.executable, TOOL, '--bin', ta_bin, '--json', '--cycle', '1'],
                                   capture_output=True, text=True).stdout)
    ok(j1['ok'] and j1['cycle'] == 1, '--cycle 1 берёт D1')
    out = subprocess.run([sys.executable, TOOL, '--bin', ta_bin], capture_output=True, text=True)
    ok(out.returncode == 0 and 'D2 / D1' in out.stdout and 'C1' in out.stdout, 'отчёт печатается целиком')

    # Two cells: B holds 90 % of A by construction. The ratio needs no current: same board, same load.
    cmp = json.loads(subprocess.run([sys.executable, TOOL, '--compare', ta_bin, tb_bin, '--json'],
                                    capture_output=True, text=True).stdout)
    ok(abs(cmp['capacity_ratio'] - 0.9) < 0.002, 'сравнение: B держит %.2f %% от A (правда 90)' % (100 * cmp['capacity_ratio']))
    cmp2 = json.loads(subprocess.run([sys.executable, TOOL, '--compare', ta_bin, tb_bin, '--json', '--current', '120'],
                                     capture_output=True, text=True).stdout)
    ok(abs(cmp2['capacity_ratio'] - cmp['capacity_ratio']) < 1e-9, 'и отношение не зависит от принятого тока')
    txt = subprocess.run([sys.executable, TOOL, '--compare', ta_bin, tb_bin], capture_output=True, text=True)
    ok(txt.returncode == 0 and 'holds 90.0 %' in txt.stdout, 'сравнение печатается')

    # Progress over UDP, v2 lines (eleven fields: detail at the end) read the same way.
    recs = cf.parse_bin(open(ta_bin, 'rb').read())
    lines = '\n'.join('x CURVE,%d,%d,%d,%d,%.4f,%.4f,%.5f,%d,%d,%d' % (r['seq'], r['kind'], r['t'], r['load_s'],
                      r['v_rest'], r['v_load'], r['gain'], r['spread'], r['flags'], r['detail']) for r in recs)
    U = cf.analyse(cf.parse_udp(lines))
    ok(U['complete'] and abs(U['D'][2]['q0_hours'] - A['D'][2]['q0_hours']) < 1e-3,
       'журнал по сети в формате v2 даёт тот же тест')

print('стенд разряда    проверок %d, провалов %d' % (count, fails))
sys.exit(1 if fails else 0)
