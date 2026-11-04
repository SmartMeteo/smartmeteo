// The weather module's own parsing, checked by compiling weather.cpp INTO this file: the interesting
// work — MET's symbol table, the date parsers, the day summary and the icon a day gets — lives in
// static functions that nothing outside the file can call. Including the source is how a test
// reaches them without adding a seam to the firmware for the test's benefit.
#include "check.h"
#include "weather.cpp"

int main() {
    // ---- MET Norway's symbols (weathericons legend.csv) -> OWM icon codes ---------------------
    area("символы MET");
    char ic[4]; const char *txt = nullptr;
    ok(met_symbol("clearsky_day", ic, &txt), "clearsky_day распознан");
    eq_str(ic, "01d", "ясно днём");
    ok(met_symbol("clearsky_night", ic, &txt), "clearsky_night распознан");
    eq_str(ic, "01n", "ясно ночью — тот же код с n");
    ok(met_symbol("partlycloudy_polartwilight", ic, &txt), "полярные сумерки распознаны");
    eq_str(ic, "03d", "сумерки рисуются как день");
    ok(met_symbol("heavyrainandthunder", ic, &txt), "гроза с ливнем");
    eq_str(ic, "11d", "гроза");
    ok(!met_symbol("nosuchsymbol", ic, &txt), "неизвестный символ отвергается");
    ok(!met_symbol("", ic, &txt), "пустая строка отвергается");
    // Длинный мусор не должен переполнить внутренний буфер на 40 байт.
    ok(!met_symbol("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_day", ic, &txt),
       "слишком длинный идентификатор отвергается без чтения за буфером");

    // ---- Даты -------------------------------------------------------------------------------
    area("даты");
    eq_int(parse_iso_utc("2026-09-19T12:00:00Z"), 1789819200, "ISO разбирается");
    eq_int(parse_iso_utc("1970-01-01T00:00:00Z"), 0,          "эпоха");
    eq_int(parse_iso_utc(""),                      0,          "пустая строка — ноль");
    eq_int(parse_iso_utc("не дата"),               0,          "мусор — ноль");
    eq_int(parse_http_date("Sat, 19 Sep 2026 12:00:00 GMT"), 1789819200, "заголовок Expires");
    eq_int(parse_http_date("Sat, 19 Xyz 2026 12:00:00 GMT"), 0, "несуществующий месяц — ноль");
    eq_int(parse_http_date(""), 0, "пустой заголовок — ноль");

    area("календарь");
    eq_int(civil_days(1970, 1, 1), 0,      "эпоха — нулевой день");
    eq_int(civil_days(2026, 9, 19), 20715, "19.09.2026");
    // Високосный год: 29 февраля существует и идёт перед 1 марта.
    ok(civil_days(2024, 2, 29) + 1 == civil_days(2024, 3, 1), "29 февраля 2024 перед 1 марта");

    // ---- Иконка дня: гроза бьёт всё, дождь считается только при pop >= 0.5 --------------------
    area("иконка дня");
    {   DayTally t{}; char out[4];
        tally_step(t, "01d", 0.0f, 0, 12);
        tally_step(t, "11d", 0.0f, 0, 15);
        pick_icon(t, out); eq_str(out, "11d", "гроза в любом шаге побеждает");
    }
    {   DayTally t{}; char out[4];
        tally_step(t, "01d", 0.0f, 0, 9);
        tally_step(t, "10d", 0.3f, 0, 12);   // вероятность ниже порога — не считается дождём
        pick_icon(t, out); ok(strcmp(out, "10d") != 0, "дождь с pop 0,3 не объявляется");
    }
    {   DayTally t{}; char out[4];
        tally_step(t, "10d", 0.8f, 0, 12);
        pick_icon(t, out); eq_str(out, "10d", "дождь с pop 0,8 объявляется");
    }
    {   DayTally t{}; char out[4];
        tally_step(t, "13d", 0.9f, 0, 12);
        pick_icon(t, out); eq_str(out, "13d", "снег важнее дождя");
    }

    // ---- Сводка по дням из ответа OWM ---------------------------------------------------------
    area("сводка прогноза");
    {
        // Два дня по три шага; проверяется, что минимум и максимум берутся по main.temp шагов дня,
        // а не по temp_min/temp_max, и что «сегодня» в список будущих дней не попадает.
        static const char *json =
          "{\"list\":["
          "{\"dt\":1789894800,\"main\":{\"temp\":10.0,\"temp_min\":-99,\"temp_max\":99},"
            "\"weather\":[{\"icon\":\"01d\",\"id\":800}],\"clouds\":{\"all\":0},\"pop\":0,\"sys\":{\"pod\":\"d\"}},"
          "{\"dt\":1789905600,\"main\":{\"temp\":18.0,\"temp_min\":-99,\"temp_max\":99},"
            "\"weather\":[{\"icon\":\"01d\",\"id\":800}],\"clouds\":{\"all\":0},\"pop\":0,\"sys\":{\"pod\":\"d\"}},"
          "{\"dt\":1789916400,\"main\":{\"temp\":4.0,\"temp_min\":-99,\"temp_max\":99},"
            "\"weather\":[{\"icon\":\"01d\",\"id\":800}],\"clouds\":{\"all\":0},\"pop\":0,\"sys\":{\"pod\":\"d\"}}"
          "]}";
        JsonDocument doc;
        ok(!deserializeJson(doc, json), "тестовый ответ разобран");
        float tmin = 999, tmax = -999; WxDay days[4]; int n = 0;
        const int32_t today = 20715;   // 19.09.2026; шаги выше — следующие сутки
        summarise_forecast(doc["list"].as<JsonArrayConst>(), 0, today, tmin, tmax, days, 4, n);
        ok(n >= 1, "хотя бы один день собран");
        if (n >= 1) {
            near(days[0].lo, 4.0, 0.01, "минимум дня — по main.temp самого холодного шага");
            near(days[0].hi, 18.0, 0.01, "максимум дня — по main.temp, а не temp_max");
        }
    }

    // ---- Экранирование запроса ----------------------------------------------------------------
    area("запрос");
    eq_str(urlEncode("Darmstadt").c_str(), "Darmstadt", "простое имя без изменений");
    ok(urlEncode("Nizhniy Novgorod").find(' ') == std::string::npos, "пробел закодирован");
    ok(urlEncode("Мюнхен").find("%D") != std::string::npos, "кириллица закодирована");


    // ---- MET Norway целиком: настоящий разбор настоящего Https::Reader -------------------------
    // Тело ответа подставляет HttpStub, всё выше него — код прибора без единой правки. День строится
    // от текущего времени, потому что met() берёт «сегодня» из time(NULL): проверяется поведение,
    // а не календарь того дня, когда тест написали.
    area("MET Norway");
    {
        const time_t nw  = time(NULL);
        const long long d0 = (long long)nw / 86400;

        auto iso = [](long long t) {
            time_t tt = (time_t)t; struct tm g; gmtime_r(&tt, &g);
            char b[32]; strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &g); return std::string(b);
        };
        auto entry = [&](long long t, float temp, const char *s1, const char *s12,
                         float wmin, float wmax) {
            std::string e = "{\"time\":\"" + iso(t) + "\",\"data\":{\"instant\":{\"details\":{";
            char b[256];
            // The extra cells of the bottom strip ride in the SAME instant block MET already sends
            // for temperature. Cloud is deliberately 0: a real reading that a sentinel of -1 must be
            // able to be told apart from, and the first cut of this used 0 for "absent".
            snprintf(b, sizeof(b), "\"air_temperature\":%.1f,\"apparent_air_temperature\":%.1f,"
                     "\"relative_humidity\":%.1f,\"wind_speed\":%.1f,"
                     "\"ultraviolet_index_clear_sky\":%.1f,\"dew_point_temperature\":%.1f,"
                     "\"cloud_area_fraction\":%.1f}}", temp, temp - 1.3f, 50.0f, 4.9f, 6.4f, 7.2f, 0.0f);
            e += b;
            // probability_of_precipitation sits in next_1_hours, not in instant — and the last entry
            // of a MET range carries no next_1_hours block at all, which is exactly why the field
            // keeps a sentinel instead of defaulting to zero.
            if (s1 && *s1) {
                e += ",\"next_1_hours\":{\"details\":{\"probability_of_precipitation\":35.0},"
                     "\"summary\":{\"symbol_code\":\""; e += s1; e += "\"}}";
            }
            if (wmax > -900) {
                snprintf(b, sizeof(b), ",\"next_6_hours\":{\"summary\":{\"symbol_code\":\"cloudy\"},"
                         "\"details\":{\"air_temperature_min\":%.1f,\"air_temperature_max\":%.1f}}", wmin, wmax);
                e += b;
            }
            if (s12 && *s12) { e += ",\"next_12_hours\":{\"summary\":{\"symbol_code\":\""; e += s12; e += "\"}}"; }
            e += "}}";
            return e;
        };

        std::string body = "{\"type\":\"Feature\",\"properties\":{\"meta\":{\"units\":{}},\"timeseries\":[";
        body += entry(nw, 12.3f, "clearsky_day", nullptr, -999, -999);          // сейчас
        const long long t1 = (d0 + 1) * 86400, t2 = (d0 + 2) * 86400;
        body += "," + entry(t1 +  0*3600,  8.0f, "partlycloudy_night", nullptr,  7.0f, 11.0f);
        body += "," + entry(t1 +  6*3600, 10.0f, "cloudy",             "rain",  10.0f, 16.0f);
        body += "," + entry(t1 + 12*3600, 16.0f, "rain",               nullptr, 12.0f, 17.0f);
        body += "," + entry(t1 + 18*3600, 13.0f, "cloudy",             nullptr,  9.0f, 13.0f);
        body += "," + entry(t2 +  0*3600,  5.0f, "snow",               nullptr, -999, -999);
        body += "," + entry(t2 +  6*3600,  6.0f, "snow",               nullptr, -999, -999);
        body += "," + entry(t2 + 12*3600,  9.0f, "snow",               nullptr, -999, -999);
        body += "," + entry(t2 + 18*3600,  4.0f, "cloudy",             nullptr, -999, -999);
        body += "]}}";

        auto run = [&](size_t chunk, WxNow &now, float &tmin, float &tmax,
                       WxDay *days, int &n, time_t &exp, int &st) {
            HttpStub::reset();
            HttpStub::body    = body;
            HttpStub::expires = "Sat, 19 Sep 2026 12:00:00 GMT";
            HttpStub::chunk   = chunk;
            tmin = 999; tmax = -999; n = 0;
            return Weather::met(49.8728f, 8.6512f, 0, now, tmin, tmax, days, 4, n, exp, st);
        };

        WxNow now; float tmin, tmax; WxDay days[4]; int n; time_t exp; int st;
        ok(run(0, now, tmin, tmax, days, n, exp, st), "ответ разобран");
        eq_int(st, 200, "статус 200");
        eq_int((long long)exp, 1789819200LL, "Expires из заголовка");
        near(now.temp, 12.3, 0.05, "температура сейчас");
        near(now.feels, 11.0, 0.05, "ощущается — из apparent_air_temperature");
        eq_int(now.hum, 50, "влажность");
        near(now.wind, 4.9, 0.05, "ветер");
        // Новые ячейки нижней полосы. Читаются из того же ответа, что и прогноз, — эфира не стоят.
        near(now.uv, 6.4, 0.05, "УФ из ultraviolet_index_clear_sky");
        near(now.dew, 7.2, 0.05, "точка росы");
        eq_int(now.cloud, 0, "облачность 0 % — настоящее показание, а не признак отсутствия");
        // Вероятность осадков в подставном ответе есть, а прошивка её больше не просит и не
        // разбирает (снята 2026-09-28: MET даёт её только по Скандинавии). Лишнее поле в чужом
        // ответе должно просто проходить мимо, а не ломать разбор.
        eq_str(now.icon, "01d", "иконка из next_1_hours");
        eq_str(now.desc.c_str(), "Clear sky", "описание из таблицы MET");
        near(tmin, 12.3, 0.05, "сегодня: только запись текущего часа");
        near(tmax, 12.3, 0.05, "сегодня: максимум оттуда же");
        eq_int(n, 2, "два дня вперёд");
        if (n == 2) {
            eq_int(days[0].day, (long long)(d0 + 1), "первый день — завтра");
            near(days[0].lo, 7.0,  0.05, "минимум завтра — из шестичасового окна");
            near(days[0].hi, 17.0, 0.05, "максимум завтра — из шестичасового окна");
            eq_str(days[0].icon, "10d", "иконка завтра — next_12_hours ближе всего к 06:00");
            near(days[1].lo, 4.0, 0.05, "минимум послезавтра — по мгновенным");
            near(days[1].hi, 9.0, 0.05, "максимум послезавтра — по мгновенным");
            eq_str(days[1].icon, "13d", "иконка послезавтра — подсчётом дневных символов");
        }

        // То же тело, выданное по 7 байт: потоковый разбор не должен зависеть от границ чтения.
        WxNow n2; float a2, b2; WxDay d2[4]; int c2; time_t e2; int s2;
        ok(run(7, n2, a2, b2, d2, c2, e2, s2), "тот же ответ по 7 байт разобран");
        ok(HttpStub::reads > 50, "тело действительно пришло кусками");
        eq_int(c2, n, "столько же дней");
        near(n2.temp, now.temp, 0.001, "та же температура");
        if (c2 == 2 && n == 2) {
            near(d2[0].hi, days[0].hi, 0.001, "тот же максимум");
            eq_str(d2[1].icon, days[1].icon, "та же иконка");
        }

        // Отказ сервера.
        HttpStub::reset(); HttpStub::body = body; HttpStub::status = 429;
        WxNow n3; float a3 = 999, b3 = -999; WxDay d3[4]; int c3 = 0; time_t e3; int s3;
        ok(!Weather::met(49.8f, 8.6f, 0, n3, a3, b3, d3, 4, c3, e3, s3), "HTTP 429 — отказ");
        eq_int(s3, 429, "статус сохранён для журнала");
        eq_int(c3, 0, "дней не собрано");

        // Оборванное тело: то, что успело прийти, остаётся пригодным.
        HttpStub::reset(); HttpStub::body = body.substr(0, body.size() / 2); HttpStub::chunk = 13;
        WxNow n4; float a4 = 999, b4 = -999; WxDay d4[4]; int c4 = 0; time_t e4; int s4;
        const bool cut = Weather::met(49.8f, 8.6f, 0, n4, a4, b4, d4, 4, c4, e4, s4);
        ok(cut, "обрыв на середине не теряет уже прочитанное");
        near(n4.temp, 12.3, 0.05, "текущая температура уцелела");

        // Мусор вместо JSON.
        HttpStub::reset(); HttpStub::body = "not json at all";
        WxNow n5; float a5 = 999, b5 = -999; WxDay d5[4]; int c5 = 0; time_t e5; int s5;
        ok(!Weather::met(49.8f, 8.6f, 0, n5, a5, b5, d5, 4, c5, e5, s5), "не-JSON отвергается");

        // Пустой массив: ответ разобран, но брать нечего.
        HttpStub::reset();
        HttpStub::body = "{\"properties\":{\"timeseries\":[]}}";
        WxNow n6; float a6 = 999, b6 = -999; WxDay d6[4]; int c6 = 0; time_t e6; int s6;
        ok(!Weather::met(49.8f, 8.6f, 0, n6, a6, b6, d6, 4, c6, e6, s6), "пустой timeseries — отказ");
        eq_int(c6, 0, "дней нет");
    }

    return report("погода");
}
