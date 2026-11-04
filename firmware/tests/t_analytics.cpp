// The air verdict, checked against the documents it came from — Bosch's BME690 datasheet table 6
// for the bands, the Met Office tendency terms for the barometer, and the cascade the owner
// approved on 2026-09-14. The REAL analytics.cpp is compiled in; nothing here is a copy of it.
#include "check.h"
#include "analytics.h"

int main() {
    Analytics &A = Analytics::getInstance();

    // ---- Bosch's bands, on the ROUNDED index (the index has a resolution of 1, table 3) --------
    area("полосы Bosch");
    eq_int(Analytics::iaqBand(0),     IAQ_EXCELLENT,   "0 — Excellent");
    eq_int(Analytics::iaqBand(50),    IAQ_EXCELLENT,   "50 — верхняя граница Excellent");
    eq_int(Analytics::iaqBand(51),    IAQ_GOOD,        "51 — Good");
    eq_int(Analytics::iaqBand(100),   IAQ_GOOD,        "100 — верхняя граница Good");
    eq_int(Analytics::iaqBand(101),   IAQ_LIGHTLY,     "101 — Lightly");
    eq_int(Analytics::iaqBand(150),   IAQ_LIGHTLY,     "150 — верхняя граница Lightly");
    eq_int(Analytics::iaqBand(151),   IAQ_MODERATELY,  "151 — Moderately");
    eq_int(Analytics::iaqBand(200),   IAQ_MODERATELY,  "200 — верхняя граница Moderately");
    eq_int(Analytics::iaqBand(201),   IAQ_HEAVILY,     "201 — Heavily");
    eq_int(Analytics::iaqBand(250),   IAQ_HEAVILY,     "250 — верхняя граница Heavily");
    eq_int(Analytics::iaqBand(251),   IAQ_SEVERELY,    "251 — Severely");
    eq_int(Analytics::iaqBand(350),   IAQ_SEVERELY,    "350 — верхняя граница Severely");
    eq_int(Analytics::iaqBand(351),   IAQ_EXTREMELY,   "351 — Extremely");
    eq_int(Analytics::iaqBand(500),   IAQ_EXTREMELY,   "500 — Extremely");
    // Округление, а не отбрасывание: 150,4 это ещё 150.
    eq_int(Analytics::iaqBand(150.4f), IAQ_LIGHTLY,    "150,4 округляется вниз");
    eq_int(Analytics::iaqBand(150.5f), IAQ_MODERATELY, "150,5 округляется вверх");

    area("слова");
    eq_str(Analytics::iaqBandName(IAQ_EXCELLENT),  "Excellent",           "полное имя");
    eq_str(Analytics::iaqBandName(IAQ_EXTREMELY),  "Extremely polluted",  "полное имя худшей");
    eq_str(Analytics::iaqBandShort(IAQ_EXCELLENT), "Pure",                "короткое имя");
    eq_str(Analytics::iaqBandShort(IAQ_MODERATELY),"Moderate",            "короткое имя");
    // Блок Air Quality — 107 px, glcd размера 2 это 12 px на букву: больше восьми не поместится.
    for (int b = IAQ_EXCELLENT; b <= IAQ_EXTREMELY; b++)
        ok(strlen(Analytics::iaqBandShort((IaqBand)b)) <= 8, "короткое имя не длиннее восьми букв");
    // Выход за диапазон не должен читать за концом таблицы.
    eq_str(Analytics::iaqBandName((IaqBand)99), "Extremely polluted", "мусорный класс зажимается");

    // ---- Барометр: термины Met Office на шаге 0,1 гПа ------------------------------------------
    // Все проверки скорости идут под уровнем НИЖЕ нормы: с 2026-10-05 у плашки два входа, и без
    // низкого уровня она молчит при любой скорости — это проверяется отдельной областью ниже.
    const float LOW = 1005.0f;
    area("барометр");
    eq_int(Analytics::baroMode(0.0f, LOW),   0, "без изменения — число");
    eq_int(Analytics::baroMode(-1.5f, LOW),  0, "-1,5 ещё не falling");
    eq_int(Analytics::baroMode(-1.6f, LOW),  1, "-1,6 — falling, Rain");
    eq_int(Analytics::baroMode(-3.5f, LOW),  1, "-3,5 всё ещё Rain");
    eq_int(Analytics::baroMode(-3.6f, LOW),  2, "-3,6 — falling quickly, Storm");
    eq_int(Analytics::baroMode(-9.0f, LOW),  2, "сильное падение — Storm");
    eq_int(Analytics::baroMode(+5.0f, LOW),  0, "рост — число");
    // Округление до десятой: -1,55 это -1,6 и уже Rain.
    eq_int(Analytics::baroMode(-1.55f, LOW), 1, "-1,55 округляется до -1,6");

    // ---- Барометр: гистерезис на СНЯТИЕ --------------------------------------------------------
    // Входить по-прежнему строго по границе Met Office — все восемь проверок выше идут с prev = 0 и
    // этим же и заняты. Держится только то, что уже на стекле, и только на 0,4 гПа дольше.
    eq_int(Analytics::baroMode(-1.4f, LOW, 0), 0, "без прежнего режима -1,4 не включает Rain");
    eq_int(Analytics::baroMode(-1.4f, LOW, 1), 1, "а уже показанный Rain на -1,4 держится");
    eq_int(Analytics::baroMode(-1.2f, LOW, 1), 1, "на самой границе снятия ещё держится");
    eq_int(Analytics::baroMode(-1.1f, LOW, 1), 0, "за ней снимается");
    eq_int(Analytics::baroMode(+2.0f, LOW, 1), 0, "рост снимает сразу");
    eq_int(Analytics::baroMode(-3.3f, LOW, 2), 2, "показанный Storm на -3,3 держится");
    eq_int(Analytics::baroMode(-3.1f, LOW, 2), 1, "ослаб ниже своей границы — опускается до Rain");
    eq_int(Analytics::baroMode(-3.3f, LOW, 1), 1, "а Rain на -3,3 в Storm не поднимается");
    eq_int(Analytics::baroMode(-1.1f, LOW, 2), 0, "Storm тоже снимается на общей границе");
    // Запас обязан перекрывать скачок опорной записи — падение/12 при шаге истории 15 мин, то есть
    // 0,3 гПа у самой границы Storm. Иначе гистерезис есть, а дребезг остаётся.
    ok(Analytics::BARO_RAIN_CLEAR  - Analytics::BARO_RAIN_TRIP  >= 0.3f, "запас Rain перекрывает скачок опоры");
    ok(Analytics::BARO_STORM_CLEAR - Analytics::BARO_STORM_TRIP >= 0.3f, "запас Storm перекрывает скачок опоры");
    // И снятие обязано быть СЛАБЕЕ входа, иначе это не гистерезис, а сдвинутый порог.
    ok(Analytics::BARO_RAIN_CLEAR  > Analytics::BARO_RAIN_TRIP, "снятие Rain слабее входа");
    ok(Analytics::BARO_STORM_CLEAR > Analytics::BARO_STORM_TRIP, "снятие Storm слабее входа");

    // ---- Барометр: УРОВЕНЬ, а не только скорость -----------------------------------------------
    // Второй вход появился 2026-10-05 и по измерению, а не по рассуждению: барометр упал на 3,9 гПа
    // за семь часов, плашка сказала Rain над солнечным днём, а MET для тех же координат дал ясно и
    // 0,0 мм на 44 часа вперёд — дождь пришёл через двое суток, когда давление опустилось к 1008.
    // Та же скорость, противоположный смысл; различает их только уровень.
    area("барометр: уровень");
    eq_int(Analytics::baroMode(-1.7f, 1023.2f), 0, "5 октября: падение при 1023 — не прогноз");
    eq_int(Analytics::baroMode(-9.0f, 1030.0f), 0, "и обвал в антициклоне тоже молчит");
    eq_int(Analytics::baroMode(-2.2f, 1009.3f), 1, "7 октября: та же скорость ниже нормы — Rain");
    eq_int(Analytics::baroMode(-4.0f, 1000.0f), 2, "быстрое падение ниже нормы — Storm");
    // Границу проверяем числом: сдвинь стандартную атмосферу молча, и это упадёт.
    eq_int(Analytics::baroMode(-2.0f, 1013.2f), 1, "на самой границе 1013,25 прогноз ещё есть");
    eq_int(Analytics::baroMode(-2.0f, 1013.3f), 0, "на волос выше — уже нет");
    // Гистерезис по уровню: множитель приведения верен лишь до пары десятых гПа.
    eq_int(Analytics::baroMode(-2.0f, 1014.2f, 1), 1, "показанный Rain держится до 1014,25");
    eq_int(Analytics::baroMode(-2.0f, 1014.3f, 1), 0, "за ней снимается даже при падении");
    ok(Analytics::BARO_LEVEL_CLEAR > Analytics::BARO_LEVEL_TRIP, "снятие по уровню слабее входа");
    // Неизвестный уровень — прибор ни разу не видел погоды. Тогда слова нет вовсе: обещание, за
    // которое нечем ответить, хуже числа.
    eq_int(Analytics::baroMode(-9.0f, NAN),    0, "без уровня даже обвал — только число");
    eq_int(Analytics::baroMode(-2.0f, NAN, 1), 0, "и прежний режим его не воскрешает");
    // Низкий уровень сам по себе плашку не поднимает — нужна и скорость.
    eq_int(Analytics::baroMode(0.0f,  980.0f), 0, "низкое давление без падения — число");
    eq_int(Analytics::baroMode(-1.5f, 980.0f), 0, "и падение слабее границы Met Office — тоже");

    // ---- Точка росы ----------------------------------------------------------------------------
    area("точка росы");
    near(A.calculateDewPoint(20.0f, 50.0f), 9.3,  0.3, "20 °C / 50 % ≈ 9,3");
    near(A.calculateDewPoint(25.0f, 100.0f), 25.0, 0.3, "при 100 % равна температуре");
    ok(std::isfinite(A.calculateDewPoint(20.0f, 0.0f)), "нулевая влажность не даёт бесконечность");

    // ---- Каскад: первое совпадение побеждает ---------------------------------------------------
    area("каскад");
    AlertConfig cfg; cfg.co2_level = 1100; cfg.co2_en = true; cfg.voc_en = true; cfg.hum_en = true;
    {   // Точность 0 — газ ещё не верен, что бы он ни показывал.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 300.0f, 22.0f, 45.0f, true, 0, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "точность 0 — класс 0");
        eq_str(txt.c_str(), "Warming up", "точность 0 — «Warming up»");
    }
    {   // Сильное загрязнение перевешивает всё остальное, но ему нужны два тика подряд.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 260.0f, 22.0f, 45.0f, true, 3, NAN, cfg, st, cls, txt, air);
        A.classifyAir(600, 260.0f, 22.0f, 45.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 3, "IAQ 260 на втором тике — класс 3");
    }
    {   // CO2 выше уровня владельца — Stuffy, и держится в гистерезисе до -150.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(1200, 40.0f, 22.0f, 45.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 1, "1200 ppm при пороге 1100 — класс 1");
        A.classifyAir(1000, 40.0f, 22.0f, 45.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 1, "1000 ppm — всё ещё в гистерезисе");
        A.classifyAir(940, 40.0f, 22.0f, 45.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "940 ppm — ниже порога снятия, чисто");
    }
    {   // Сырость требует двух циклов подряд и снимается ниже 62 %.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 40.0f, 22.0f, 75.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "сырость не объявляется на первом замере");
        A.classifyAir(600, 40.0f, 22.0f, 75.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "75 % на втором цикле — класс 5");
        A.classifyAir(600, 40.0f, 22.0f, 60.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "60 % — ниже снятия");
    }

    // ---- Сырость считает ЗАМЕРЫ, а не вызовы ---------------------------------------------------
    // Дефект, живший до 2026-10-02. classifyAir зовётся на газовом такте (300 с), а влажность
    // читается по интервалу владельца — 15 мин в профиле по умолчанию. На тиках между замерами
    // measure() подаёт прежнее значение из RTC, и счётчик рос на одном и том же числе: один выброс
    // за порог объявлял сырость через пять минут сам по себе. Хуже того, alertPending() ссылается
    // на эту самую «устойчивость за два цикла» как на причину не требовать второго подтверждения,
    // так что единственный замер уходил владельцу как WARNING — в Telegram и с подъёмом радио
    // ночью. Проверки ниже написаны так, чтобы упасть ровно на этом.
    area("сырость считает замеры");
    {   // Свежий замер за порогом, затем повторная подача того же числа: ничего не узнали.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 40.0f, 21.0f, 71.0f, true,  3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "один свежий замер 71 % — ещё не сырость");
        A.classifyAir(600, 40.0f, 21.0f, 71.0f, false, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "повтор того же замера сырость НЕ объявляет");
        for (int i = 0; i < 5; i++)
            A.classifyAir(600, 40.0f, 21.0f, 71.0f, false, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "и пять повторов подряд тоже");
        A.classifyAir(600, 40.0f, 21.0f, 71.0f, true,  3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "второй настоящий замер — вот теперь сырость");
    }
    {   // Каденция профиля по умолчанию целиком: газ раз в 5 мин, CO2 раз в 15, один выброс.
        AlertState st; int cls = -1, air = -1; std::string txt;
        bool ever = false;
        float rtc_hum = 55.0f;
        for (int min = 0; min <= 40; min += 5) {
            const bool co2_due = (min % 15) == 0;
            if (co2_due) rtc_hum = (min == 0) ? 71.0f : 55.0f;
            A.classifyAir(600, 40.0f, 21.0f, rtc_hum, co2_due, 3, NAN, cfg, st, cls, txt, air);
            if (cls == 5) ever = true;
        }
        ok(!ever, "один выброс в каденции Optimal сырость не объявляет ни разу");
    }
    {   // Снятие по-прежнему требует настоящего замера: повтор не гасит защёлку.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 40.0f, 21.0f, 72.0f, true,  3, NAN, cfg, st, cls, txt, air);
        A.classifyAir(600, 40.0f, 21.0f, 72.0f, true,  3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "два замера 72 % — сырость");
        A.classifyAir(600, 40.0f, 21.0f, 72.0f, false, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "повтор сырость не снимает (здесь её держит счётчик)");
        A.classifyAir(600, 40.0f, 21.0f, 65.0f, true,  3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "65 % — выше снятия, защёлка держит");
        // И вот здесь проверяется ИМЕННО защёлка: свежие 65 % обнулили счётчик, так что соседний
        // механизм результат больше не подпирает. Без этой строки слом «защёлку гасит повтор»
        // проходил молча — счётчик всё ещё стоял на двух и отвечал за сырость вместо защёлки.
        A.classifyAir(600, 40.0f, 21.0f, 65.0f, false, 3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "повтор при обнулённом счётчике защёлку тоже не гасит");
        A.classifyAir(600, 40.0f, 21.0f, 61.0f, true,  3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 0, "61 % — ниже снятия, чисто");
    }
    {   // Сами границы, числом. Экран округляет влажность до целого, поэтому «70 %» на стекле это
        // что угодно от 69,5 до 70,5 — и по какую сторону порога оно лежит, видно только здесь.
        // Порог строгий: ровно 70 % НЕ сырость, иначе консоль («above 70 %») обещала бы не то.
        auto cls_after_two = [&](float hum) {
            AlertState st; int cls = -1, air = -1; std::string txt;
            A.classifyAir(600, 40.0f, 21.0f, hum, true, 3, NAN, cfg, st, cls, txt, air);
            A.classifyAir(600, 40.0f, 21.0f, hum, true, 3, NAN, cfg, st, cls, txt, air);
            return cls;
        };
        eq_int(cls_after_two(70.0f), 0, "ровно 70 % — ещё не сырость, порог строгий");
        eq_int(cls_after_two(70.1f), 5, "70,1 % — уже сырость");
        eq_int(cls_after_two(69.9f), 0, "69,9 % — не сырость");
        // Ветки точки росы больше нет (2026-10-02). Она включалась только выше 23,8 °C в комнате,
        // то есть летом, и для плесени это ровно наоборот. Без уличной температуры остаётся один
        // комнатный порог, и 66 % его не переходят ни при какой температуре.
        for (float T : {21.0f, 24.0f, 25.0f, 28.0f}) {
            AlertState w; int wc = -1, air = -1; std::string wt;
            A.classifyAir(600, 40.0f, T, 66.0f, true, 3, NAN, cfg, w, wc, wt, air);
            A.classifyAir(600, 40.0f, T, 66.0f, true, 3, NAN, cfg, w, wc, wt, air);
            eq_int(wc, 0, "66 % без улицы — не сырость ни при какой температуре комнаты");
        }
        // Снятие — тоже строгое сравнение, и его граница 62.
        AlertState l; int lc = -1, air = -1; std::string lt;
        A.classifyAir(600, 40.0f, 21.0f, 75.0f, true, 3, NAN, cfg, l, lc, lt, air);
        A.classifyAir(600, 40.0f, 21.0f, 75.0f, true, 3, NAN, cfg, l, lc, lt, air);
        A.classifyAir(600, 40.0f, 21.0f, 62.1f, true, 3, NAN, cfg, l, lc, lt, air);
        eq_int(lc, 5, "62,1 % — защёлка ещё держит");
        A.classifyAir(600, 40.0f, 21.0f, 62.0f, true, 3, NAN, cfg, l, lc, lt, air);
        eq_int(lc, 0, "ровно 62 % — снято");
    }

    {   // Счётчик не сбрасывается повтором: он просто стоит на месте.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 40.0f, 21.0f, 71.0f, true,  3, NAN, cfg, st, cls, txt, air);
        for (int i = 0; i < 3; i++)
            A.classifyAir(600, 40.0f, 21.0f, 71.0f, false, 3, NAN, cfg, st, cls, txt, air);
        A.classifyAir(600, 40.0f, 21.0f, 71.0f, true,  3, NAN, cfg, st, cls, txt, air);
        eq_int(cls, 5, "повторы между двумя замерами счёт не теряют");
    }

    // ---- Сырость ушла из вердикта о воздухе, но НЕ из тревог -----------------------------------
    // Решение владельца 2026-10-02: «Damp» переехало в блок влажности, рядом с числом, из которого
    // оно и выведено. Значит каскад отдаёт два ответа: airClass — про воздух, им живёт блок Air
    // Quality; pollutionClass — общий, им живут Telegram, MQTT, /api/status и автомат тревог.
    // Самое важное здесь — что ВТОРОЙ не сдвинулся ни на класс, иначе переезд на стекле тихо
    // переписал бы то, что приходит владельцу в телефон.
    area("воздух отдельно, сырость отдельно");
    {
        struct Case { const char *what; float hum; uint16_t co2; float iaq; int cls; int air; };
        // Ровно та очерёдность, что была до правки: 3 выше сырости, сырость выше 1 и 2.
        const Case cases[] = {
            { "чисто",                      45,  600,  40, 0, 0 },
            { "только духота",              45, 1200,  40, 1, 1 },
            { "только газ 2",               45,  600, 180, 2, 2 },
            { "только газ 3",               45,  600, 260, 3, 3 },
            { "только сырость",             75,  600,  40, 5, 0 },
            { "сырость и духота",           75, 1200,  40, 5, 1 },
            { "сырость и газ 2",            75,  600, 180, 5, 2 },
            { "сырость и газ 3 — газ выше", 75,  600, 260, 3, 3 },
        };
        for (const auto &c : cases) {
            AlertState st; int cls = -1, air = -1; std::string txt;
            A.classifyAir(c.co2, c.iaq, 21.0f, c.hum, true, 3, NAN, cfg, st, cls, txt, air);
            A.classifyAir(c.co2, c.iaq, 21.0f, c.hum, true, 3, NAN, cfg, st, cls, txt, air);
            eq_int(cls, c.cls, (std::string("общий вердикт не изменился: ") + c.what).c_str());
            eq_int(air, c.air, (std::string("воздух без сырости: ") + c.what).c_str());
            ok(air != 5, "вердикт о воздухе никогда не равен 5");
        }
    }
    {   // И слово: при сырости общий текст — «Damp», это то, что уходит в Telegram и MQTT.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(1200, 40.0f, 21.0f, 75.0f, true, 3, NAN, cfg, st, cls, txt, air);
        A.classifyAir(1200, 40.0f, 21.0f, 75.0f, true, 3, NAN, cfg, st, cls, txt, air);
        eq_str(txt.c_str(), "Damp", "сырость побеждает духоту в ОБЩЕМ тексте");
        eq_int(air, 1, "а воздух при этом по-прежнему «душно» — экран его покажет");
        eq_int(Analytics::alertTier(cls, 1200), TIER_WARNING, "уровень тревоги прежний");
    }

    // ---- Сырость судится по СТЕНЕ, а не по комнате ---------------------------------------------
    // Решение владельца 2026-10-02. Плесень растёт на поверхности, а не в воздухе, и поверхность
    // холоднее комнаты ровно настолько, насколько холодно снаружи. Числа ниже посчитаны формулой
    // EN ISO 13788 отдельно, в Python, а не сняты с этого же кода — иначе проверка подтверждала бы
    // саму себя. f = 0,70, минимум DIN 4108-2 для нового строительства.
    area("сырость: стена, а не комната");
    {
        // T_surface = T_out + 0,70 * (T_in - T_out); поверхностная = e(комнаты) / es(поверхности).
        struct S { float tin, rh, tout, want; };
        const S s[] = {
            { 21,  70,  0, 100.0f },   // январь: расчёт даёт 104 — это конденсат, обрезается до 100
            { 21,  70, 12,  82.8f },   // октябрь, как сейчас у владельца
            { 21,  70, 20,  71.3f },   // июль: та же влажность, риска нет
            { 21,  50,  0,  74.3f },
            { 19,  68,  0,  97.8f },
            { 25,  70, 20,  76.6f },
        };
        for (const auto &c : s) {
            const float got = Analytics::surfaceRH(c.tin, c.rh, c.tout);
            near(got, c.want, 0.6, "поверхностная влажность совпала с расчётом по стандарту");
        }
        ok(!std::isfinite(Analytics::surfaceRH(21, 70, NAN)), "без улицы — NAN, а не выдуманное число");
        ok(Analytics::surfaceRH(21, 70, 25) <= 100.0f, "жара снаружи не делает стену теплее комнаты");
        near(Analytics::surfaceRH(21, 70, 25), 70.0, 0.1, "и тогда поверхностная равна комнатной");
        ok(Analytics::surfaceRH(21, 70, 0) <= 100.0f, "выше насыщения значение обрезано сотней");
    }
    {   // И сам вердикт: ОДНА И ТА ЖЕ комнатная влажность, три сезона, три ответа.
        struct C { const char *when; float tout; float hum; int want; };
        const C cases[] = {
            { "январь, 70 % — конденсат",      0, 70, 5 },
            { "январь, 55 % — уже за порогом", 0, 55, 5 },
            { "январь, 45 % — чисто",          0, 45, 0 },
            { "октябрь, 70 % — риск",         12, 70, 5 },
            { "октябрь, 60 % — чисто",        12, 60, 0 },
            { "июль, 70 % — безвредно",       20, 70, 0 },
            { "июль, 85 % — вот теперь да",   20, 85, 5 },
        };
        for (const auto &c : cases) {
            AlertState st; int cls = -1, air = -1; std::string txt;
            A.classifyAir(600, 40.0f, 21.0f, c.hum, true, 3, c.tout, cfg, st, cls, txt, air);
            A.classifyAir(600, 40.0f, 21.0f, c.hum, true, 3, c.tout, cfg, st, cls, txt, air);
            eq_int(cls, c.want, c.when);
        }
    }
    {   // Прежнее поведение было перевёрнутым, и это стоит закрепить явно: 68 % зимой — молчало,
        // 70 % летом — кричало. Теперь ровно наоборот, и это и есть смысл правки.
        AlertState w; int wc = -1, air = -1; std::string wt;
        A.classifyAir(600, 40.0f, 19.0f, 68.0f, true, 3, 0.0f, cfg, w, wc, wt, air);
        A.classifyAir(600, 40.0f, 19.0f, 68.0f, true, 3, 0.0f, cfg, w, wc, wt, air);
        eq_int(wc, 5, "19 °C и 68 % в мороз — сырость (раньше прибор молчал)");
        AlertState z; int zc = -1, air2 = -1; std::string zt;
        A.classifyAir(600, 40.0f, 25.0f, 70.0f, true, 3, 20.0f, cfg, z, zc, zt, air2);
        A.classifyAir(600, 40.0f, 25.0f, 70.0f, true, 3, 20.0f, cfg, z, zc, zt, air2);
        eq_int(zc, 0, "25 °C и 70 % в жару — тишина (раньше слал предупреждение)");
    }
    {   // Гистерезис по поверхности: порог 80, снятие 70 — ниже 70 плесень не растёт вовсе.
        AlertState st; int cls = -1, air = -1; std::string txt;
        A.classifyAir(600, 40.0f, 21.0f, 70.0f, true, 3, 12.0f, cfg, st, cls, txt, air);
        A.classifyAir(600, 40.0f, 21.0f, 70.0f, true, 3, 12.0f, cfg, st, cls, txt, air);
        eq_int(cls, 5, "82,8 % на стене — сырость");
        A.classifyAir(600, 40.0f, 21.0f, 64.0f, true, 3, 12.0f, cfg, st, cls, txt, air);
        eq_int(cls, 5, "75,7 % — между порогами, защёлка держит");
        A.classifyAir(600, 40.0f, 21.0f, 58.0f, true, 3, 12.0f, cfg, st, cls, txt, air);
        eq_int(cls, 0, "68,6 % — ниже снятия, чисто");
    }

    // ---- Уровни тревоги ------------------------------------------------------------------------
    area("уровни");
    eq_int(Analytics::alertTier(3, 600),  TIER_WARNING,  "класс 3 — WARNING");
    eq_int(Analytics::alertTier(5, 600),  TIER_WARNING,  "класс 5 — WARNING");
    eq_int(Analytics::alertTier(2, 600),  TIER_ADVISORY, "класс 2 — ADVISORY");
    eq_int(Analytics::alertTier(1, 1200), TIER_ADVISORY, "класс 1 ниже 1500 — ADVISORY");
    eq_int(Analytics::alertTier(1, 1500), TIER_WARNING,  "класс 1 от 1500 — WARNING");
    eq_int(Analytics::alertTier(0, 600),  TIER_CLEAR,    "класс 0 — тишина");

    return report("аналитика воздуха");
}
