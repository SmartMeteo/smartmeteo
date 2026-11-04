#!/usr/bin/env python3
"""Catch the discharge bench's progress on the home network (main/curve_bench.cpp broadcasts it).

    tools/curve_listen.py [--port 47800] [--out curve_udp.log]

Every line is appended to the log with the time it arrived, and a short summary is printed. The log
is only a view: the authoritative record is in the device's flash (tools/curve_fit.py --port), and
tools/curve_fit.py --udp FILE reads this one for a look at a run still in progress.
"""
import argparse, socket, time

ap = argparse.ArgumentParser()
ap.add_argument('--port', type=int, default=47800)
ap.add_argument('--out', default='curve_udp.log')
a = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', a.port))
print('listening on udp %d, writing %s' % (a.port, a.out), flush=True)
with open(a.out, 'a') as f:
    while True:
        data, addr = s.recvfrom(512)
        line = data.decode('ascii', 'replace').strip()
        stamp = time.strftime('%Y-%m-%d %H:%M:%S')
        f.write('%s %s %s\n' % (stamp, addr[0], line)); f.flush()
        p = line.split(',')
        if p[0] == 'CURVE' and len(p) == 10:
            print('%s  #%s kind %s  %.1f h  rest %.3f V  load %.3f V  (gain %s)'
                  % (stamp, p[1], p[2], int(p[3]) / 3600.0, float(p[5]) * float(p[7]),
                     float(p[6]) * float(p[7]), p[7]), flush=True)
        elif p[0] == 'CURVE-HB' and len(p) == 5:
            print('%s  alive  %.1f h  under load %.3f V' % (stamp, int(p[1]) / 3600.0,
                  float(p[3]) * float(p[4])), flush=True)
