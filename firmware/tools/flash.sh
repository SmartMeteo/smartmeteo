#!/usr/bin/env bash
# Собрать, ПОДПИСАТЬ, залить. Единственный правильный способ положить прошивку на прибор.
#
# Почему не `idf.py flash`: включён SECURE_SIGNED_ON_UPDATE, и IDF проверяет на старте, что САМ
# работающий образ подписан — иначе он не сможет проверить следующий, и вызывается abort() ещё до
# app_main. Неподписанная заливка даёт крэш-петлю (наблюдалось: 55 загрузок за 45 с), а счётчик
# незавершённых загрузок её не ловит, потому что живёт внутри цикла, до которого дело не доходит.
# Вылезло это 2026-08-11 на первой же заливке вечера.
#
# Ключ намеренно лежит ВНЕ репозитория и НЕ прописан в sdkconfig: путь к нему сделал бы проект
# несобираемым на любой другой машине (те же грабли, что с ArduinoJson, §4.31). Поэтому подпись —
# отдельный шаг здесь, а сборка остаётся воспроизводимой и без ключа.

set -euo pipefail

# One name for the signing key across both scripts. SMARTMETEO_KEY is still honoured so that a
# shell already exporting it keeps working, but sign_release.sh has always used the longer name and
# that is the one to set: two names for one secret is how you end up signing with the wrong key and
# only finding out when a device refuses the image.
KEY="${SMARTMETEO_SIGNING_KEY:-${SMARTMETEO_KEY:-$HOME/.smartmeteo-signing/smartmeteo_sbv2_key1.pem}}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

# The same sources must give the same image, and without this they do not: ESP-IDF's openthread
# component bakes the build time into its version string — `string(TIMESTAMP ...)` in its
# CMakeLists, unconditional, unaffected by CONFIG_APP_REPRODUCIBLE_BUILD. Two builds of one tree
# nine hours apart differed in exactly five bytes, "09:55:15" against "19:20:34", and therefore in
# every hash below. CMake honours SOURCE_DATE_EPOCH for that call, so the timestamp is pinned to the
# commit the tree is at: a release rebuilt from its commit comes out byte for byte the same, which
# is the whole reason the sources are in git at all. A tree with no commits falls back to a fixed
# date rather than to "now".
export SOURCE_DATE_EPOCH="$(git -C "$HERE" log -1 --format=%ct 2>/dev/null || echo 1700000000)"

[ -f "$KEY" ] || { echo "нет ключа: $KEY  (задай SMARTMETEO_SIGNING_KEY)"; exit 1; }
[ -n "${IDF_PATH:-}" ] || { echo "среда ESP-IDF не активирована: . \$HOME/esp/v5.5.2/esp-idf/export.sh"; exit 1; }
# С 0.10.0 в образе есть Matter: без среды esp-matter CMake проекта останавливается сам.
[ -n "${ESP_MATTER_PATH:-}" ] || { echo "среда esp-matter не активирована: . \$HOME/esp/esp-matter/export.sh"; exit 1; }

echo "── сборка ────────────────────────────────────────────"
idf.py build >/dev/null || { idf.py build; exit 1; }

echo "── подпись ───────────────────────────────────────────"
python3 "$IDF_PATH/components/esptool_py/esptool/espsecure.py" \
        sign_data --version 2 --keyfile "$KEY" \
        --output build/smart-meteo-signed.bin build/smart-meteo.bin | tail -1
python3 "$IDF_PATH/components/esptool_py/esptool/espsecure.py" \
        verify_signature --version 2 --keyfile "$KEY" build/smart-meteo-signed.bin \
        | grep -i success || { echo "ПОДПИСЬ НЕ ПРОВЕРИЛАСЬ — не заливаю"; exit 1; }

# Крэш-дамп нечитаем без своего ELF, а хеш печатает сам прибор — по нему и храним.
H=$(python3 -c "b=open('build/smart-meteo.bin','rb').read(); print(b[0x20+144:0x20+176].hex()[:9])")
mkdir -p coredumps && cp build/smart-meteo.elf "coredumps/app_${H}.elf"
echo "ELF сохранён: coredumps/app_${H}.elf"

echo "── заливка ───────────────────────────────────────────"
# Прибор спит и бодрствует секундами, поэтому порт существует урывками. Опрос раз в две секунды в
# это окно не попадает; каждые 150 мс — попадает с первой попытки.
# ПО УМОЛЧАНИЮ ЗАЛИВАЮТСЯ otadata И ПРИЛОЖЕНИЕ, и это осознанно: не трогая таблицу разделов, мы не
# можем случайно потерять NVS, историю или калибровку. otadata пишется всегда, как в build/flash_args
# ESP-IDF: после обновления по воздуху прибор мог грузиться из app1, и один app0 не запустился бы. Но когда partitions.csv менялся, одного
# приложения МАЛО — новый образ будет искать раздел, которого на приборе нет, и промолчит об этом
# (регистратор просто ничего не запишет). Тогда нужен --full.
#
# --full пишет загрузчик, таблицу разделов, otadata и приложение. Смещения существующих разделов
# при этом не меняются, поэтому данные целы; otadata переписывается на "грузиться из app0" — туда
# же, куда кладётся образ, так что состояние остаётся согласованным.
FULL=0
ARGS=()
for a in "$@"; do
  if [ "$a" = "--full" ]; then FULL=1; else ARGS+=("$a"); fi
done
if [ "$FULL" = "1" ]; then
  echo "   режим: ПОЛНАЯ заливка (загрузчик + таблица разделов + otadata + приложение)"
  # Сверяем, что таблица в сборке не двигает существующие разделы. Потерять историю молча дороже,
  # чем потратить секунду на проверку.
  python3 - <<'CHK'
import subprocess, os, sys
idf = os.environ["IDF_PATH"]
out = subprocess.run([sys.executable, f"{idf}/components/partition_table/gen_esp32part.py",
                      "build/partition_table/partition-table.bin"],
                     capture_output=True, text=True).stdout
# Offsets moved on 2026-09-16 when the table was rebalanced for the unified Wi-Fi+Matter image:
# nvs 24K->64K, app slots 2560K->3072K, storage 2048K->1600K, events 768K->256K. app0 did NOT move —
# app partitions are 64K-aligned and the grown nvs still fitted below 0x20000. Everything after it did.
want = {"nvs": "0x9000", "app0": "0x20000", "app1": "0x320000",
        "storage": "0x620000", "coredump": "0x7b0000"}
bad = []
for line in out.splitlines():
    if line.startswith("#") or not line.strip():
        continue
    f = [x.strip() for x in line.split(",")]
    if len(f) >= 4 and f[0] in want and f[3].lower() != want[f[0]]:
        bad.append(f"{f[0]}: {f[3]} вместо {want[f[0]]}")
if bad:
    print("РАЗДЕЛЫ СДВИНУЛИСЬ — заливка стёрла бы данные:"); [print("  " + b) for b in bad]
    sys.exit(1)
print("   разделы на прежних местах — данные переживут")
CHK
  [ $? -eq 0 ] || exit 1
fi

# set -u makes an empty array expansion an error, so guard it: with no extra arguments this must
# still run rather than abort after the image is already built and signed.
FULL=$FULL python3 - ${ARGS[@]+"${ARGS[@]}"} <<'PY'
import glob, os, subprocess, sys, time
full = os.environ.get('FULL') == '1'
img = (['0x0',     'build/bootloader/bootloader.bin',
        '0x8000',  'build/partition_table/partition-table.bin',
        '0x19000', 'build/ota_data_initial.bin',
        '0x20000', 'build/smart-meteo-signed.bin'] if full else
       ['0x19000', 'build/ota_data_initial.bin',
        '0x20000', 'build/smart-meteo-signed.bin'])
deadline = time.time() + 300
tries = 0
# Two boards on the same Mac (two battery benches side by side, 2026-10-05): the first port in the
# glob is a coin toss. FLASH_PORT names the port; FLASH_MAC makes the write refuse any other board.
want_port = os.environ.get('FLASH_PORT')
want_mac = (os.environ.get('FLASH_MAC') or '').lower()
while time.time() < deadline:
    p = [want_port] if want_port and os.path.exists(want_port) else ([] if want_port else glob.glob('/dev/cu.usbmodem*'))
    if not p:
        time.sleep(0.15); continue
    if not want_port and len(p) > 1:
        print("НЕСКОЛЬКО ПОРТОВ: %s — укажи FLASH_PORT (и FLASH_MAC)" % ', '.join(p)); sys.exit(2)
    tries += 1
    if want_mac:
        rm = subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32c6', '-p', p[0],
                             '--before', 'default_reset', '--after', 'no_reset', 'read_mac'],
                            capture_output=True, text=True, timeout=60)
        if rm.returncode != 0:
            time.sleep(0.3); continue
        got = [l.split(':', 1)[1].strip().lower() for l in rm.stdout.splitlines() if l.startswith('BASE MAC')]
        if not got or got[0] != want_mac:
            print("НА %s НЕ ТА ПЛАТА: %s, ждали %s — не пишу" % (p[0], got[0] if got else '?', want_mac)); sys.exit(2)
    if not full:
        # The offset check above reads the table in build/, not the one on the device. Without --full the
        # device's own table decides where the image lands, so read it first: on a device still on the
        # pre-2026-09-16 layout (2560K slots, otadata at 0xF000) a 3 MB image would run over app1 and the
        # new otadata would go where nothing reads it — a device that boots nothing until --full.
        dev_pt = '/tmp/smartmeteo_device_pt.bin'
        rr = subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32c6', '-p', p[0],
                             '-b', '460800', '--before', 'default_reset', '--after', 'no_reset',
                             'read_flash', '0x8000', '0xC00', dev_pt],
                            capture_output=True, text=True, timeout=60)
        if rr.returncode != 0:
            time.sleep(0.3); continue
        want_pt = open('build/partition_table/partition-table.bin', 'rb').read()
        if open(dev_pt, 'rb').read() != want_pt[:0xC00]:
            print("ТАБЛИЦА РАЗДЕЛОВ НА ПРИБОРЕ ДРУГАЯ — обычная заливка не сработает, нужна --full "
                  "(NVS, история и журнал при этом не сохранятся)")
            sys.exit(2)
    r = subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32c6', '-p', p[0],
                        '-b', '460800', '--before', 'default_reset', '--after', 'hard_reset',
                        'write_flash'] + img,
                       capture_output=True, text=True, timeout=180)
    if r.returncode == 0:
        print(f"ЗАЛИТО с попытки {tries}" + (" (полная)" if full else "")); sys.exit(0)
    time.sleep(0.3)
print(f"не поймал окно порта за 5 минут (попыток {tries}) — передёрни кабель или подержи кнопку")
sys.exit(1)
PY
