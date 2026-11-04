# Подставной прибор: его веб-API, ровно те же заголовки и та же аутентификация.
#
# Отдаёт НАСТОЯЩИЙ main/console.html, а не копию: копия разошлась бы с прошивкой в первый же день, и
# тест начал бы проверять вчерашнюю страницу.
#
#     tests/mock.py [порт]        поднять и открыть руками на http://127.0.0.1:8097
#     GET /stress?on=1            предельные данные: длинные имена, 12345 ppm, -40,5 C
#     GET /nobsec?on=1            прошивка, собранная без Bosch BSEC: индекса воздуха нет
#     GET /mock?wl=2&fab=0        подменить состояние прибора
#     GET /mock/saves             что страница сохранила
import json, time, threading, os, sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = os.path.join(HERE, '..', 'main', 'console.html')
S = {'mode':'STA', 'wl':2, 'fab':1, 'pending':False, 'conn':0, 'ssid':'HomeWiFi', 'setup':0,
     'remain':170, 'keep':False, 'rebooted':0, 'saves':[], 'no_wifi':0, 'toff':4.0, 'temp':22.9, 'seq':0, 'down':0,
     'co2lvl':1100,
     # Три запомненные сети (wifi_slots.h). Одна ничего не проверяет: карточка списка
     # появляется только когда выбор вообще есть.
     'nets':['HomeWiFi','Office-Guest','Phone-Hotspot'],
     # Калибровка CO2 — единственный необратимый путь в изделии, и до 2026-09-30 подставной
     # прибор её не изображал вовсе, то есть она не проверялась ничем.
     'cal':'idle', 'cal_n':0, 'cal_err':0, 'cal_settled':1, 'cal_unsettled':0, 'cal_lo':412, 'cal_hi':431, 'cal_avg':420, 'cal_corr':-350}
AUTH = {'pass':'Qw3rty!@', 'nonce':None, 'session':None, 'fails':0}

def sha256hex(x):
    import hashlib
    return hashlib.sha256(x.encode()).hexdigest()

def signed_in(handler):
    ck = handler.headers.get('Cookie') or ''
    return AUTH['session'] is not None and ('sm_auth=' + AUTH['session']) in ck

def status():
    now = int(time.time())
    d = _status_base(now)
    # Плата v2: ячейка читается делителем, и консоль показывает карточку калибровки.
    d['sensor'].setdefault('bat_src', 'divider')
    d['sensor'].setdefault('bat_cal', S.get('bat_cal', 1.0))
    # Карточка батареи. Прибор отдаёт три верхних числа из той же подгонки, что и «Measured draw»,
    # и придерживает их (0 и -1), пока заряд не упал больше, чем различает газомер
    # (power_estimate.h). По умолчанию — «ещё копит», как у прибора, только что снятого с кабеля.
    if S.get('bat', 'collecting') == 'measured':
        d['sensor'].update({'on_usb': False, 'bat_p': 83,
                            'pwr_ok': True, 'pwr_ua': 416.7, 'pwr_hours': 96, 'pwr_drop': 2.0,
                            'pwr_mv': 20, 'pwr_months': 6.7, 'pwr_conf': 1,
                            'bat_daily_p': 0.5, 'bat_daily_mah': 10.0, 'bat_tte_h': 3984})
    else:
        d['sensor'].update({'pwr_ok': False, 'bat_daily_p': 0, 'bat_daily_mah': 0, 'bat_tte_h': -1})
    # Сборка без Bosch BSEC (bsec_manager_none.cpp): индекса нет и не будет, прибор так и говорит.
    if S.get('nobsec'):
        d['sensor'].update({'iaq_built': False, 'iaq': 0, 'static_iaq': 0, 'iaq_acc': 0,
                            'gas_pct': 0, 'gas_res': 0, 'pollution_class': 0,
                            'pollution_text': 'No air-quality index'})
    if S.get('stress'):
        d['cfg']['dev_name'] = 'Гостиная на втором этаже у окна'
        d['cfg']['ssid'] = 'Очень длинное имя домашней сети 5 ГГц'
        d['cfg']['city'] = 'Garmisch-Partenkirchen, Bayern'
        d['cfg']['tg_chat'] = '-1001234567890123'
        d['cfg']['mq_host'] = 'homeassistant.fritz.box.local.lan'
        d['cfg']['tz_h'] = -9.5
        d['cfg']['lowbat_v'] = 3.55
        d['sensor'].update({'co2': 12345, 'temp': -40.5, 'hum': 100, 'pres': 108400,
                            'iaq': 498, 'static_iaq': 498, 'iaq_acc': 3,
                            'pollution_class': 3, 'pollution_text': 'Extremely polluted',
                            'bat_v': 4.185, 'bat_p': 100, 's_temp': -40.5, 's_hum': 100,
                            'b_temp': 125.5, 'b_hum': 3, 't_offset': 19.75, 'charging': True})
        d['net'].update({'ip': '192.168.100.200', 'rssi': -99, 'ssid': d['cfg']['ssid']})
        # Поля — как их реально шлёт прошивка (web_server.cpp getStatusHandler): ver/seq, а не fw;
        # uptime в МИЛЛИСЕКУНДАХ от текущей загрузки; ota со своими именами.
        d['sys'].update({'uptime': 99999999, 'boots': 65535, 'chip_temp': -40.0,
                         'hist_recs': 51200, 'reset': 'Brownout / power dip',
                         'ver': '0.10.4+69', 'seq': 69, 'heap_free': 9000})
        d['sys']['ota'] = {'checked': now - 3600, 'avail': '0.10.5+70', 'seq': 70,
                           'error': 'signature check failed on the downloaded image',
                           'auto': 1, 'can_revert': True, 'cur': 69, 'state': 3,
                           'why': 'signature', 'phase': 'Verifying the signature'}
    if S.get('show'):
        # Витрина для снимков гида на сайте (tests/shot_guide.js): обычный прибор в обычный день —
        # город задан, газ откалиброван, батарея измерена. Без этого гид показывал бы читателю
        # «Warming up» и «Add your city» как будто это нормальное состояние.
        d['cfg'].update({'wl_mode': S['wl'], 'city': 'Darmstadt, DE', 'wx_lat': 49.8728, 'wx_lon': 8.6512,
                         'dev_name': 'Living room', 'tg_en': 1, 'tg_set': True, 'tg_chat': '123456789',
                         'u_pres': 1})
        d['sys'].update({'ver': '0.10.30+95', 'seq': 95, 'uptime': 5400000})
        d['sys']['ota'].update({'cur': 95, 'checked': int(time.time()) - 5400})
        d['sensor'].update({'co2': 795, 'temp': 22.8, 'hum': 45, 'pres': 759.1, 's_temp': 22.8, 's_hum': 45,
                            'iaq': 38, 'static_iaq': 41, 'iaq_acc': 3, 'pollution_class': 0,
                            'pollution_text': 'Excellent', 'out_temp': 13.2, 'out_desc': 'Partly cloudy',
                            'out_icon': '02d', 'out_city': 'Darmstadt, DE', 'out_hum': 64, 'out_wind': 3.1,
                            'out_feels': 12.6, 'surface_rh': 57.0, 'bat_v': 3.98})
    return d

def _status_base(now):
    return {
     'sys':{'uptime':846000,'boots':3,'reset':'Deep Sleep','chip':'ESP32-C6 rev2','cpu_mhz':160,'epoch':now,
            'chip_temp':24.9,'heap_free':132000,'heap_total':373000,'heap_min':100000,'flash_kb':8192,
            'app_kb':3072,'hist_recs':960,'hist_cap':51200,'idf':'v5.5.2','portal_remain':(900 if S['keep'] else S['remain']),'portal_keep':S['keep'],
            'ver':'0.10.3+68','seq':68,'trusted':2,'ota':{'checked':0,'avail':'','seq':0,'error':'','auto':1,'can_revert':True,'cur':68,'state':0},'plan':{'profile':1,'night':False,'meas_min':15,'radio_min':0,'live':True,'last_meas':now-60,'last_co2':now-60}},
     'net':{'ssid':S['ssid'] if S['mode']=='STA' else 'Smart Meteo','ip':'192.168.1.42' if (S['mode']=='STA' or S['conn']==2) else '192.168.4.1',
            'mac':'02:00:00:12:34:56','rssi':-54,'ch':6,'gw':'192.168.1.1','dns':'192.168.1.1','mode':S['mode'],'conn_state':S['conn'],
            # По какой сети открыта страница. Прибор решает это по локальному адресу сокета; мок
            # просто переключается флагом, чтобы обе ветки оговорки были проверяемы.
            'own_ap':S.get('own_ap', False),
            # Прибор это отдаёт (web_server.cpp net["online"]), а мок не отдавал — расхождение в
            # подставном приборе, а не в консоли.
            'online':S['mode']=='STA'},
     'sensor':{'co2':601,'temp':S['temp'],'hum':47,'pres':0,'iaq':50,'static_iaq':50,'iaq_acc':0,'gas_pct':0,'gas_res':52000,
            'pollution_class':0,'pollution_text':'Warming up','bat_v':4.18,'bat_p':99,'charging':False,'on_usb':True,
            's_temp':S['temp'],'s_hum':47,'b_temp':24.2,'b_hum':44,'t_offset':S['toff'],
            'out_temp':16.2,'out_uv':4,'out_cloud':62,'out_dew':11.5,
            # Влажность на холодной стене — то, на чём прибор судит сырость. Настоящий
            # отдаёт null, когда уличной температуры нет; здесь число, чтобы строка жила.
            'surface_rh':76.4,
            'p_sea':1009.3, 'baro_k':1.0172, 'baro_src':'measured',
            'co2cal':S['cal'],'co2cal_n':S['cal_n'],'co2cal_need':6,'co2cal_err':S['cal_err'],
            'co2cal_lo':S['cal_lo'],'co2cal_hi':S['cal_hi'],'co2cal_avg':S['cal_avg'],
            'co2cal_corr':S['cal_corr'],
            'co2cal_settled':S['cal_settled'],'co2cal_unsettled':S['cal_unsettled']},
     'cfg':{'wl_mode':S['wl'],'nets':S['nets'],'profile':1,'live_clk':1,'ssid':S['ssid'],'pass_set':bool(S['ssid']),'s_int':15,'w_int':30,
            'nm_en':1,'nm_sh':23,'nm_eh':7,'tz_h':2,'lowbat_v':3.3,'bat_mah':2000,'city':'','owm_set':False,
            'tg_en':0,'tg_set':False,'tg_chat':'','dev_name':'Test','g_span':24,'mq_tls':0,'al_co2':S['co2lvl'],'al_co2en':1,
            'al_voc':1,'al_hum':1,'u_temp':0,'u_pres':0,'mq_host':'','mq_pass_set':True,'mq_port':1883,'mq_topic':'smartmeteo','ha_id':'smartmeteo_a1b2c3','mq_user':'',
            'mt_fab':S['fab'],'h_step':15,'wx_src':0,'no_wifi':S['no_wifi']}}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype='application/json', extra=None):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(b)))
        for k,v in (extra or {}).items(): self.send_header(k, v)
        for k,v in getattr(self,'extra_headers',[]) or []: self.send_header(k,v)
        self.extra_headers=[]
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query, keep_blank_values=True)
        if u.path == '/stress':
            S['stress'] = (q.get('on', ['1'])[0] == '1')
            return self.send(200, json.dumps({'stress': S['stress']}))
        if u.path == '/nobsec':
            S['nobsec'] = (q.get('on', ['1'])[0] == '1')
            return self.send(200, json.dumps({'nobsec': S['nobsec']}))
        if u.path == '/ownap':
            S['own_ap'] = (q.get('on', ['1'])[0] == '1')
            return self.send(200, json.dumps({'own_ap': S['own_ap']}))
        if u.path == '/':
            # the same headers the device now sends, so the page is tested under them
            self.extra_headers = [
              ('Content-Security-Policy', "default-src 'none'; script-src 'unsafe-inline'; "
               "style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; manifest-src 'self'; form-action 'none'; "
               "frame-ancestors 'none'; base-uri 'none'"),
              ('X-Content-Type-Options', 'nosniff'),
              ('Referrer-Policy', 'no-referrer')]
            return self.send(200, open(CONSOLE,'rb').read(), 'text/html',
                             {'Set-Cookie':'sm_setup=%d; Path=/' % S['setup']})
        if u.path == '/mock':
            for k,v in q.items():
                v=v[0]
                if k == 'nets': S[k] = [x for x in v.split(',') if x]; continue
                S[k] = int(v) if v.lstrip('-').isdigit() else v
            return self.send(200, json.dumps({k:S[k] for k in S if k!='saves'}))
        if u.path == '/mock/saves': return self.send(200, json.dumps(S['saves']))
        if u.path.startswith('/api/') and not signed_in(self):
            return self.send(401, '{"auth":"required"}')
        if u.path == '/api/status':
            if S.get('down'): return self.send(503, 'down', 'text/plain')
            return self.send(200, json.dumps(status()))
        if u.path == '/api/matter':
            return self.send(200, json.dumps({'selected':S['wl']==2,'fabrics':S['fab'],'forget_pending':S['pending'],
                                              'ok':True,'qr':'MT:Y.K9042C00KA0648G00','manual':'3497-011-2332'}))
        if u.path == '/api/matter/qr.svg':
            return self.send(200, '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect width="10" height="10" fill="#fff"/><rect x="2" y="2" width="6" height="6"/></svg>', 'image/svg+xml')
        if u.path == '/api/scan':
            # auth: "open" | "psk" | "enterprise" — прошивка владеет перечислением esp_wifi и
            # отвечает словом (WiFiNetwork). Корпоративная сеть здесь обязательна: прибор её не
            # умеет вовсе, и до 2026-10-01 она выглядела как домашняя.
            return self.send(200, json.dumps([
                {'ssid':'HomeWiFi','rssi':-50,'enc':True,'auth':'psk'},
                {'ssid':'CORP-WIFI','rssi':-55,'enc':True,'auth':'enterprise'},
                {'ssid':'Other','rssi':-70,'enc':True,'auth':'psk'}]))
        if u.path == '/api/events.bin':
            import struct, time as _t
            HDR = struct.Struct("<4sHHIII"); REC = struct.Struct("<IIHBBBBBBHHHHHbbI")
            now = int(_t.time())
            evs = [(1, 1), (2, 2 | (1 << 8)), (2, 2 | (2 << 8)), (3, 3), (9, 9 | (1 << 8)),
                   (4, 4), (6, 6 | (68 << 8)), (5, 5), (8, 8), (7, 7)]
            body = b''
            for i, (n, detail) in enumerate(evs):
                body += REC.pack(i + 1, now - (len(evs) - i) * 600, 900, 11, 3, 0, 0, 0, 0,
                                 130, 700, 0, 4100, 0, -54, 24, detail)
            # плюс пара записей других видов — их карточка обязана отфильтровать
            body += REC.pack(99, now - 60, 900, 0, 2, 0, 0, 0, 0, 130, 700, 0, 4100, 0, -54, 24, 0)
            return self.send(200, HDR.pack(b"SMEV", 1, REC.size, 8192, len(evs), len(evs) + 1) + body,
                             'application/octet-stream')
        if u.path == '/api/history':
            if not S.get('show'): return self.send(200, '[]')
            import math, time as _t
            now = int(_t.time()); rows = []
            for i in range(96, -1, -1):          # сутки по 15 минут: ночь в закрытой комнате, утро с окном
                t = now - i * 900; h = (t // 3600) % 24
                co2 = int(560 + 520 * max(0.0, math.sin((h - 15) / 24 * 2 * math.pi)) + 40 * math.sin(i / 3))
                # Класс вердикта намеренно смешанный: полосу под графиком и её легенду нельзя
                # проверить на ряде из одних нулей — одинаковые значения не отличают «тихо» от
                # «не нарисовано». Класс 4 снят с 0.9.55 и лежит в самых старых записях: ровно так
                # он и встречается на приборе, у которого история глубже последнего обновления.
                cls = 4 if i >= 95 else 5 if 40 <= i <= 47 else 1 if co2 >= 1000 else 0
                rows.append({'t': t, 'co2': co2, 'temp': round(22 + math.sin(i / 16), 1), 'hum': 45 + i % 5,
                             'pres': round(759.1 - i * 0.015, 1), 'iaq': 30 + (i * 7) % 40, 'gas': 52000, 'cls': cls,
                             'soc': 83 - i * 0.02, 'mv': 3980 + i // 4,
                             # Климат BME690 пишется только на газовых тиках, остальные записи несут 0 —
                             # ровно так он лежит в истории прибора.
                             'bt': round(24.5 + math.sin(i / 10), 1) if i % 3 == 0 else 0,
                             'bh': 40 + i % 7 if i % 3 == 0 else 0})
            return self.send(200, json.dumps(rows))
        return self.send(200, '{}')
    def do_POST(self):
        _u = urlparse(self.path)
        if _u.path == '/api/batcal':
            if not signed_in(self): return self.send(401, '{"ok":0}')
            n = int(self.headers.get('Content-Length') or 0)
            b = json.loads(self.rfile.read(n) or '{}')
            ref, raw = float(b.get('cell_v') or 0), 4.07
            if not (3.0 <= ref <= 4.5) or not (0.95 <= ref / raw <= 1.05):
                return self.send(200, '{"ok":false,"error":"range"}')
            S['bat_cal'] = ref / raw
            return self.send(200, json.dumps({'ok': True, 'gain': ref / raw, 'raw': raw, 'cell': ref}))
        if _u.path == '/api/portal':
            if not signed_in(self): return self.send(401, '{"ok":0}')
            n = int(self.headers.get('Content-Length') or 0)
            b = json.loads(self.rfile.read(n) or '{}')
            S['keep'] = bool(b.get('keep'))
            return self.send(200, json.dumps({'ok': True, 'keep': S['keep']}))
        if _u.path == '/api/trust/forget':
            d = status(); return self.send(200, json.dumps({'ok':True,'forgot':2}))
        if _u.path == '/api/auth/challenge':
            import secrets, time as _t
            if AUTH.get('lock', 0) > _t.time():
                return self.send(429, '{"locked":1}')
            if not AUTH['nonce'] or _t.time() - AUTH.get('nonce_at', 0) >= 60:
                AUTH['nonce'] = secrets.token_hex(16); AUTH['nonce_at'] = _t.time()
            return self.send(200, json.dumps({'nonce': AUTH['nonce']}))
        if _u.path == '/api/auth':
            import time as _t
            if AUTH.get('lock', 0) > _t.time():
                return self.send(429, '{"locked":1}')
            n = int(self.headers.get('Content-Length') or 0)
            b = json.loads(self.rfile.read(n) or '{}')
            fresh = (AUTH['nonce'] and b.get('nonce') == AUTH['nonce']
                     and _t.time() - AUTH.get('nonce_at', 0) < 60)
            ok = fresh and b.get('resp') == sha256hex(AUTH['nonce'] + ':' + AUTH['pass'])
            if fresh: AUTH['nonce'] = None
            if not fresh:
                return self.send(401, '{"ok":0,"stale":1}')
            if not ok:
                AUTH['fails'] = AUTH.get('fails', 0) + 1
                if AUTH['fails'] >= 5:
                    AUTH['lock'] = _t.time() + 60; AUTH['fails'] = 0
                    return self.send(401, '{"ok":0}')
            if ok:
                AUTH['fails'] = 0
                import secrets
                AUTH['session'] = secrets.token_hex(16)
                self.send_response(200)
                self.send_header('Content-Type','application/json')
                self.send_header('Set-Cookie','sm_auth=%s; Path=/' % AUTH['session'])
                body = b'{"ok":1}'
                self.send_header('Content-Length', str(len(body)))
                self.end_headers(); self.wfile.write(body)
                return
            return self.send(401, '{"ok":0}')
        if _u.path.startswith('/api/') and not signed_in(self):
            return self.send(401, '{"auth":"required"}')
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        try: body = json.loads(raw) if raw else {}
        except Exception: body = {}
        p = urlparse(self.path).path
        S['saves'].append({'path':p,'body':body,'t':time.strftime('%H:%M:%S')})
        if p == '/api/save':
            if 'forget_net' in body:
                S['nets'] = [n for n in S['nets'] if n != body['forget_net']]
            if 'ssid' in body and body['ssid']:
                S['nets'] = [body['ssid']] + [n for n in S['nets'] if n != body['ssid']]
                S['nets'] = S['nets'][:3]
            if 'wl_mode' in body: S['wl'] = int(body['wl_mode'])
            if 'no_wifi' in body: S['no_wifi'] = int(body['no_wifi'])
            # Уровень тревоги CO2 запоминается и зажимается так же, как в прошивке: без этого круг
            # «сохранил — опрос вернул» не проверить, а именно на нём и ломался уровень вне списка.
            if 'al_co2' in body:
                S['co2lvl'] = max(700, min(1600, int(body['al_co2'])))
            if 'ssid' in body:
                new = body['ssid']
                if S['mode'] == 'STA':
                    S['conn'] = 2 if new == S['ssid'] else 3
                else:
                    S['conn'] = 1
                    def later(ok=('bad' not in new)):
                        time.sleep(2.5); S['conn'] = 2 if ok else -1
                    threading.Thread(target=later, daemon=True).start()
                S['ssid'] = new
            return self.send(200, '{"ok":true}')
        if p == '/api/password':
            v = body.get('new','')
            AL = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVXYZ0123456789"
                  "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~")
            ok = len(v) == 8 and all(c in AL for c in v)
            if ok: AUTH['pass'] = v
            return self.send(200, json.dumps({'ok': ok} if ok else {'ok': False, 'error': 'rules'}))
        if p == '/api/calibrate':
            if body.get('cancel'):
                S['cal'] = 'failed'; S['cal_err'] = 4
            elif body.get('ref'):
                S['cal'] = 'running'; S['cal_n'] = 0; S['cal_err'] = 0
                S['calref'] = int(body['ref'])
            return self.send(200, json.dumps({'ok':True,'pending':True,'samples':S['cal_n']}))
        if p == '/api/reboot': S['rebooted'] += 1; return self.send(200, '{"ok":true,"msg":"Rebooting..."}')
        if p == '/api/poweroff': S['poweroff'] = S.get('poweroff', 0) + 1; return self.send(200, '{"ok":true,"msg":"Switching off..."}')
        if p == '/api/matter/forget':
            S['pending'] = not body.get('cancel', False)
            return self.send(200, json.dumps({'ok':True,'forget_pending':S['pending']}))
        if p == '/api/tempcal':
            ref = float(body.get('ref', 0)); was = S['temp']
            S['toff'] = S['toff'] + (was - ref); S['temp'] = ref
            # Как настоящий: отдаём показание, по которому считали. Оно НАМЕРЕННО не равно тому,
            # что страница показывает после — там уже следующий замер.
            return self.send(200, json.dumps({'ok':True,'offset':S['toff'],'was':4.0,
                                              'delta':round(ref-was,2),'read':was,'clamped':False}))
        return self.send(200, '{"ok":true}')
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8097
srv = ThreadingHTTPServer(('127.0.0.1', PORT), H)
print('mock на http://127.0.0.1:%d, пароль %s' % (PORT, AUTH['pass']), flush=True)
srv.serve_forever()
