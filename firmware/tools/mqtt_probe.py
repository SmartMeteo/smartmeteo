#!/usr/bin/env python3
"""
Проверить брокер MQTT так, как это делает прибор — и сказать, ЧТО именно ответил брокер.

Зачем отдельный инструмент: консоль прибора сваливает «не достучался» и «не пустили» в одно
слово `connect`, хотя сам брокер отвечает кодом. Здесь этот код называется словами.

Пароль НЕ передаётся аргументом и нигде не сохраняется: он спрашивается с клавиатуры, не
печатается на экран и живёт только в памяти процесса. В истории оболочки его тоже не останется.

    python3 tools/mqtt_probe.py --host 192.168.1.50 --user smartmeteo
    python3 tools/mqtt_probe.py --host 192.168.1.50 --user smartmeteo --weather
    python3 tools/mqtt_probe.py --host 192.168.1.50 --user smartmeteo --watch 120

Без зависимостей: MQTT 3.1.1 поверх сокета, сорок строк протокола. Ставить ничего не нужно.
"""
import argparse, getpass, json, socket, struct, sys, time

def rl(n):                      # remaining length, переменной длины
    out = bytearray()
    while True:
        b = n % 128; n //= 128
        if n: b |= 0x80
        out.append(b)
        if not n: return bytes(out)

def s_(b):                      # строка с длиной впереди
    return struct.pack(">H", len(b)) + b

def read_rl(sock):
    n = 0; mult = 1
    while True:
        c = sock.recv(1)
        if not c: raise ConnectionError("брокер закрыл соединение")
        n += (c[0] & 0x7F) * mult
        if not c[0] & 0x80: return n
        mult *= 128

CONNACK = {
    0: "соединение принято — учётные данные подошли",
    1: "брокер не поддерживает эту версию протокола",
    2: "идентификатор клиента отвергнут",
    3: "сервис недоступен",
    4: "НЕВЕРНОЕ имя пользователя или пароль",
    5: "НЕ АВТОРИЗОВАН — брокер отказал",
}

# Коды 4 и 5 различает не протокол, а брокер, и различает как хочет. Mosquitto с внешней
# аутентификацией (надстройка Home Assistant — как раз такая) отвечает ПЯТЁРКОЙ на всё подряд:
# и на несуществующего пользователя, и на неверный пароль, и на анонимный вход. Проверено на
# живом брокере 2026-09-21: анонимная попытка и попытка с выдуманным именем дали один и тот же
# код 5. Поэтому обещать здесь «код скажет, что именно не так» нельзя — скажет только «отказ»,
# а причину назовёт журнал самой надстройки.
REFUSAL_NOTE = ("причину этот код не называет: Mosquitto с аутентификацией через Home Assistant\n"
                "   отвечает одинаково и на неизвестного пользователя, и на неверный пароль.\n"
                "   Точный ответ — в журнале надстройки: Settings -> Add-ons -> Mosquitto -> Log")

def connect(host, port, user, password, cid=b"smartmeteo-probe", timeout=6):
    flags = 0x02                                        # clean session
    payload = s_(cid)
    if user:
        flags |= 0x80; payload += s_(user.encode())
        if password is not None:
            flags |= 0x40; payload += s_(password.encode())
    var = s_(b"MQTT") + bytes([4, flags]) + struct.pack(">H", 30)
    body = var + payload
    sock = socket.socket(); sock.settimeout(timeout)
    sock.connect((host, port))
    sock.send(bytes([0x10]) + rl(len(body)) + body)
    hdr = sock.recv(1)
    if not hdr or hdr[0] != 0x20:
        sock.close(); raise ConnectionError("это не ответ MQTT — на порту что-то другое")
    read_rl(sock); rest = sock.recv(2)
    return sock, rest[1]

SAMPLE = {
    "temp": 14.2, "feels": 12.8, "tmin": 9.1, "tmax": 17.4,
    "hum": 71, "wind": 4.9, "desc": "probe", "icon": "10d", "city": "Probe",
    "forecast": [
        {"date": "2026-09-22", "icon": "03d", "tmin": 10.2, "tmax": 18.0},
        {"date": "2026-09-23", "icon": "01d", "tmin": 8.7,  "tmax": 19.5},
        {"date": "2026-09-24", "icon": "10d", "tmin": 11.0, "tmax": 16.2},
    ],
}

def publish(sock, topic, payload, retain=True):
    body = s_(topic.encode()) + payload.encode()        # QoS 0: идентификатора нет
    sock.send(bytes([0x30 | (1 if retain else 0)]) + rl(len(body)) + body)

def watch(sock, topic, seconds):
    body = struct.pack(">H", 1) + s_(topic.encode()) + bytes([0])
    sock.send(bytes([0x82]) + rl(len(body)) + body)
    sock.settimeout(1.0)
    end = time.time() + seconds
    print("слушаю %s в течение %d с (Ctrl-C чтобы прекратить)\n" % (topic, seconds))
    while time.time() < end:
        try:
            hdr = sock.recv(1)
            if not hdr: break
            n = read_rl(sock)
            data = b""
            while len(data) < n:
                chunk = sock.recv(n - len(data))
                if not chunk: break
                data += chunk
            if hdr[0] >> 4 == 3:                        # PUBLISH
                tl = struct.unpack(">H", data[:2])[0]
                t = data[2:2+tl].decode(errors="replace")
                msg = data[2+tl:]
                print("[%s] %d байт: %s" % (t, len(msg), msg[:400].decode(errors="replace")))
        except socket.timeout:
            sock.send(bytes([0xC0, 0x00]))              # PINGREQ, чтобы брокер не отключил
        except KeyboardInterrupt:
            break

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--user", default="")
    p.add_argument("--topic", default="smartmeteo")
    p.add_argument("--weather", action="store_true",
                   help="опубликовать образец погоды в <topic>/weather (retained)")
    p.add_argument("--watch", type=int, metavar="СЕК",
                   help="слушать <topic>/# указанное число секунд")
    a = p.parse_args()

    pw = getpass.getpass("пароль для %s (не отображается): " % (a.user or "анонимного входа")) if a.user else None

    try:
        sock, rc = connect(a.host, a.port, a.user, pw)
    except Exception as e:
        print("не удалось даже открыть соединение: %s: %s" % (type(e).__name__, e))
        print("значит дело не в учётных данных — проверьте адрес, порт и сеть.")
        return 2

    print("брокер ответил: код %d — %s" % (rc, CONNACK.get(rc, "неизвестный код")))
    if rc != 0:
        if rc in (4, 5): print("   " + REFUSAL_NOTE)
        sock.close(); return 1

    if a.weather:
        publish(sock, a.topic + "/weather", json.dumps(SAMPLE))
        print("образец погоды опубликован в %s/weather (retained) — прибор возьмёт его в "
              "следующем радиоокне" % a.topic)
        time.sleep(0.5)
    if a.watch:
        watch(sock, a.topic + "/#", a.watch)

    sock.send(bytes([0xE0, 0x00]))                      # DISCONNECT
    sock.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
