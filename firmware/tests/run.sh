#!/usr/bin/env bash
# Все проверки одной командой. ESP-IDF для этого не нужен: код прибора собирается этим компьютером.
#
#     tests/run.sh              проверить всё
#     tests/run.sh t_weather    только один набор (t_analytics, t_weather, t_portal, t_sunmoon, t_cycle, t_curve, t_screen, t_console, t_browser)
#     tests/run.sh --accept     принять новый вид экрана как правильный (посмотрев на него глазами)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$HERE/../main"
AJSON="$HERE/../components/ArduinoJson/src"
ONLY="${1:-}"
FAILED=0

# Санитайзеры включены намеренно: чтение за буфером в разборе ответа сервера — это ровно тот класс
# ошибок, ради которого стоит запускать разбор на компьютере, а не на приборе.
CXXFLAGS=(-std=gnu++17 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
          -Wall -Wno-unused-function -I"$HERE" -I"$HERE/stubs" -I"$MAIN" -I"$AJSON")

run_cpp() {   # имя, дополнительные исходники…
    local name="$1"; shift
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && [ "$ONLY" != "--accept" ] && return 0
    printf '\n\033[1m== %s ==\033[0m\n' "$name"
    if ! c++ "${CXXFLAGS[@]}" -o "$HERE/$name" "$HERE/$name.cpp" "$@"; then
        echo "  СБОРКА НЕ ПРОШЛА"; FAILED=1; return
    fi
    "$HERE/$name" || FAILED=1
}

run_py() {    # имя, аргументы…
    local name="$1"; shift
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && [ "$ONLY" != "--accept" ] && return 0
    printf '\n\033[1m== %s ==\033[0m\n' "$name"
    python3 "$HERE/$name.py" "$@" || FAILED=1
}

run_cpp t_analytics "$MAIN/analytics.cpp"
run_cpp t_weather   "$HERE/stubs/https_stub.cpp"
run_cpp t_portal    "$MAIN/portal_art.cpp" "$MAIN/qrcodegen.c"
run_cpp t_sunmoon   "$MAIN/sun_moon.cpp"

# Вторым слоем — сверка со второй, независимой реализацией. Инварианты в t_sunmoon.cpp ловят грубое,
# но систематический сдвиг на пять минут проходит их все; это ловит только сравнение формулировок.
run_py t_sunmoon

# Расписание пробуждений: НАСТОЯЩИЙ bench_screens.cpp, собранный на этой машине с заглушками
# ESP-IDF под ним (tests/stubs/idf). Отдельная функция, потому что у этого набора свой порядок -I
# (собственный esp_http_client.h должен перекрыть общий), нужны исключения — выход из сна и
# перезагрузки сделан броском — и половина прошивки линкуется настоящей.
#
# Это единственный набор, который трогает bench_screens.cpp. До него файл был крупнейшим в проекте
# и непокрытым вовсе: сон, радио, I2C и панель заменить нечем, кроме прибора, — но РЕШЕНИЕ о том,
# что мерить, когда выходить в сеть и на сколько засыпать, отделимо от исполнения.
run_cycle() {
    [ -n "$ONLY" ] && [ "$ONLY" != "t_cycle" ] && [ "$ONLY" != "--accept" ] && return 0
    printf '\n\033[1m== t_cycle ==\033[0m\n'
    local IDF="$HERE/stubs/idf" BSEC="$HERE/../components/bsec2/include"
    local FLAGS=(-std=gnu++17 -O0 -g -fexceptions -fno-omit-frame-pointer
                 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
                 -I"$HERE" -I"$IDF" -I"$HERE/stubs" -I"$MAIN" -I"$AJSON" -I"$BSEC")
    # Си-исходники вендоров — отдельно: у них свой стандарт.
    local CO="$HERE/.cycle-obj"; mkdir -p "$CO"
    for c in qrcodegen bme69x; do
        cc -std=gnu11 -O0 -g -w -I"$IDF" -I"$MAIN" -c -o "$CO/$c.o" "$MAIN/$c.c" || {
            echo "  СБОРКА НЕ ПРОШЛА ($c.c)"; FAILED=1; return; }
    done
    # Настоящие исходники прошивки. Заглушены только три класса (fw_stub.cpp) и закрытый алгоритм
    # Bosch (bsec_stub.cpp) — причины расписаны в тех файлах.
    # Заголовки Bosch есть не везде: публичный репозиторий их не несёт (лицензия BSEC, §3.1). Тогда
    # набор собирается так же, как прошивка без них, — с bsec_manager_none.cpp, — и проверяет ту
    # сборку. Ни одна проверка t_cycle не опирается на выходы алгоритма.
    local GAS=(bsec_manager) GAS_STUB=("$IDF/bsec_stub.cpp")
    # Заглушка алгоритма (stubs/idf/bsec_stub.cpp) написана по заголовкам Bosch и в публичную
    # выгрузку тоже не идёт — без неё набор собирается так же, как без заголовков.
    if [ ! -f "$BSEC/bsec_interface.h" ] || [ ! -f "$IDF/bsec_stub.cpp" ]; then
        GAS=(bsec_manager_none); GAS_STUB=()
        echo "  (без Bosch BSEC: собирается bsec_manager_none.cpp)"
    fi
    local REAL=(nvs_manager analytics sun_moon hist_store evt_log screen epaper epd_vendor
                portal_screen portal_art scd41 bme690 max17048 battery_monitor battery_display power_estimate power_history battery_source curve_log curve_summary curve_screen i2c_manager
                clock_sync pm_gate "${GAS[@]}" telemetry weather telegram https_client wifi_slots
                ota_check ota_install captive_dns wifi_manager)
    local SRC=(); for f in "${REAL[@]}"; do SRC+=("$MAIN/$f.cpp"); done
    if ! c++ "${FLAGS[@]}" -o "$HERE/t_cycle" "$HERE/t_cycle.cpp" \
             "$IDF/host.cpp" "$IDF/fw_stub.cpp" ${GAS_STUB[@]+"${GAS_STUB[@]}"} \
             "${SRC[@]}" "$CO/qrcodegen.o" "$CO/bme69x.o"; then
        echo "  СБОРКА НЕ ПРОШЛА"; FAILED=1; return
    fi
    "$HERE/t_cycle" || FAILED=1
}
run_cycle
# Стенд разряда: образы журнала, записанные в t_cycle кодом прошивки, читает скрипт с компьютера.
run_py t_curve
if [ "$ONLY" = "--accept" ]; then run_py t_screen --accept; else run_py t_screen; fi
run_py t_console

# Браузер. Не все машины его имеют, поэтому набор пропускается с объяснением, а не с провалом:
# node ставится архивом в ~/.local/node, движок — командой ниже.
run_browser() {
    [ -n "$ONLY" ] && [ "$ONLY" != "t_browser" ] && [ "$ONLY" != "--accept" ] && return 0
    printf '\n\033[1m== t_browser ==\033[0m\n'
    local NODE
    NODE="$(command -v node || echo "$HOME/.local/node/bin/node")"
    if [ ! -x "$NODE" ]; then
        echo "  пропущено: node не найден (установить: см. tests/README.md)"; return 0
    fi
    if [ ! -d "$HERE/node_modules/playwright" ]; then
        echo "  пропущено: playwright не установлен ($NODE ... npm i playwright && npx playwright install chromium)"
        return 0
    fi
    PATH="$(dirname "$NODE"):$PATH" "$NODE" "$HERE/t_browser.js" || FAILED=1
}
run_browser

printf '\n'
if [ "$FAILED" = 0 ]; then printf '\033[32mвсё прошло\033[0m\n'; else printf '\033[31mесть провалы\033[0m\n'; fi
exit $FAILED
