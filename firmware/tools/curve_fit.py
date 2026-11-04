#!/usr/bin/env python3
"""Read the battery bench's log (main/curve_bench.cpp): the report of a whole test, the comparison of
two cells, and the voltage-to-percent table of main/battery_source.cpp.

    tools/curve_fit.py --port --save cellA.bin   read the log off the device (cable in) with esptool
    tools/curve_fit.py --bin cellA.bin           a saved image of the coredump partition
    tools/curve_fit.py --udp curve_udp.log       what tools/curve_listen.py caught (progress; may be partial)
    tools/curve_fit.py --compare A.bin B.bin     two tests side by side: which cell holds more, by how much

    --cycle N        the discharge to build the table from (default: the last complete one)
    --current MA     the load current the mAh and Wh are computed with (default 82, an estimate:
                     the board has no shunt). Ratios between two cells do not depend on it.

    --gain G         the divider gain to apply, when the board was calibrated AFTER the run
    --rest-ratio R   rest current as a fraction of the load current (default 0.0125)
    --save FILE      keep the raw partition image read with --port
    --write          put the table into main/battery_source.cpp, between its CURVE-TABLE markers
    --cell C         which cell's table: 804050 (v2 boards, the default) or 103450 (the v1 board)
    --source FILE    ...into this file instead (tests)
    --erase          erase the partition after a successful read (with --port): the product
                     firmware otherwise complains once per boot about a crash dump it cannot parse
    --json           machine-readable result on stdout (tests)

HOW THE PERCENT IS DERIVED. The cell feeds the board through a linear regulator, which draws from
its input the current its output delivers whatever the input voltage; the bench holds that load
constant. Charge therefore leaves the cell in proportion to time under load, and the rests draw a
small, known fraction of it. With q = load_s + rest_ratio * (t - load_s), the fraction left is
1 - q / q_cutoff, where q_cutoff is interpolated at the rested reading crossing BATTERY_CUTOFF_V:
100 % is the charger's own "finished", 0 % is where the product locks out. That is the scale the
product's percentage has to mean.
"""
import argparse, glob, json, os, re, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, '..', 'main', 'battery_source.cpp')
SPEC = os.path.join(HERE, '..', 'main', 'battery_spec.h')

REC = 32
FMT = '<HBBIIfffHBBI'                      # main/curve_log.h, struct Rec — 32 bytes
MAGIC = 0xC7B1
KIND = {1: 'header', 2: 'sample', 3: 'end', 4: 'charger', 5: 'reset', 6: 'test', 7: 'charge-start',
        8: 'charge-sample', 9: 'charge-end', 10: 'pulse', 11: 'unplug', 12: 'test-done', 13: 'prog'}
PROG_A_PER_V = 1.0                         # TP4056: I = V_PROG / R_PROG x 1200, R7 = 1.2 kOhm -> 1 V = 1 A
CHARGE_TERM_A = 0.1                        # it stops at a tenth of the programmed current
# How much of a charge the R7 readings may leave uncovered — the same two numbers as
# main/curve_summary.h, where the reason is written down (a straight line across an hour of the
# constant-current-to-constant-voltage turn is not a measurement). t_curve.py checks they match.
PROG_FIRST_BY_S = 900
PROG_MAX_GAP_S = 1800
FLAG_RECHARGE = 16
LOAD_MA = 82.0                             # main/curve_summary.h: an estimate, see there
REST_RATIO = 0.0125
CV_BAND = 0.010
PART_ADDR, PART_SIZE = 0x7B0000, 0x10000   # partitions.csv: coredump
GRID = [100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 10, 7, 5, 3, 2, 1]


def cutoff_v():
    m = re.search(r'BATTERY_CUTOFF_V\s*=\s*([0-9.]+)f', open(SPEC).read())
    return float(m.group(1))


def parse_bin(blob):
    recs = []
    for i in range(0, len(blob) - REC + 1, REC):
        chunk = blob[i:i + REC]
        if chunk == b'\xff' * REC:
            continue
        f = struct.unpack(FMT, chunk)
        if f[0] != MAGIC or zlib.crc32(chunk[:28]) != f[11]:
            continue                        # torn or foreign: skipped, as the firmware does
        recs.append(dict(kind=f[1], flags=f[2], t=f[3], load_s=f[4], v_rest=f[5], v_load=f[6],
                         gain=f[7], seq=f[8], spread=f[9], detail=f[10]))
    return recs


def parse_udp(text):
    recs = []
    for line in text.splitlines():
        i = line.find('CURVE,')
        if i < 0:
            continue
        p = line[i:].strip().split(',')
        if len(p) not in (10, 11):
            continue
        seq, kind, t, load_s = int(p[1]), int(p[2]), int(p[3]), int(p[4])
        recs.append(dict(kind=kind, flags=int(p[9]), t=t, load_s=load_s, v_rest=float(p[5]),
                         v_load=float(p[6]), gain=float(p[7]), seq=seq, spread=int(p[8]),
                         detail=int(p[10]) if len(p) == 11 else 0))
    # The same record can arrive twice (it is resent when the radio comes up); keep one per seq.
    seen, out = set(), []
    for r in recs:
        if r['seq'] not in seen:
            seen.add(r['seq']); out.append(r)
    return sorted(out, key=lambda r: r['seq'])


def read_port(port):
    if not port:
        ports = sorted(glob.glob('/dev/cu.usbmodem*'))
        if not ports:
            sys.exit('no /dev/cu.usbmodem* — is the cable in and the device awake?')
        port = ports[0]
    fd, path = tempfile.mkstemp(suffix='.bin'); os.close(fd)
    cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32c6', '-p', port, '-b', '460800',
           '--before', 'default_reset', '--after', 'hard_reset', 'read_flash',
           hex(PART_ADDR), hex(PART_SIZE), path]
    subprocess.run(cmd, check=True)
    blob = open(path, 'rb').read(); os.unlink(path)
    return blob, port


def last_run(recs):
    start = max((i for i, r in enumerate(recs) if r['kind'] == 1), default=None)
    if start is None:
        return None
    return recs[start:]


def interp(x, xs, ys):
    """y at x on a polyline whose xs are monotonic (either direction)."""
    for i in range(1, len(xs)):
        a, b = xs[i - 1], xs[i]
        if (a - x) * (b - x) <= 0 and a != b:
            return ys[i - 1] + (x - a) / (b - a) * (ys[i] - ys[i - 1])
    return None


def fit(run, gain=None, rest_ratio=0.0125, cut=None):
    cut = cutoff_v() if cut is None else cut
    res = dict(ok=False, samples=0, resets=sum(1 for r in run if r['kind'] == 5),
               voided=any(r['kind'] == 4 for r in run), ended=any(r['kind'] == 3 for r in run))
    if res['voided']:
        res['error'] = 'the run is void: a cable went in mid-run'
        return res
    pts = []
    for r in run:
        if r['kind'] != 2 or r['v_rest'] <= 2.0:
            continue
        g = gain if gain else r['gain']
        q = r['load_s'] + rest_ratio * max(0, r['t'] - r['load_s'])
        pts.append(dict(q=q, v=r['v_rest'] * g, vl=(r['v_load'] * g if r['v_load'] > 2.0 else None),
                        t=r['t'], load_s=r['load_s'], spread=r['spread']))
    res['samples'] = len(pts)
    if len(pts) < 5:
        res['error'] = 'too few samples'
        return res
    # Rested readings only fall on a discharge; a rise is noise, and a curve must be monotonic for
    # the firmware's lookup. Running minimum along time.
    vmin = pts[0]['v']
    for p in pts:
        vmin = min(vmin, p['v']); p['vm'] = vmin
    res['hours'] = pts[-1]['t'] / 3600.0
    res['load_hours'] = pts[-1]['load_s'] / 3600.0
    res['v_full'] = pts[0]['vm']
    drops = [p['v'] - p['vl'] for p in pts if p['vl'] is not None]
    res['load_sag_mv'] = 1000 * sorted(drops)[len(drops) // 2] if drops else None
    # 0 %: where the rested reading crosses the cutoff.
    q0 = None
    hit = next((i for i, p in enumerate(pts) if p['vm'] <= cut), None)
    if hit is not None and hit > 0:
        a, b = pts[hit - 1], pts[hit]
        q0 = b['q'] if a['vm'] == b['vm'] else a['q'] + (a['vm'] - cut) / (a['vm'] - b['vm']) * (b['q'] - a['q'])
    if q0 is None:
        res['error'] = ('the run has not reached %.2f V yet (last %.3f V)' % (cut, pts[-1]['vm'])
                        if not res['ended'] else
                        'the run ended above %.2f V with this gain — was it calibrated differently?' % cut)
        res['last_v'] = pts[-1]['vm']
        return res
    for p in pts:
        p['soc'] = 100.0 * (1.0 - p['q'] / q0)
    use = [p for p in pts if p['soc'] >= 0.0]
    socs, vs = [p['soc'] for p in use] + [0.0], [p['vm'] for p in use] + [cut]
    table = []
    for s in GRID:
        v = interp(s, socs, vs) if s < socs[0] else vs[0]
        table.append((round(v, 3), float(s)))
    # Strictly descending voltages: the firmware interpolates between neighbours.
    for i in range(1, len(table)):
        if table[i][0] >= table[i - 1][0]:
            table[i] = (round(table[i - 1][0] - 0.001, 3), table[i][1])
    res.update(ok=True, q0=q0, table=table,
               points=[(round(p['soc'], 2), round(p['vm'], 4)) for p in use])
    return res


def temp_of(code):
    return code / 2.0 - 40.0 if code else None


def segments(recs):
    """A v2 test as its segments, in order. A v1 log (no test record) is one discharge: the last run."""
    if not any(r['kind'] == 6 for r in recs):
        run = last_run(recs)
        return dict(test=False, complete=False, segs=[dict(type='discharge', cycle=1, recs=run)] if run else [])
    start = max(i for i, r in enumerate(recs) if r['kind'] == 6)
    out, cur = [], None
    complete = False
    for r in recs[start + 1:]:
        k = r['kind']
        if k == 7:
            cur = dict(type='charge', cycle=r['spread'], recharge=bool(r['flags'] & FLAG_RECHARGE), recs=[r])
            out.append(cur)
        elif k == 1:
            cur = dict(type='discharge', cycle=r['spread'] if r['spread'] in (1, 2) else 1, recs=[r])
            out.append(cur)
        elif k == 12:
            complete = True
        elif cur is not None:
            cur['recs'].append(r)
            if k == 9 and cur['cycle'] == 2 and not cur['recharge']:
                complete = True
    return dict(test=True, complete=complete, segs=out, t0=recs[start]['t'])


def discharge_stats(seg, gain=None, current=LOAD_MA, rest_ratio=REST_RATIO, cut=None):
    run = seg['recs']
    res = fit(run, gain=gain, rest_ratio=rest_ratio, cut=cut)
    g = lambda r: gain if gain else r['gain']
    samples = [r for r in run if r['kind'] == 2 and r['v_rest'] > 2.0]
    pulses = [(r['v_rest'] - r['v_load']) * g(r) * 1000 for r in run
              if r['kind'] == 10 and r['v_rest'] > 2.0 and r['v_load'] > 2.0]
    temps = [temp_of(r['detail']) for r in run if r['kind'] in (2, 10) and r['detail']]
    vh, prev = 0.0, 0
    for r in samples:
        if r['v_load'] > 2.0:
            vh += r['v_load'] * g(r) * (r['load_s'] - prev) / 3600.0
        prev = r['load_s']
    if samples:
        last = samples[-1]
        res['q_end_h'] = (last['load_s'] + rest_ratio * max(0, last['t'] - last['load_s'])) / 3600.0
    res.update(cycle=seg['cycle'], pulses=len(pulses),
               pulse_mv=sorted(pulses)[len(pulses) // 2] if pulses else None,
               pulse_last_mv=pulses[-1] if pulses else None,
               temp_min=min(temps) if temps else None, temp_max=max(temps) if temps else None,
               wh=vh * current / 1000.0, current_ma=current)
    if res.get('q0'):
        res['q0_hours'] = res['q0'] / 3600.0
        res['mah'] = res['q0_hours'] * current
    return res


def charge_stats(seg, prev_pulse_mv=None, gain=None, current=LOAD_MA):
    recs = seg['recs']
    st = recs[0]
    g = lambda r: gain if gain else r['gain']
    res = dict(cycle=seg['cycle'], recharge=seg['recharge'], done=False, cut=False, idle=False,
               resets=sum(1 for r in recs if r['kind'] == 5))
    res['v_before'] = st['v_rest'] * g(st) if st['v_rest'] > 2.0 else None
    res['v_first'] = st['v_load'] * g(st) if st['v_load'] > 2.0 else None
    pts, top, end, v10, prog = [], 0.0, None, None, []
    if res['v_first']:
        pts.append((0.0, res['v_first']))
    after = []
    for r in recs[1:]:
        if r['kind'] == 8 and r['v_rest'] > 2.0:
            v = r['v_rest'] * g(r)
            if end is None:
                top = max(top, v)
                if r['t'] >= 10 and v10 is None:
                    v10 = v
                pts.append((r['t'] / 60.0, v))
            else:
                after.append((r['t'] - end['t'], v))
        elif r['kind'] == 9:
            end = r
            top = max(top, r['v_load'] * g(r))
        elif r['kind'] == 11:
            res['cut'] = True; res['idle'] = r['detail'] == 1
        elif r['kind'] == 13 and end is None and 0.0 <= r['v_load'] <= 1.5:
            if not prog or r['t'] >= prog[-1][0]:
                prog.append((r['t'], r['v_load'] * PROG_A_PER_V))
    temps = [temp_of(r['detail']) for r in recs if r['kind'] in (7, 8, 9) and r['detail']]
    res.update(v_top=top or None, v10=v10, samples=len(pts),
               temp_min=min(temps) if temps else None, temp_max=max(temps) if temps else None)
    if end is not None:
        res['done'] = True
        res['t_done'] = end['t']
        res['v_done'] = end['v_rest'] * g(end) if end['v_rest'] > 2.0 else None
        res['t_cv'] = next((int(round(m * 60)) for m, v in pts if v >= top - CV_BAND), None)
        if after:
            res['relaxed_v'], res['relaxed_s'] = after[-1][1], after[-1][0]
            res['relaxation'] = [(round(s / 60.0, 1), round(v, 4)) for s, v in after]
    else:
        last = [r for r in recs if r['kind'] == 8]
        res['t_so_far'] = last[-1]['t'] if last else 0
    # The charge put in, from the R7 readings: flat to the first, trapezoids between, and down to the
    # termination current at the end — the same integral as main/curve_summary.cpp.
    res['prog'] = prog
    res['prog_n'] = len(prog)
    if prog:
        a_s = prog[0][1] * prog[0][0]
        for (t0, a0), (t1, a1) in zip(prog, prog[1:]):
            a_s += 0.5 * (a0 + a1) * (t1 - t0)
        if res['done'] and res['t_done'] > prog[-1][0]:
            a_s += 0.5 * (prog[-1][1] + CHARGE_TERM_A) * (res['t_done'] - prog[-1][0])
        res['prog_mah'] = a_s / 3.6
    gaps = [t1 - t0 for (t0, _), (t1, _) in zip(prog, prog[1:])]
    if res['done'] and prog and res['t_done'] > prog[-1][0]:
        gaps.append(res['t_done'] - prog[-1][0])
    res['prog_gap'] = max(gaps) if gaps else 0
    res['prog_covers'] = bool(res['done'] and len(prog) >= 4 and prog[0][0] <= PROG_FIRST_BY_S
                              and res['prog_gap'] <= PROG_MAX_GAP_S)
    if prev_pulse_mv and res['v_before'] and v10 and v10 > res['v_before']:
        res['i_est_ma'] = current * (v10 - res['v_before']) * 1000.0 / prev_pulse_mv
    res['curve'] = [(round(m, 2), round(v, 4)) for m, v in pts]
    return res


def analyse(recs, gain=None, current=LOAD_MA, rest_ratio=REST_RATIO):
    """Every segment of the test worked out; the same numbers the bench's screen shows."""
    sg = segments(recs)
    out, last_pulse, prev_ended = [], None, False
    for s in sg['segs']:
        if s['type'] == 'discharge':
            d = discharge_stats(s, gain=gain, current=current, rest_ratio=rest_ratio)
            last_pulse = d['pulse_last_mv']
            prev_ended = d['ended'] and not d['voided']
            out.append(dict(type='discharge', **d))
        else:
            c = charge_stats(s, prev_pulse_mv=last_pulse if prev_ended else None, gain=gain, current=current)
            c['after_discharge'] = prev_ended
            prev_ended = False
            out.append(dict(type='charge', **c))
    # The measured ones: the latest attempt of each discharge, charges without the re-charges.
    best = dict(D={}, C={})
    for x in out:
        if x['type'] == 'discharge':
            best['D'][x['cycle']] = x
        elif not x['recharge']:
            best['C'][x['cycle']] = x
    # The load current, measured: a charge from the cutoff puts back what the discharge before it
    # took, so its mAh over that discharge's hours is the current the discharge ran at.
    mah = h = 0.0
    for k in (1, 2):
        c, d = best['C'].get(k), best['D'].get(k)
        if c and d and c.get('after_discharge') and c.get('prog_covers') and d.get('ended') \
                and not d.get('voided') and d.get('q_end_h'):
            mah += c['prog_mah']; h += d['q_end_h']
    load_meas = mah / h if h > 0 else None
    return dict(test=sg['test'], complete=sg['complete'], segments=out, D=best['D'], C=best['C'],
                load_ma_measured=load_meas)


def hm(s):
    return '%d:%02d' % (s // 3600, s // 60 % 60) if s is not None else '--'


def fmt(v, f):
    return f % v if v is not None else '--'


def prog_why(x):
    # Why a charge's R7 readings were not trusted, in the words a person can act on next time.
    if not x.get('done'):
        return 'the charge did not finish'
    if x['prog_n'] < 4:
        return 'only %d readings' % x['prog_n']
    if x['prog'][0][0] > PROG_FIRST_BY_S:
        return 'the first reading came at %s, the start is uncovered' % hm(x['prog'][0][0])
    return 'a %s gap without a reading (at most %s)' % (hm(x['prog_gap']), hm(PROG_MAX_GAP_S))


def print_report(a, name=''):
    print('=== %s%s ===' % (name + ': ' if name else '', 'test COMPLETE' if a['complete'] else
                           ('test in progress' if a['test'] else 'one discharge (v1 log)')))
    for x in a['segments']:
        if x['type'] == 'discharge':
            state = 'complete' if x['ended'] and not x['voided'] else 'VOID' if x['voided'] else 'running'
            print('D%d  %-8s  %5.2f h under load (%.2f h wall), %d samples, %d pulses, %d resets'
                  % (x['cycle'], state, x.get('load_hours', 0) or 0, x.get('hours', 0) or 0,
                     x['samples'], x['pulses'], x['resets']))
            if x.get('q0'):
                print('     to %.2f V: %.2f h under load  =  ~%.0f mAh, ~%.2f Wh at %.0f mA'
                      % (cutoff_v(), x['q0_hours'], x['mah'], x['wh'], x['current_ma']))
            print('     full %s V rested; sag after 15 min %s mV; pulse at 10 s %s mV (~%s ohm); temp %s..%s C'
                  % (fmt(x.get('v_full'), '%.3f'), fmt(x.get('load_sag_mv'), '%.0f'), fmt(x['pulse_mv'], '%.0f'),
                     fmt(x['pulse_mv'] / x['current_ma'] if x['pulse_mv'] else None, '%.2f'),
                     fmt(x['temp_min'], '%.1f'), fmt(x['temp_max'], '%.1f')))
            if not x.get('ok') and x.get('error'):
                print('     (no curve: %s)' % x['error'])
        else:
            tag = 'C%d%s' % (x['cycle'], ' recharge' if x['recharge'] else '')
            if x['done']:
                print('%-4s complete  %s to the end of charge: CC %s + CV %s; from %s V (%s V on the charger)'
                      % (tag, hm(x['t_done']), hm(x['t_cv']), hm(x['t_done'] - x['t_cv'] if x['t_cv'] is not None else None),
                         fmt(x['v_before'], '%.3f'), fmt(x['v_first'], '%.3f')))
            else:
                print('%-4s %s  %s so far%s' % (tag, 'CUT' if x['cut'] else 'running', hm(x['t_so_far']),
                                               ' (charger idle: cell above its restart point)' if x['idle'] else ''))
            print('     top %s V on the charger; step at 10 s %s mV; charge current at the start ~%s mA; temp %s..%s C'
                  % (fmt(x['v_top'], '%.3f'),
                     fmt((x['v10'] - x['v_before']) * 1000 if x['v10'] and x['v_before'] else None, '%+.0f'),
                     fmt(x.get('i_est_ma'), '%.0f'), fmt(x['temp_min'], '%.1f'), fmt(x['temp_max'], '%.1f')))
            if x.get('prog_n'):
                print('     R7 readings: %d; charge put in %s' % (x['prog_n'],
                      ('%.0f mAh (measured)' % x['prog_mah']) if x.get('prog_covers') else
                      ('~%.0f mAh, NOT COUNTED: %s' % (x.get('prog_mah', 0), prog_why(x)))))
            if x.get('relaxed_v'):
                print('     relaxed to %.3f V on the idle charger, %s after the end' % (x['relaxed_v'], hm(x['relaxed_s'])))
    if a.get('load_ma_measured'):
        lm = a['load_ma_measured']
        print('load current MEASURED from the charges: %.1f mA (assumed %.0f)' % (lm, LOAD_MA))
        for k in (1, 2):
            d = a['D'].get(k)
            if d and d.get('q0_hours'):
                print('     D%d to %.2f V: %.0f mAh, %.2f Wh' % (k, cutoff_v(), d['q0_hours'] * lm,
                      d['wh'] * lm / d['current_ma']))
    D = a['D']
    if 1 in D and 2 in D and D[1].get('q0') and D[2].get('q0'):
        print('D2 / D1 = %.2f %%  (the cell lost or gained this much between the two cycles)'
              % (100.0 * D[2]['q0'] / D[1]['q0']))


def capacity_h(a):
    qs = [a['D'][k]['q0_hours'] for k in (1, 2) if k in a['D'] and a['D'][k].get('q0')]
    return sum(qs) / len(qs) if qs else None


def compare(a, b, na, nb):
    print_report(a, na); print(); print_report(b, nb); print()
    print('=== %s against %s ===' % (nb, na))
    ca, cb = capacity_h(a), capacity_h(b)
    if ca and cb:
        print('capacity (mean of the measured discharges): %.2f h vs %.2f h under load  ->  %s holds %.1f %% of %s'
              % (ca, cb, nb, 100.0 * cb / ca, na))
        print('   (a ratio of times on the same board and load: the assumed current cancels)')
    for k in (1, 2):
        ta = a['C'].get(k, {}).get('t_done'); tb = b['C'].get(k, {}).get('t_done')
        if ta and tb:
            print('charge C%d: %s vs %s (%.0f %%)' % (k, hm(ta), hm(tb), 100.0 * tb / ta))
    for key, label, f in (('v_full', 'rested full', '%.3f V'), ('load_sag_mv', 'sag after 15 min', '%.0f mV'),
                          ('pulse_mv', 'pulse at 10 s', '%.0f mV')):
        va = [a['D'][k].get(key) for k in (1, 2) if k in a['D'] and a['D'][k].get(key) is not None]
        vb = [b['D'][k].get(key) for k in (1, 2) if k in b['D'] and b['D'][k].get(key) is not None]
        if va and vb:
            print('%-17s %s vs %s' % (label, f % (sum(va) / len(va)), f % (sum(vb) / len(vb))))
    # The two curves at the same state of charge.
    ta = next((a['D'][k] for k in (2, 1) if k in a['D'] and a['D'][k].get('ok')), None)
    tb = next((b['D'][k] for k in (2, 1) if k in b['D'] and b['D'][k].get('ok')), None)
    if ta and tb:
        print('voltage at:   ' + '  '.join('%4d%%' % p for p, _ in [(t[1], 0) for t in ta['table'][::3]]))
        print('  %-10s ' % na + '  '.join('%.3f' % t[0] for t in ta['table'][::3]))
        print('  %-10s ' % nb + '  '.join('%.3f' % t[0] for t in tb['table'][::3]))


# Which table in battery_source.cpp a cell's curve lives in. The 804050 (every v2 board) keeps the
# original markers; the 103450 (the v1 board) has its own, so --write never lands in the wrong one.
MARKERS = {'804050': 'CURVE-TABLE', '103450': 'CURVE-TABLE-103450'}


def current_table(src, marker='CURVE-TABLE'):
    body = src[src.index('// %s BEGIN' % marker):src.index('// %s END' % marker)]
    pts = [(float(v), float(p)) for v, p in re.findall(r'\{\s*([0-9.]+)f,\s*([0-9.]+)f\s*\}', body)]
    return pts + [(cutoff_v(), 0.0)]


def soc_on(table, v):
    if v >= table[0][0]:
        return 100.0
    for i in range(1, len(table)):
        if v >= table[i][0]:
            a, b = table[i - 1], table[i]
            return b[1] + (v - b[0]) / (a[0] - b[0]) * (a[1] - b[1])
    return 0.0


def render(table, note, marker='CURVE-TABLE', indent='        '):
    rows, line = [], []
    for v, p in table:
        line.append('{%.3ff, %5.1ff}' % (v, p))
        if len(line) == 4:
            rows.append(indent + ', '.join(line) + ','); line = []
    if line:
        rows.append(indent + ', '.join(line) + ',')
    return ('%s// %s BEGIN\n%s// %s\n' % (indent, marker, indent, note) + '\n'.join(rows) +
            '\n%s{BATTERY_CUTOFF_V, 0.0f},\n%s// %s END' % (indent, indent, marker))


def write_table(path, table, note, marker='CURVE-TABLE'):
    src = open(path).read()
    begin = '// %s BEGIN' % marker
    a = src.index(begin)
    a = src.rindex('\n', 0, a) + 1                  # the table keeps the indentation it was found at
    indent = src[a:src.index(begin)]
    b = src.index('// %s END' % marker) + len('// %s END' % marker)
    open(path, 'w').write(src[:a] + render(table, note, marker, indent) + src[b:])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--port', nargs='?', const='', default=None)
    src.add_argument('--bin'); src.add_argument('--udp')
    src.add_argument('--compare', nargs=2, metavar=('A.bin', 'B.bin'))
    ap.add_argument('--gain', type=float); ap.add_argument('--rest-ratio', type=float, default=REST_RATIO)
    ap.add_argument('--current', type=float, default=LOAD_MA); ap.add_argument('--cycle', type=int)
    ap.add_argument('--save'); ap.add_argument('--write', action='store_true'); ap.add_argument('--source')
    ap.add_argument('--erase', action='store_true'); ap.add_argument('--json', action='store_true')
    ap.add_argument('--cell', choices=sorted(MARKERS), default='804050')
    a = ap.parse_args()
    marker = MARKERS[a.cell]

    if a.compare:
        A, B = (analyse(parse_bin(open(f, 'rb').read()), gain=a.gain, current=a.current,
                        rest_ratio=a.rest_ratio) for f in a.compare)
        na, nb = (os.path.splitext(os.path.basename(f))[0] for f in a.compare)
        if a.json:
            ca, cb = capacity_h(A), capacity_h(B)
            print(json.dumps(dict(a=strip(A), b=strip(B), capacity_ratio=(cb / ca if ca and cb else None))))
            return 0
        compare(A, B, na, nb)
        return 0

    port = None
    if a.port is not None:
        blob, port = read_port(a.port or None)
        if a.save:
            open(a.save, 'wb').write(blob)
        recs = parse_bin(blob)
    elif a.bin:
        recs = parse_bin(open(a.bin, 'rb').read())
    else:
        recs = parse_udp(open(a.udp).read())

    an = analyse(recs, gain=a.gain, current=a.current, rest_ratio=a.rest_ratio)
    if not an['segments']:
        sys.exit('no run in the log')
    # The discharge the table comes from: the one asked for, else the last complete one, else the last.
    dis = [x for x in an['segments'] if x['type'] == 'discharge']
    if a.cycle:
        dis = [x for x in dis if x['cycle'] == a.cycle]
    pick = next((x for x in reversed(dis) if x.get('ok')), dis[-1] if dis else None)
    res = pick if pick else dict(ok=False, error='no discharge in the log')
    source = a.source or SOURCE
    if res.get('ok'):
        old = current_table(open(source).read(), marker)
        res['vs_current_max'] = max(abs(soc_on(old, v) - s) for s, v in res['points'])
    if a.json:
        out = dict(res) if not an['test'] else dict(res, test=strip(an))
        print(json.dumps(out)); return 0 if res.get('ok') else 1

    print_report(an)
    print()
    if not res.get('ok'):
        print('no table:', res.get('error'))
    else:
        print("table from D%d: %d samples; the firmware's table is off by up to %.1f points against it"
              % (res['cycle'], res['samples'], res['vs_current_max']))
        print(render(res['table'], 'measured by curve_bench', marker))
        if a.write:
            write_table(source, res['table'],
                        'measured by curve_bench: %d samples, %.1f h, rested full %.3f V'
                        % (res['samples'], res['hours'], res['v_full']), marker)
            print('written to', source)
    if a.erase and port:
        subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32c6', '-p', port,
                        'erase_region', hex(PART_ADDR), hex(PART_SIZE)], check=True)
        print('partition erased')
    return 0 if res.get('ok') else 1


def strip(an):
    """The analysis without the long point lists, for --json."""
    out = dict(test=an['test'], complete=an['complete'], segments=[], load_ma_measured=an.get('load_ma_measured'))
    for x in an['segments']:
        out['segments'].append({k: v for k, v in x.items() if k not in ('points', 'curve', 'relaxation', 'table', 'prog')})
    return out


if __name__ == '__main__':
    sys.exit(main())
