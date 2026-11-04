// Консоль в настоящем браузере: нажатия, вкладки, мастер, гид и вёрстка на узких экранах.
//
// Это пятый слой, которого до 2026-09-19 не было: разбор файла ловит синтаксис и пути, но не
// отвечает, влезает ли текст в поле и не уходит ли кнопка за край. Страницу отдаёт tests/mock.py —
// подставной прибор с той же аутентификацией и теми же заголовками, а файл берётся настоящий.
//
//     node t_browser.js            проверить
//     node t_browser.js --headed   посмотреть своими глазами
'use strict';
const { chromium } = require('playwright');
const { spawn } = require('child_process');
const path = require('path');
const net = require('net');

const HERE = __dirname;
// Заглавные и символы намеренно: пока пароль мока был из одних строчных, приведение
// регистра в окне входа было невидимо — и заперло владельца в тот же день.
const PASS = 'Qw3rty!@';
// 412 и 430 добавлены 2026-09-30: это Pixel 7 и iPhone Pro Max, и ровно между 375 и 1024 пряталась
// шапка, уехавшая за правый край от одной новой кнопки. Список из четырёх ширин молчал, пока её не
// померили вручную — а «перенести порог» без этого выглядит как исправление и им не является.
const WIDTHS = [320, 360, 375, 412, 430, 1024];

let fails = 0, checks = 0;
function ok(cond, what) {
    checks++;
    if (!cond) fails++;
    console.log((cond ? '  ок      ' : '  ПРОВАЛ  ') + what);
}
function area(t) { console.log('== ' + t + ' =='); }

function freePort() {
    return new Promise(res => {
        const s = net.createServer();
        s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
    });
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function startMock(port) {
    const p = spawn('python3', [path.join(HERE, 'mock.py'), String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
    await new Promise((res, rej) => {
        const t = setTimeout(() => rej(new Error('mock не поднялся')), 10000);
        p.stdout.on('data', d => { if (String(d).includes('mock на')) { clearTimeout(t); res(); } });
        p.on('exit', c => { clearTimeout(t); rej(new Error('mock вышел с кодом ' + c)); });
    });
    return p;
}

// Вёрстка: ничего не должно уезжать за правый край, и ни один пункт списка не должен обрезаться.
// Ширину текста меряем тем же шрифтом, каким он нарисован, — иначе это гадание.
// Прокрутка здесь обязательна, а не предусмотрительна: окно теста 375x812, график на дашборде
// лежит ниже сгиба, и наведение по его координатам уходило мимо видимой области. Второй блок этого
// не замечал только потому, что нажатие на чип Playwright сам прокручивает к элементу.
// Повтор со сдвигом — от второй ловушки того же рода: движение мыши в ТУ ЖЕ точку, где курсор уже
// стоит после прошлого наведения, события не порождает вовсе.
async function hoverTip(page, fx, fy) {
  await page.locator('#chartBox').scrollIntoViewIfNeeded();
  await sleep(150);
  const box = await page.locator('#chartBox svg').boundingBox();
  for (let i = 0; i < 6; i++) {
    await page.mouse.move(box.x + box.width * fx + (i % 2 ? 9 : 0), box.y + box.height * fy);
    const txt = await page.evaluate(() => {
      const t = document.querySelector('#chartBox .tooltip');
      return (t && t.style.opacity === '1') ? t.textContent.trim() : '';
    });
    if (txt.length > 5) return txt;
    await sleep(200);
  }
  return '';
}
async function layout(page, w) {
    return page.evaluate(() => {
        const doc = document.documentElement;
        const vw = doc.clientWidth;
        const over = doc.scrollWidth > vw + 1;
        const past = [];
        for (const e of document.querySelectorAll('body *')) {
            const s = getComputedStyle(e);
            if (s.display === 'none' || s.visibility === 'hidden' || s.position === 'fixed') continue;
            const r = e.getBoundingClientRect();
            if (r.width === 0 || r.height === 0) continue;
            if (r.right > vw + 1) past.push((e.id || e.className || e.tagName) + ' до ' + Math.round(r.right));
        }
        const cut = [];
        const cv = document.createElement('canvas').getContext('2d');
        for (const sel of document.querySelectorAll('select')) {
            const r = sel.getBoundingClientRect();
            if (r.width === 0) continue;
            const cs = getComputedStyle(sel);
            cv.font = cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.fontFamily;
            const room = sel.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight) - 16;
            for (const o of sel.options)
                if (cv.measureText(o.textContent).width > room + 0.5)
                    cut.push((sel.id || sel.name) + ': «' + o.textContent + '»');
        }
        return { vw, over, past: past.slice(0, 4), cut: cut.slice(0, 4) };
    }).then(r => ({ ...r, want: w }));
}

(async () => {
    const port = await freePort();
    const mock = await startMock(port);
    const base = 'http://127.0.0.1:' + port;
    const browser = await chromium.launch({ headless: !process.argv.includes('--headed') });
    const errors = [];
    const ctx = await browser.newContext({ viewport: { width: 375, height: 812 } });
    const page = await ctx.newPage();
    // Исключение скрипта — всегда дефект. А вот 401 до входа — правильное поведение прибора, и
    // Chromium пишет в console.error ЛЮБОЙ неуспешный ответ, включая ожидаемые. Считать их
    // ошибками значило бы требовать, чтобы страница не пробовала запрашивать, пока не вошли, —
    // ровно то, ради чего окно входа и существует.
    page.on('pageerror', e => errors.push('исключение: ' + e.message));
    page.on('console', m => {
        if (m.type() !== 'error') return;
        const t = m.text();
        if (/Failed to load resource/i.test(t)) return;
        // Консоль намеренно вешает beforeunload: уход со страницы гасит радио прибора, а оно самое
        // дорогое, что он делает. Chromium отказывается показывать этот диалог при ПРОГРАММНОЙ
        // перезагрузке (нет жеста пользователя) и пишет об отказе в console.error. Это его политика,
        // а не наш дефект — и обработчик при этом работает, что проверяется отдельно.
        if (/beforeunload.*never had a user gesture/i.test(t)) return;
        errors.push('console.error: ' + t);
    });
    page.on('requestfailed', r => {
        // Отменённые опросы при уходе со страницы — не ошибка.
        if (!/aborted|ERR_ABORTED/i.test(r.failure()?.errorText || '')) errors.push('запрос не прошёл: ' + r.url());
    });

    try {
        area('вход');
        await page.goto(base + '/', { waitUntil: 'networkidle' });
        ok(await page.locator('#signin').isVisible(), 'без пароля показывается окно входа');
        ok(!(await page.locator('#v-set').isVisible()), 'настройки до входа не показываются');

        // Глаз у поля входа. Этот пароль переписывают со стекла — восемь случайных знаков со
        // заглавными, — и до 2026-10-08 его вводили вслепую: об опечатке узнавали только по
        // отказу, а их пять до минутной блокировки.
        ok(await page.locator('#siPassToggle').count() === 1, 'у поля входа есть кнопка глаза');
        ok(await page.locator('#siPassToggle').isVisible(), 'и она видна');
        ok(await page.locator('#siPassToggle').getAttribute('data-locked') === '1',
           'пока поле пусто, глаз явно заперт — показывать нечего');

        await page.fill('#siPass', 'wrongpass99');
        await page.click('#siBtn');
        await sleep(400);
        ok(await page.locator('#signin').isVisible(), 'неверный пароль не пускает');

        await page.fill('#siPass', PASS);
        ok(await page.locator('#siPassToggle').getAttribute('data-locked') !== '1',
           'с набранным паролем глаз доступен');
        ok(await page.locator('#siPass').getAttribute('type') === 'password', 'и по умолчанию пароль скрыт');
        await page.click('#siPassToggle');
        ok(await page.locator('#siPass').getAttribute('type') === 'text', 'нажатие открывает набранное');

        // Открыть мало — надо, чтобы владелец увидел ТО ЖЕ, что нарисовано на стекле. Значение в
        // поле верное, и его ничто не перекрашивает начертанием: до 2026-10-08 здесь стояло
        // text-transform:lowercase, остаток того дня, когда из doSignIn() убрали .toLowerCase().
        // Пока пароль был скрыт, эта ложь была невидима; с глазом она смотрела бы прямо в лицо.
        const geo = await page.evaluate(() => {
            const i = document.getElementById('siPass'), b = document.getElementById('siPassToggle');
            const ri = i.getBoundingClientRect(), rb = b.getBoundingClientRect(), cs = getComputedStyle(i);
            return { val: i.value, tt: cs.textTransform,
                     padL: parseFloat(cs.paddingLeft), padR: parseFloat(cs.paddingRight),
                     bw: rb.width, bRight: rb.right,
                     inside: rb.left >= ri.left - 0.5 && rb.right <= ri.right + 0.5,
                     docW: document.documentElement.clientWidth };
        });
        ok(geo.val === PASS, 'открыто именно набранное: ' + JSON.stringify(geo.val));
        ok(geo.tt === 'none',
           'и начертание не приводит регистр (text-transform: ' + geo.tt + ')');
        ok(geo.padR >= geo.bw + 4, 'под кнопку отведено место: ' + geo.padR + ' против ' + geo.bw + ' px');
        ok(Math.abs(geo.padR - geo.padL) < 0.5,
           'отступы симметричны — центровка не съехала (' + geo.padL + '/' + geo.padR + ')');
        ok(geo.inside, 'кнопка внутри самого поля');
        ok(geo.bRight <= geo.docW, 'и не уходит за правый край страницы');

        // 320 px — самый узкий телефон из списка; окно входа появляется до входа, поэтому в общий
        // прогон вёрстки ниже оно не попадает и мерить его надо здесь.
        await page.setViewportSize({ width: 320, height: 800 });
        await sleep(120);
        const narrow = await page.evaluate(() => {
            const i = document.getElementById('siPass'), b = document.getElementById('siPassToggle');
            const ri = i.getBoundingClientRect(), rb = b.getBoundingClientRect();
            return { inside: rb.left >= ri.left - 0.5 && rb.right <= ri.right + 0.5,
                     over: document.documentElement.scrollWidth > document.documentElement.clientWidth };
        });
        ok(narrow.inside && !narrow.over, 'на 320 px кнопка на месте и поперечной прокрутки нет');
        await page.setViewportSize({ width: 375, height: 812 });
        await sleep(120);

        await page.click('#siPassToggle');
        ok(await page.locator('#siPass').getAttribute('type') === 'password', 'повторное нажатие скрывает обратно');

        // Очистка руками не поднимает input, то есть sync() глаза сам не выполнится: открытый
        // пароль остался бы открытым над пустым полем. siClear подаёт событие за него.
        await page.click('#siPassToggle');
        await page.evaluate(() => siClear());
        ok(await page.locator('#siPass').getAttribute('type') === 'password',
           'очистка поля возвращает пароль под точки');
        ok(await page.locator('#siPassToggle').getAttribute('data-locked') === '1', 'и запирает глаз');

        // Вход с открытым паролем и по Enter: кнопка глаза стоит внутри обёртки поля, и ошибка в
        // её разметке могла бы перехватить отправку.
        await page.fill('#siPass', PASS);
        await page.click('#siPassToggle');
        await page.press('#siPass', 'Enter');
        await page.waitForSelector('#signin', { state: 'hidden', timeout: 8000 });
        ok(true, 'верный пароль открывает консоль (открытым, по Enter)');

        area('вкладки');
        for (const [t, probe] of [['dash', '#v-dash'], ['mode', '#v-mode'], ['net', '#v-net'],
                                  ['set', '#v-set'], ['sys', '#v-sys']]) {
            await page.evaluate(tab => go(tab), t);
            await sleep(250);
            ok(await page.locator(probe).isVisible(), 'вкладка ' + t + ' открывается');
        }

        area('калибровка температуры: арифметика на экране сходится');
        // Ровно случай из снимка владельца 2026-10-02: прибор читает одно, человек вводит другое,
        // и ответ обязан назвать ОБА числа — иначе «shifted by -0.2» рядом с показанным 20.4 и
        // введённым 20.5 выглядит ошибкой прибора, хотя прибор прав.
        await page.evaluate(() => go('set'));
        await sleep(300);
        {
            const live = (await page.locator('#tcalLive').textContent()).trim();
            const read = parseFloat(live);
            await page.fill('#tcalRef', (read - 0.3).toFixed(1));
            await page.click('#tcalBtn');
            await sleep(600);
            const m = (await page.locator('#tcalMsg').textContent()).trim();
            ok(/the device read/.test(m), 'сообщение называет показание прибора');
            ok(m.includes(read.toFixed(1)), 'и это ровно то, что было на экране (' + read.toFixed(1) + ')');
            ok(m.includes((read - 0.3).toFixed(1)), 'и введённое значение тоже');
            ok(/shifted by -0\.3/.test(m), 'и сдвиг сходится с ними: ' + m.slice(0, 90));
        }

        area('сырость: консоль показывает, на чём судила');
        // «Damp» без видимого основания — тот же тупик, что прежний жёсткий порог. Прибор отдаёт
        // влажность на холодной стене, и дашборд обязан её показывать; прочерк означает «улицы
        // нет», и его нельзя спутать с нулём.
        await page.evaluate(() => go('dash'));
        await sleep(300);
        {
            const v = (await page.locator('#dSurf').textContent()).trim();
            ok(/^\d+ %$/.test(v), 'влажность на стене показана числом (' + v + ')');
            const pop = await page.locator('#dSurf').locator('xpath=../..').textContent();
            ok(/80 %/.test(pop), 'и подпись называет порог плесени');
            ok(/outdoor/i.test(pop), 'и объясняет, откуда берётся стена');
        }
        {
            // Уровень, по которому судит барометр, и ПОЧЕМУ его может не быть. Дыра без причины
            // читается как поломка — тот же довод, что у «стены».
            const v = (await page.locator('#dSea').textContent()).trim();
            ok(/^1009\.3 hPa/.test(v), 'уровень у моря показан числом (' + v + ')');
            ok(/altitude measured/i.test(v), 'и сказано, что высота измерена по погоде');
            const pop = await page.locator('#dSea').locator('xpath=../..').textContent();
            ok(/1023/.test(pop) && /1008/.test(pop), 'подпись объясняет, зачем уровень вообще нужен');
        }

        area('график: полоса вердикта');
        // Структурная проверка доказывает, что код полосы есть; что она ДОШЛА до стекла — только
        // эта. Подставной прибор отдаёт намеренно смешанный ряд классов: на одних нулях нельзя
        // отличить «тихо» от «не нарисовано».
        // История у мока появляется только в режиме витрины — поэтому график в этом тесте не
        // проверялся ни разу, и это выяснилось, когда полоса «не нарисовалась».
        await page.request.get(base + '/mock?show=1');
        await page.reload();
        await page.evaluate(() => go('dash'));
        await page.waitForSelector('#chartBox svg', { timeout: 10000 });
        await sleep(300);
        {
            const b = await page.evaluate(() => {
                const rs = [...document.querySelectorAll('#chartBox svg rect[fill-opacity]')];
                const gy = [...document.querySelectorAll('#chartBox svg line')]
                    .filter(l => (l.getAttribute('stroke') || '').includes('border'))
                    .map(l => parseFloat(l.getAttribute('y1')));
                const lg = document.querySelector('#chartBox .chart-legend');
                return { n: rs.length,
                         inks: [...new Set(rs.map(r => r.getAttribute('fill')))].length,
                         top: rs.length ? Math.min(...rs.map(r => parseFloat(r.getAttribute('y')))) : -1,
                         plot: gy.length ? Math.max(...gy) : -1,
                         legend: lg ? lg.textContent : '' };
            });
            ok(b.n > 0, 'полоса нарисована, отрезков ' + b.n);
            // Четыре класса в окне — четыре оттенка. На трёх проверка прошла бы со слипшимися
            // Stuffy и Stale air, то есть ровно с тем дефектом, который она и нашла.
            ok(b.inks >= 4, 'классы различимы по цвету, оттенков ' + b.inks);
            // Полоса обязана стоять НИЖЕ поля данных: иначе она съела бы часть кривой. Условие
            // требует настоящих координат: при пустом графике обе величины были -1, и проверка
            // проходила, ничего не измерив.
            ok(b.plot > 0 && b.top >= b.plot,
               'полоса ниже последней линии сетки (' + b.top + ' >= ' + b.plot + ')');
            ok(/Stale air/.test(b.legend),
               'снятый класс 4 назван своим именем, а не числом: ' + b.legend.trim().slice(0, 70));
            ok(/Damp/.test(b.legend) && /Stuffy/.test(b.legend), 'и остальные классы окна тоже');
            ok(/No warning/.test(b.legend), 'и тихий класс назван, а не опущен');
        }
        {
            const tip = await hoverTip(page, 0.5, 0.4);
            ok(/No warning|Stuffy|Damp|Stale air/.test(tip),
               'подсказка называет вердикт этой записи: ' + tip.slice(0, 70));
        }

        area('график: давление с одним знаком');
        // Весь порог Rain — 1.2 мм рт. ст., и целыми единицами он округляется в «757 -> 756», то
        // есть в ничто. Это и был ответ на «почему Rain, падения же не было».
        await page.click('#metricChips .chip[data-m="pres"]');
        await sleep(400);
        {
            const leg = (await page.locator('#chartBox .chart-legend').textContent()).trim();
            ok(/Pressure\s+-?\d+\.\d/.test(leg), 'давление в легенде с десятыми: ' + leg.slice(0, 80));
            const tip = await hoverTip(page, 0.6, 0.5);
            ok(/Pressure -?\d+\.\d/.test(tip), 'и в подсказке тоже: ' + tip.slice(0, 80));
        }
        area('выгрузка истории: напряжение и климат BME690');
        // Две колонки BME690 появились в 0.9.60 и были пустыми в КАЖДОЙ выгрузке: консоль не
        // переносила эти поля из ответа прибора в свой ряд. Напряжение добавлено 2026-10-05 ради
        // замеров расхода по режимам — по проценту четверть пункта в сутки не видна. Проверка
        // открывает настоящий файл, а не смотрит на код, который его собирает.
        {
            const [dl] = await Promise.all([ page.waitForEvent('download', { timeout: 10000 }),
                                             page.click('#exportBtn') ]);
            const fs = require('fs');
            const text = fs.readFileSync(await dl.path(), 'utf8').trim().split('\n');
            const head = text[0].split(',');
            const rows = text.slice(1).map(l => l.split(','));
            const col = n => head.indexOf(n);
            ok(head[head.length - 1] === 'battery_v', 'напряжение — последняя колонка: ' + head.slice(-3).join(','));
            ok(col('bme_temp_c') === 9 && col('battery_pct') === 8,
               'прежние колонки на своих местах (battery_pct ' + col('battery_pct') + ', bme_temp_c ' + col('bme_temp_c') + ')');
            ok(rows.length > 50, 'строк ' + rows.length);
            const vs = rows.map(r => r[col('battery_v')]).filter(Boolean);
            ok(vs.length === rows.length && vs.every(v => /^\d\.\d{3}$/.test(v)),
               'напряжение в каждой строке, три знака: ' + vs.slice(0, 2).join(' '));
            ok(vs.some(v => v !== vs[0]), 'и оно меняется от строки к строке — это ряд, а не константа');
            const bt = rows.map(r => r[col('bme_temp_c')]).filter(Boolean);
            ok(bt.length > 0 && bt.length < rows.length,
               'климат BME690 есть там, где записан, и пуст там, где нет: ' + bt.length + ' из ' + rows.length);
        }

        await page.request.get(base + '/mock?show=0');
        await page.reload();
        await sleep(300);

        area('журнал безопасности');
        await page.evaluate(() => go('sys'));
        await sleep(250);
        // Свёрнут с 2026-10-08, и до раскрытия НИЧЕГО не скачано: это и есть вся экономия.
        // Спрашиваем ВИДИМОСТЬ, а не свойство open: у <div> оно undefined, и проверка на него
        // проходила бы при снятом <details> — поймано сломом 2026-10-08.
        ok(!(await page.locator('#secLog').isVisible()), 'журнал свёрнут, пока его не открыли');
        ok(await page.locator('#secBox').evaluate(e => e.tagName) === 'DETAILS',
           'и свёрнут именно раскрывающимся блоком');
        ok(/Loading/i.test(await page.locator('#secLog').textContent() || ''),
           'и до этого его никто не тянул');
        await page.locator('#secBox summary').click();
        await page.waitForFunction(() => {
            const e = document.getElementById('secLog');
            return e && e.textContent.trim().length > 20;
        }, { timeout: 8000 }).catch(() => {});
        const log = await page.locator('#secLog').textContent();
        ok(/signed in|console opened|password|update|factory/i.test(log || ''),
           'двоичный журнал разобран в тексте');


        area('смена пароля');
        // Владелец набрал qwerty123456 и получил «нельзя эти символы», не узнав какие
        // (железо, 2026-09-19). С тех пор алфавит стал полным, а длина — ровно восемь, но ответ
        // по-прежнему обязан называть виновника.
        // Карточка пароля переехала в настройки (решение владельца 2026-10-08): это настройка,
        // а не сведения о приборе.
        await page.evaluate(() => go('set'));
        await sleep(250);
        ok(await page.locator('#v-set #pwNew').count() === 1, 'карточка пароля во вкладке настроек');
        ok(await page.locator('#v-sys #pwNew').count() === 0, 'и в System её больше нет');
        ok(await page.locator('#wizAgainBtn').count() === 0, 'кнопки Run setup wizard нет');
        await page.fill('#pwNew', 'qwertyW1');
        await sleep(200);
        const e1 = ((await page.locator('#pwErr').textContent()) || '');
        ok(/"W"/.test(e1), 'заглавная W названа: ' + e1.slice(0, 90));
        ok(/wide|fits/i.test(e1), 'и объяснено почему — она шире строки');
        ok(!/"q"|"w"|"1"/.test(e1), 'строчная w и единица больше не запрещены');

        await page.fill('#pwNew', 'qw rty12');
        await sleep(200);
        ok(/" "|space/i.test((await page.locator('#pwErr').textContent()) || ''), 'пробел назван');

        await page.fill('#pwNew', 'qwerty');
        await sleep(200);
        ok(((await page.locator('#pwErr').textContent()) || '') === '',
           'недобранная длина по ходу набора не ругается');
        await page.click('#pwBtn');
        await sleep(200);
        ok(/exactly 8/i.test((await page.locator('#pwErr').textContent()) || ''),
           'по нажатию длина названа точным числом');

        // Двойники разрешены, но о них предупреждают: пароль переписывают со стекла, и час
        // 2026-09-19 ушёл ровно на это.
        await page.fill('#pwNew', 'qwe1Il0o');
        await sleep(200);
        const e3 = ((await page.locator('#pwErr').textContent()) || '');
        ok(/look alike/i.test(e3), 'про двойники предупреждают: ' + e3.slice(0, 80));
        ok(await page.locator('#pwErr.hint-warn').isVisible().catch(() => false),
           'это предупреждение, а не отказ');
        await page.click('#pwBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        await page.click('#mOk');
        await sleep(300);
        ok(await page.locator('#pwNote').isVisible(), 'и пароль с двойниками всё же принимается');

        // Регистр значим: приводить набранное к нижнему было бы подменой выбора владельца.
        await page.fill('#pwNew', 'Qw3rty!@');
        await sleep(200);
        ok(!(await page.locator('#pwErr').isVisible().catch(() => true)),
           'пароль с большой буквой, цифрой и символами проходит');
        await page.click('#pwBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        await page.click('#mOk');
        await sleep(400);
        ok(await page.locator('#pwNote').isVisible(), 'прибор принял пароль и сказал об этом');
        ok(((await page.locator('#pwNew').inputValue()) || '') === '', 'поле очищено после смены');


        // Выключение вручную. На e-ink выключенный прибор неотличим от сломанного, поэтому
        // диалог обязан сказать три вещи: что ничего не стирается, чем включить обратно и какая
        // у перерыва настоящая цена (газовый датчик теряет базовую линию).
        area('выключение прибора');
        // Смена пароля выше теперь идёт во вкладке настроек, поэтому в System надо вернуться.
        await page.evaluate(() => go('sys'));
        await sleep(250);
        ok(await page.locator('#offBtn').isVisible(), 'кнопка «Turn off» есть в System');
        await page.click('#offBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        const offTxt = ((await page.locator('#modal').textContent()) || '');
        ok(/nothing is erased|are kept/i.test(offTxt), 'диалог обещает, что ничего не стирается');
        ok(/press the button/i.test(offTxt), 'и говорит, чем включить обратно');
        ok(/air.quality/i.test(offTxt), 'и честно называет цену перерыва');
        // Отмена ничего не делает — это половина смысла подтверждения. Считаем сами запросы,
        // а не состояние подставного прибора: важно, что страница НЕ отправила, а не что тот
        // не посчитал.
        let offPosts = 0;
        page.on('request', r => { if (r.method() === 'POST' && r.url().includes('/api/poweroff')) offPosts++; });
        await page.click('#mCancel');
        await page.waitForSelector('#modal.show', { state: 'hidden', timeout: 4000 }).catch(() => {});
        await sleep(300);
        ok(offPosts === 0, 'отмена не отправляет запрос на выключение');
        // А подтверждение отправляет.
        await page.click('#offBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        await page.click('#mOk');
        await sleep(500);
        ok(offPosts === 1, 'подтверждение отправляет ровно один запрос');

        area('настройки батареи');
        // До 2026-09-19 ёмкость и порог были только в API, а три числа на дашборде считались из
        // них. Настройка, которой владелец не видит, но последствия которой ему показывают, хуже
        // отсутствующей.
        //
        // Режим Wi-Fi здесь обязателен: в Matter месяцы намеренно не оцениваются — цикл сна другой
        // и не измерен, поэтому карточка показывает прочерк вместо неверного числа.
        await page.request.get(base + '/mock?wl=0');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(600);

        const okModal = async () => {
            await page.waitForSelector('#modal.show', { timeout: 4000 }).catch(() => {});
            if (await page.locator('#modal.show').isVisible().catch(() => false)) {
                await page.click('#mOk');
                await page.waitForSelector('#modal.show', { state: 'hidden', timeout: 4000 }).catch(() => {});
            }
            await sleep(250);
        };

        await page.evaluate(() => go('set'));
        await sleep(250);
        // Решение пользователя 2026-09-27: ёмкость и порог отключения вшиты производителем и из
        // консоли убраны — владелец не должен иметь возможности сделать прибор неправильным.
        ok(!(await page.locator('#batMah').count()),     'поля ёмкости в консоли нет');
        ok(!(await page.locator('#lowbatV').count()),    'поля порога отключения нет');
        ok(!(await page.locator('#saveBattery').count()), 'кнопки сохранения батареи нет');

        // А оценка месяцев обязана остаться и считаться — до 2026-09-19 она делила на зашитые 1700
        // вместо ёмкости прибора, и это была ровно та ошибка, ради которой поле когда-то завели.
        const months = () => page.evaluate(() => {
            const e = document.querySelector('.modecard .life b');
            return e ? e.textContent.trim() : '';
        });
        await page.evaluate(() => go('mode'));
        await sleep(300);
        const before = await months();
        ok(before !== '—' && before !== '', 'в режиме Wi-Fi месяцы показываются: ' + before);
        // И считается от того, что отдал ПРИБОР, а не от своей копии: подменяем ёмкость в ответе
        // прибора и ждём, что число поедет.
        await page.evaluate(() => { ST.data.cfg.bat_mah = 3500; buildModeCards(); });
        await sleep(200);
        const after = await months();
        ok(before !== after, 'оценка следует за ёмкостью с прибора: ' + before + ' → ' + after);

        // Живые часы — самая дорогая настройка из тех, что владелец может тронуть: без них прибор
        // просыпается 288 раз в сутки вместо 1056. До 2026-09-21 выключатель был только в Custom,
        // то есть владельцу Optimal, просившему больше времени работы, предлагали ночной режим
        // (+27 %), а рычаг вдвое сильнее прятали. Теперь он общий — проверяем именно на Optimal.
        await page.evaluate(() => go('mode'));
        await sleep(200);
        await page.evaluate(() => { ST.selMode = 1; buildModeCards(); updateNightLock(); });
        await sleep(200);
        ok(await page.locator('#lcEn').isVisible(), 'выключатель часов виден вне Custom');
        ok(!(await page.locator('#lcEn').isDisabled()), 'и на Optimal он доступен');
        const lifeOf = () => page.evaluate(() => {
            const c = [...document.querySelectorAll('.modecard')][1];
            const b = c && c.querySelector('.life b');
            return b ? b.textContent.trim() : '';
        });
        await page.evaluate(() => { el('lcEn').checked = true;  el('lcEn').onchange(); });
        await sleep(150);
        const withClock = await lifeOf();
        await page.evaluate(() => { el('lcEn').checked = false; el('lcEn').onchange(); });
        await sleep(150);
        const noClock = await lifeOf();
        ok(withClock !== '' && noClock !== '', 'оценка Optimal показывается: ' + withClock + ' / ' + noClock);
        ok(withClock !== noClock, 'выключатель часов меняет оценку: ' + withClock + ' -> ' + noClock);
        const num = t => parseFloat(String(t).replace(/[^0-9.]/g, '')) || 0;
        ok(num(noClock) > num(withClock) || /year/i.test(noClock),
           'без часов оценка БОЛЬШЕ: ' + withClock + ' -> ' + noClock);
        ok(!(await page.locator('#applyMode').isDisabled()),
           'Apply оживает: иначе владелец выключил бы часы и не сохранил');
        // Ultra Save по-прежнему запирает часы — прошивка их гасит, и переключатель обязан это
        // показывать, а не притворяться работающим.
        await page.evaluate(() => { ST.selMode = 2; buildModeCards(); updateNightLock(); });
        await sleep(200);
        ok(await page.locator('#lcEn').isDisabled(), 'в Ultra Save часы заперты');
        ok(await page.locator('#lcLockNote').isVisible(), 'и объяснено почему');
        // А ночной режим в Ultra больше НЕ принудителен: 5 % не стоят решения за владельца.
        ok(!(await page.locator('#nmEn').isDisabled()), 'ночной режим в Ultra Save теперь выбирается');
        await page.evaluate(() => { ST.selMode = 1; el('lcEn').checked = true; el('lcEn').onchange();
                                    buildModeCards(); updateNightLock(); });
        await sleep(150);
        await page.evaluate(() => go('set'));
        await sleep(200);

        // Полоса обязана СКАЗАТЬ, что прислал хаб. Дефект 0.10.16 тем и был неприятен, что на
        // стекле «хаб этого не даёт» и «ячейка сломалась» выглядят одинаково — прочерками.
        await page.evaluate(() => go('set'));
        await sleep(250);
        await page.evaluate(() => { ST.strip = [5, 3, 2]; renderStrip(); });   // УФ, ветер, влажность
        await sleep(150);
        const feed = await page.locator('#stripFeed').innerText();
        ok(/UV:\s*4/.test(feed), 'консоль показывает присланный УФ: ' + feed.slice(0, 60));

        // Решение владельца 2026-09-28: список сокращён до шести. Дождь снят последним — MET даёт
        // вероятность осадков только по Скандинавии, и вне её ячейка не заполнилась бы никогда.
        // Номера снятых выведены из обращения, а не освобождены: прибор в поле мог хранить их в NVS.
        const opts = await page.evaluate(() =>
            [...document.querySelectorAll('#strip0 option')].map(o => o.textContent.trim()));
        ok(opts.length === 6, 'в списке ровно шесть вариантов, получено ' + opts.length);
        ok(!opts.some(o => /Rain|Cloud|Dew|Today/i.test(o)),
           'снятых вариантов в списке нет: ' + opts.join(', '));

        area('настроен без Wi-Fi: видно и лечится');
        await page.request.get(base + '/mock?no_wifi=1&wl=0');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        await page.evaluate(() => go('net'));
        await sleep(300);
        ok(await page.locator('#noWifiNote').isVisible(),
           'в разделе сети плашка называет состояние');
        const noteTxt = await page.locator('#noWifiNote').innerText();
        ok(/radio never comes up|without Wi-Fi/i.test(noteTxt) && /Save a network/i.test(noteTxt),
           'и говорит, чем лечится: ' + noteTxt.replace(/\s+/g, ' ').slice(0, 64));
        // Главное: вкладка Mode больше не утверждает обратное. До правки она печатала расписание
        // радио для прибора, чьё радио не включится, и по нему же считала срок работы батареи.
        await page.evaluate(() => go('mode'));
        await sleep(300);
        // Спрашиваем ту самую функцию, которая врала: она и есть предмет проверки, а скрести по
        // разметке карточки значит проверять вёрстку вместо правила.
        const radioRow = await page.evaluate(() => wifiRowVal(0).join(' / '));
        ok(/off/i.test(radioRow) && !/Every \d+ min/i.test(radioRow),
           'вкладка Mode говорит, что радио выключено: ' + radioRow);
        await page.request.get(base + '/mock?no_wifi=0');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);

        area('пароль брокера: поле обещает то, что прибор делает');
        await page.evaluate(() => go('net'));
        await sleep(300);
        await page.evaluate(() => selectConn(1));   // поля брокера живут в своей карточке
        await sleep(250);
        const hint = () => page.locator('#mqPassHelp').innerText();
        ok(/saved/i.test(await hint()), 'при прежнем адресе: пароль сохранён — ' + (await hint()).slice(0, 40));
        await page.fill('#mqHost', '10.0.0.9');
        await sleep(200);
        const moved = await hint();
        ok(/no longer applies|type the new one/i.test(moved),
           'при смене адреса поле говорит, что пароль придётся ввести заново: ' + moved.slice(0, 60));
        ok(await page.locator('#mqPassHelp.err').count() === 1, 'и помечено как отказ, а не как совет');

        area('корпоративную сеть прибор называет, а не даёт в неё биться');
        // В прошивке нет ни одного EAP-клиента, а connectSTA требует WIFI_AUTH_WPA_WPA2_PSK. До
        // 2026-10-01 скан сводил тип защиты к булеву "secure", поэтому сеть с доменным логином
        // выглядела как домашняя и отказывала словами «Could not join» — теми же, что при опечатке.
        await page.evaluate(() => go('net'));
        await sleep(250);
        await page.evaluate(() => selectConn(0));
        await sleep(250);
        await page.click('#scanBtn');
        await page.waitForSelector('#netlist .netitem', { timeout: 8000 });
        await sleep(300);
        ok(await page.locator('#netlist .netitem[data-ssid="CORP-WIFI"]').count() === 1,
           'корпоративная сеть в списке есть — прятать её значило бы показать сломанный скан');
        ok(/company login/i.test(await page.locator('#netlist .netitem[data-ssid="CORP-WIFI"]').innerText()),
           'и помечена как требующая корпоративного логина');
        await page.click('#netlist .netitem[data-ssid="CORP-WIFI"]');
        await page.waitForSelector('#modal.show', { timeout: 8000 });
        await sleep(300);
        const entTxt = await page.locator('#modal .box').innerText();
        ok(/802\.1X|company login/i.test(entTxt) && /cannot join/i.test(entTxt),
           'нажатие объясняет, а не даёт ввести пароль: ' + entTxt.replace(/\s+/g, ' ').slice(0, 80));
        ok(await page.inputValue('#wSsid') !== 'CORP-WIFI',
           'и имя такой сети в форму не подставляется');
        await page.click('#mOk');
        await sleep(300);
        // Обычная сеть по-прежнему подставляется — иначе починили бы одно и сломали другое.
        await page.click('#netlist .netitem[data-ssid="HomeWiFi"]');
        await sleep(300);
        ok(await page.inputValue('#wSsid') === 'HomeWiFi', 'обычная сеть подставляется как раньше');

        area('три запомненные сети: прибор ездит, владелец не перенастраивает');
        // Идея владельца 2026-10-01, после того как он унёс прибор в офис и перенастраивал его там.
        await page.request.get(base + '/mock?wl=0');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        await page.evaluate(() => go('net'));
        await sleep(300);
        await page.evaluate(() => selectConn(0));
        await sleep(300);
        ok(await page.evaluate(() => getComputedStyle(el('savedNetsCard')).display !== 'none'),
           'карточка запомненных сетей показана, когда их больше одной');
        const rows = () => page.evaluate(() => Array.from(
            document.querySelectorAll('#savedNets [data-forget]')).map(b => b.dataset.forget));
        ok((await rows()).length === 3, 'перечислены все три: ' + (await rows()).join(', '));
        ok(/on this one/i.test(await page.locator('#savedNets').innerText()),
           'та, на которой прибор сейчас, помечена');
        ok(await page.evaluate(() => {
              const live = ST.data.net.ssid;
              const b = document.querySelector('#savedNets [data-forget="' + live + '"]');
              return !!b && b.disabled;
           }), 'и её нельзя забыть, пока прибор на ней');
        // Пароль наружу не отдаётся ни в каком виде — список это только имена.
        const statusTxt = await (await page.request.get(base + '/api/status')).text();
        ok(!/"pass"\s*:\s*"[^"]/.test(statusTxt), 'перечень не несёт ни одного пароля');
        await page.click('#savedNets [data-forget="Office-Guest"]');
        await page.waitForSelector('#modal.show', { timeout: 8000 });
        await sleep(300);
        await page.click('#mOk');
        await sleep(600);
        ok((await rows()).length === 2 && !(await rows()).includes('Office-Guest'),
           'забытая сеть исчезает из списка: ' + (await rows()).join(', '));
        const saved = await (await page.request.get(base + '/mock')).json();
        ok(!saved.nets.includes('Office-Guest'), 'и с прибора тоже');
        await page.request.get(base + '/mock?wl=2');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);

        area('форма Wi-Fi не меняет режим — и теперь об этом говорит');
        // Найдено в поле 2026-10-01. На экране ДВЕ главные кнопки: маленькая в янтарной плашке
        // («Use Wi-Fi only»), которая меняет режим, и большая внизу формы («Save & connect»),
        // которая намеренно сохраняет только сеть — чтобы смена пароля Wi-Fi не выбрасывала прибор
        // из хаба. Владелец выбрал карточку Wi-Fi, нажал очевидную кнопку, получил «Wi-Fi
        // connected», перезагрузил прибор — и тот остался в MQTT. Разделение остаётся, молчание нет.
        await page.request.get(base + '/mock?wl=1&conn=2');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        await page.evaluate(() => go('net'));
        await sleep(300);
        await page.evaluate(() => selectConn(0));   // карточка Wi-Fi при работающем режиме хаба
        await sleep(250);
        ok(await page.evaluate(() => getComputedStyle(el('modeSwitch')).display !== 'none'),
           'выбор карточки Wi-Fi поднимает плашку смены режима');
        ok(/Use Wi-Fi only/i.test(await page.locator('#modeSwitchBtn').innerText()),
           'и на её кнопке написано, что именно она делает');
        await page.request.get(base + '/mock/saves');          // сбрасывать нечего — читаем после
        await page.click('#saveWifi');
        await page.waitForSelector('.modal', { state: 'visible', timeout: 8000 });
        await sleep(400);
        const wifiModal = await page.locator('.modal').innerText();
        ok(/network only/i.test(wifiModal),
           'после Save & connect окно говорит, что сохранена ТОЛЬКО сеть');
        ok(/Use Wi-Fi only/i.test(wifiModal),
           'и называет кнопку, которая меняет режим: ' + wifiModal.replace(/\s+/g, ' ').slice(0, 90));
        const sent = await (await page.request.get(base + '/mock/saves')).json();
        ok(!sent.some(b => b && Object.prototype.hasOwnProperty.call(b, 'wl_mode')),
           'и сама форма режим прибору не отправляла');
        // Окно снимает перезагрузка страницей ниже — своей кнопки закрытия у него нет.
        // Обратная сторона: в обычном Wi-Fi говорить не о чем, и оговорки быть не должно.
        await page.request.get(base + '/mock?wl=0');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        await page.evaluate(() => go('net'));
        await sleep(300);
        await page.evaluate(() => selectConn(0));
        await sleep(250);
        await page.click('#saveWifi');
        await page.waitForSelector('.modal', { state: 'visible', timeout: 8000 });
        await sleep(400);
        ok(!/network only/i.test(await page.locator('.modal').innerText()),
           'в режиме Wi-Fi оговорки нет — ей нечего сообщать');
        await page.request.get(base + '/mock?wl=2');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);

        area('калибровка CO2: прибор спрашивает, а не гадает');
        await page.evaluate(() => go('set'));
        await sleep(250);
        const calState = () => page.evaluate(() => ({
            msg: el('calMsg').textContent,
            stop: getComputedStyle(el('calAsk')).display !== 'none',
            btn: el('calibrateBtn').textContent,
        }));
        const drive = q => page.request.get(base + '/mock?' + q);
        const calls = async () => (await (await page.request.get(base + '/mock/saves')).json())
            .filter(x => x.path === '/api/calibrate');

        // Вопрос задаётся ДО того, как что-то прочитано, и ровно там, где на него можно ответить.
        const calBefore = (await calls()).length;
        await page.click('#calibrateBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        const ask = await page.locator('#modal .box').innerText();
        ok(/outside/i.test(ask), 'спрашивает, на улице ли прибор: ' + ask.split('\n')[0]);
        ok(/drift|cannot check/i.test(ask),
           'и объясняет, почему прибор не может проверить это сам');
        await page.click('#mCancel');
        await sleep(400);
        ok((await calls()).length === calBefore, 'отказ ничего не отправляет прибору');

        // Согласие — запуск. Эталон 420 не редактируется, это отдельное решение.
        await page.click('#calibrateBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        await page.click('#mOk');
        await page.waitForFunction(() => /Measuring/i.test(el('calMsg').textContent),
                                   { timeout: 15000 }).catch(() => {});
        const started = (await calls()).filter(x => x.body && x.body.ref);
        ok(started.length === 1 && started[0].body.ref === 420,
           'запуск уходит с эталоном 420 ppm');
        let st2 = await calState();
        ok(/Measuring/i.test(st2.msg), 'карточка показывает ход: ' + st2.msg.slice(0, 48));
        ok(st2.stop, 'и Stop доступен всё время прогона');

        // Шесть замеров прочитаны — прибор калибрует САМ, без второго вопроса. До 2026-09-30
        // среднее выше 600 останавливало прогон вопросом, ответить на который с балкона было нечем.
        await drive('cal=done&cal_avg=770&cal_lo=752&cal_hi=789&cal_corr=-350');
        await page.waitForFunction(() => /Done/i.test(el('calMsg').textContent),
                                   { timeout: 15000 }).catch(() => {});
        st2 = await calState();
        ok(/Done/i.test(st2.msg), 'высокое среднее больше не останавливает калибровку');
        ok(/770/.test(st2.msg) && /752/.test(st2.msg),
           'прочитанные числа показаны, но как сведения');
        ok(/-350/.test(st2.msg), 'и поправка названа');
        ok(/not outside|run this again/i.test(st2.msg),
           'с оговоркой, что делать, если прибор всё же был в комнате');
        ok(!st2.stop, 'после конца Stop убран');

        // Stop за шесть минут прогона — то, чем отменяется случайное нажатие.
        await drive('cal=running&cal_n=2');
        await page.click('#calibrateBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        await page.click('#mOk');
        await page.waitForFunction(() => getComputedStyle(el('calAsk')).display !== 'none',
                                   { timeout: 15000 }).catch(() => {});
        ok((await calState()).stop, 'Stop снова на месте');
        await page.click('#calNo');
        await sleep(600);
        const stopped = (await calls()).filter(x => x.body && x.body.cancel);
        ok(stopped.length === 1, 'Stop уходит отдельным запросом, а не вторым запуском');
        await drive('cal=idle&cal_n=0&cal_err=0');
        await sleep(3400);

        area('порог тревоги CO2: своё значение');
        await page.evaluate(() => go('set'));
        await sleep(250);
        const co2State = () => page.evaluate(() => ({
            sel: el('alCo2').value,
            shown: getComputedStyle(el('alCo2CustomRow')).display !== 'none',
            box: el('alCo2Custom').value,
            hint: el('alCo2Hint').textContent,
            bad: el('alCo2Custom').classList.contains('err'),
        }));
        const devLevel = async () => (await (await page.request.get(base + '/mock')).json()).co2lvl;

        // Обычный случай не сдвинулся: пресет выбран, поля своего значения не видно.
        let st = await co2State();
        ok(st.sel === '1100' && !st.shown, 'по умолчанию выбран пресет, поле скрыто');

        await page.selectOption('#alCo2', 'custom');
        await sleep(150);
        st = await co2State();
        ok(st.shown, 'пункт «Custom level…» открывает поле');
        ok(/700/.test(st.hint) && /1600/.test(st.hint), 'и подпись называет полосу: ' + st.hint);

        // Число вне полосы: прибор зажал бы его до 1600 и промолчал, а окно отчиталось бы о 2000 —
        // то есть владельцу назвали бы уровень, которого у него нет. Поэтому отказ до отправки.
        await page.fill('#alCo2Custom', '2000');
        await sleep(150);
        st = await co2State();
        ok(st.bad, 'число вне полосы помечено прямо в поле');
        const savesBefore = (await (await page.request.get(base + '/mock/saves')).json()).length;
        await page.click('#saveAlerts');
        await sleep(400);
        const savesAfter = (await (await page.request.get(base + '/mock/saves')).json()).length;
        ok(savesAfter === savesBefore, 'и на прибор ничего не ушло');
        ok(await devLevel() === 1100, 'уровень на приборе не тронут');
        ok(!(await page.locator('#modal.show').isVisible().catch(() => false)),
           'окна «сохранено» при отказе не появилось');

        // Годное число: сохраняется как набрано, и отчёт называет именно его.
        await page.fill('#alCo2Custom', '1250');
        await sleep(150);
        await page.click('#saveAlerts');
        await sleep(500);
        const modalTxt = await page.locator('#modal .box').innerText().catch(() => '');
        ok(/1250/.test(modalTxt), 'отчёт называет набранный уровень: ' + modalTxt.replace(/\s+/g, ' ').slice(0, 70));
        await okModal();
        ok(await devLevel() === 1250, 'прибор сохранил 1250');

        // Круг целиком. Значение вне списка НЕЛЬЗЯ присваивать <select>: браузер оставит показанным
        // первый пункт, и следующее сохранение запишет 900 поверх уровня владельца. Поэтому
        // перезагрузка — обязательная часть проверки, а не вежливость.
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        await page.evaluate(() => go('set'));
        await sleep(300);
        st = await co2State();
        ok(st.sel === 'custom' && st.box === '1250' && st.shown,
           'после перезагрузки уровень 1250 читается как своё значение, а не как 900');

        // Опрос статуса идёт раз в несколько секунд и не имеет права затирать то, что владелец
        // набирает. Тот же дефект уже ловили на нижней полосе 2026-09-24.
        await page.fill('#alCo2Custom', '1310');
        await page.evaluate(() => { fillForms(ST.data.cfg); });    // как будто пришёл ответ прибора
        await sleep(150);
        st = await co2State();
        ok(st.box === '1310', 'опрос не затирает несохранённое, в поле по-прежнему ' + st.box);

        // Вёрстка: поле и его подпись на самом узком экране.
        await page.setViewportSize({ width: 320, height: 800 });
        await sleep(300);
        const lay = await layout(page, 320);
        ok(!lay.over, 'на 320 px горизонтальной прокрутки нет');
        ok(lay.past.length === 0, 'ничего за правым краем: ' + lay.past.join('; '));
        ok(lay.cut.length === 0, 'ни один пункт списка не обрезан: ' + lay.cut.join('; '));
        await page.setViewportSize({ width: 375, height: 812 });
        await sleep(200);

        // Вернуть прибор и страницу в исходное — дальше идут проверки, которые про это не знают.
        await page.selectOption('#alCo2', '1100');
        await sleep(120);
        await page.click('#saveAlerts');
        await sleep(400);
        await okModal();
        ok(await devLevel() === 1100, 'пресет выбирается обратно и сохраняется');

        area('гид: ссылка вместо панели');
        // Решение владельца 2026-10-08: карточка гида переехала из настроек на дашборд, наверх —
        // владелец должен увидеть её сразу, как открылась консоль, а не найдя во вкладке.
        await page.evaluate(() => go('dash'));
        await sleep(250);
        // Решение владельца 2026-09-28: гид уехал на сайт, здесь остаётся ссылка. Панели и 24
        // вшитых снимка больше нет — это было 80 КБ флеша на каждом приборе.
        ok(!(await page.locator('#guideBtn').count()),   'кнопки «Open the guide» нет');
        ok(!(await page.locator('#guidePanel').count()), 'панели гида нет');
        ok(!(await page.locator('img[data-g]').count()), 'вшитых снимков в странице нет');
        const glink = page.locator('a[href*="smartmeteo.org/guide"]').first();
        ok(await glink.count() > 0, 'ссылка на гид есть');
        ok(/smartmeteo\.org\/guide/.test(await glink.getAttribute('href') || ''),
           'и ведёт на сайт: ' + (await glink.getAttribute('href')));
        // Ссылка без объяснения, почему она может не открыться, читается как сломанный прибор:
        // у своей точки доступа прибора интернета нет.
        // Кнопка со значком (вариант владельца, 2026-10-08), оговорка — отдельной строкой под ней.
        ok((await page.evaluate(() =>
               getComputedStyle(document.querySelector('.guidelink')).textDecorationLine)) === 'none',
           'кнопка не подчёркнута — выглядит кнопкой, а не сырым HTML');
        ok(await page.locator('.guidelink svg').count() === 1, 'со значком книги');
        ok(await page.evaluate(() => {
               const cs = getComputedStyle(document.querySelector('.guidelink'));
               return cs.backgroundColor;
           }) !== 'rgba(0, 0, 0, 0)', 'и залита акцентом, а не прозрачная');

        // Оговорка про интернет — только там, где она верна. Мок по умолчанию в домашней сети.
        const capHome = (await page.locator('#guideCap').textContent()) || '';
        ok(!/needs internet/i.test(capHome),
           'в домашней сети оговорки про интернет нет: ' + capHome.trim());
        ok(/smartmeteo\.org\/guide/.test(capHome), 'но адрес остаётся');

        await page.request.get(base + '/ownap?on=1');
        await page.waitForFunction(() => /needs internet/i.test(
            document.getElementById('guideCap').textContent || ''), { timeout: 8000 }).catch(() => {});
        const gtxt = (await page.locator('#guideCap').textContent()) || '';
        await page.request.get(base + '/ownap?on=0');

        // Голая <a> без класса — их в консоли пять (MET, лицензия CC, регистрация OWM). До
        // 2026-10-08 они выходили браузерным синим: на тёмном фоне это выглядит как ошибка.
        await page.evaluate(() => go('set'));
        await sleep(250);
        const bare = await page.evaluate(() => {
            const a = document.querySelector('a[href*="api.met.no"]');
            if (!a) return null;
            const cs = getComputedStyle(a);
            return { color: cs.color, deco: cs.textDecorationLine };
        });
        ok(bare !== null, 'голая ссылка на странице найдена');
        ok(bare && bare.color !== 'rgb(0, 0, 238)' && bare.color !== 'rgb(0, 0, 255)',
           'и она не браузерного синего: ' + (bare && bare.color));
        ok(bare && bare.deco === 'none', 'и не подчёркнута в покое');
        await page.evaluate(() => go('dash'));
        await sleep(200);
        ok(/internet/i.test(gtxt) && /setup network/i.test(gtxt),
           'и сказано, что ей нужен интернет, которого нет на сети прибора');

        // Место: ниже плашек показателей и прямо над History (просьба владельца, 2026-10-09).
        // Меряем геометрией, а не порядком в разметке: это то, что человек видит.
        const gpos = await page.evaluate(() => {
            const btn  = document.querySelector('.guidelink');
            const mIaq = document.getElementById('mIaq');
            const hist = [...document.querySelectorAll('#v-dash h3')]
                            .find(h => /History/i.test(h.textContent));
            const y = e => e ? e.getBoundingClientRect().top + window.scrollY : null;
            return { btn: y(btn), metrics: y(mIaq), hist: y(hist) };
        });
        ok(gpos.btn !== null && gpos.metrics !== null && gpos.hist !== null, 'кнопка, плашки и History найдены');
        ok(gpos.btn > gpos.metrics, 'кнопка ниже плашек показателей (' + Math.round(gpos.btn) + ' > ' + Math.round(gpos.metrics) + ')');
        ok(gpos.btn < gpos.hist, 'и выше блока History (' + Math.round(gpos.btn) + ' < ' + Math.round(gpos.hist) + ')');
        ok(await page.locator('#v-dash a[href*="smartmeteo.org/guide"]').count() > 0,
           'и она именно во вкладке дашборда');
        ok(await page.locator('#v-dash #dStatIaq').count() === 0,
           'разбор по воздуху с дашборда убран');
        ok(await page.locator('#v-sys #dStatIaq').count() === 1,
           'и найдён во вкладке System');
        ok(await page.locator('#v-set .sectlabel').filter({ hasText: /air-quality statuses/i }).count() === 0,
           'справочник классов из настроек убран');
        ok(await page.locator('#bcalBtn').count() === 0,
           'карточки калибровки делителя в консоли нет');

        area('вёрстка: обычные данные');
        for (const w of WIDTHS) {
            await page.setViewportSize({ width: w, height: 812 });
            for (const t of ['dash', 'mode', 'net', 'set', 'sys']) {
                await page.evaluate(tab => go(tab), t);
                await sleep(120);
                const r = await layout(page, w);
                ok(!r.over, w + ' px, ' + t + ': горизонтальной прокрутки нет');
                ok(r.past.length === 0, w + ' px, ' + t + ': ничего за правым краем'
                   + (r.past.length ? ' — ' + r.past.join('; ') : ''));
                ok(r.cut.length === 0, w + ' px, ' + t + ': пункты списков влезают'
                   + (r.cut.length ? ' — ' + r.cut.join('; ') : ''));
            }
        }

        area('вёрстка: предельные данные');
        await page.request.get(base + '/stress?on=1');
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(600);
        for (const w of [320, 375]) {
            await page.setViewportSize({ width: w, height: 812 });
            for (const t of ['dash', 'set', 'sys']) {
                await page.evaluate(tab => go(tab), t);
                await sleep(150);
                const r = await layout(page, w);
                ok(!r.over, w + ' px, ' + t + ' на предельных: прокрутки нет');
                ok(r.past.length === 0, w + ' px, ' + t + ' на предельных: ничего за краем'
                   + (r.past.length ? ' — ' + r.past.join('; ') : ''));
            }
        }
        await page.request.get(base + '/stress?on=0');

        // Прошивка без Bosch BSEC (публичные исходники): индекса нет, и консоль говорит почему,
        // а не обещает прогрев навсегда. CO2 и прочее при этом на месте.
        area('сборка без Bosch BSEC');
        await page.request.get(base + '/nobsec?on=1');
        await page.setViewportSize({ width: 375, height: 812 });
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(600);
        await page.evaluate(() => go('dash'));
        await sleep(200);
        const nb = await page.evaluate(() => ({
            big:  document.getElementById('airBig').textContent,
            desc: document.getElementById('airDesc').textContent,
            tag:  document.getElementById('mIaqt').textContent,
            acc:  document.getElementById('calAcc').textContent,
            stat: document.getElementById('dStatIaq').textContent,
            co2:  document.getElementById('mCo2').textContent }));
        ok(nb.big === 'No air-quality index', 'большое слово — «No air-quality index»: ' + nb.big);
        ok(/BSEC/.test(nb.desc) && /CO₂/.test(nb.desc), 'под ним — почему, и что работает: ' + nb.desc);
        ok(!/warming/i.test(nb.big + nb.desc + nb.stat + nb.acc), 'нигде не обещает прогрев');
        ok(nb.tag === 'not in this build', 'плашка IAQ — «not in this build»');
        ok(nb.acc === 'Not in this build', 'калибровка газа — «Not in this build»');
        ok(/601/.test(nb.co2), 'CO₂ показывается как обычно');
        await page.request.get(base + '/nobsec?on=0');

        area('мастер первой настройки');
        await page.request.get(base + '/mock?setup=1');
        await page.setViewportSize({ width: 375, height: 812 });
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(800);
        ok(await page.locator('#wiz').isVisible(), 'мастер открывается на ненастроенном приборе');
        // Шаги не проходятся одним Continue: первый требует имя сети (сеть, которую никто не
        // набрал, — не настройка, а опечатка), необязательные предлагают Skip. Тест ведёт себя как
        // человек: заполняет обязательное и пропускает необязательное.
        const seen = [];
        let tourShots = 0;
        for (let i = 0; i < 14; i++) {
            const title = (await page.locator('#wizTitle').textContent().catch(() => '')) || '';
            if (title && title.trim() !== seen[seen.length - 1]) seen.push(title.trim());
            const r = await layout(page, 375);
            if (r.over || r.past.length) { ok(false, 'шаг «' + title + '»: вёрстка уехала'); break; }

            if (await page.locator('#wzSsid').isVisible().catch(() => false))
                await page.fill('#wzSsid', 'HomeWiFi');          // сеть, которую подставной прибор примет

            // Картинки тура строятся скриптом, а не лежат в разметке, поэтому увидеть их можно
            // только на открытом шаге. Пустая рамка вместо снимка экрана ничего не ломает и ничего
            // не пишет в журнал — её замечает либо этот тест, либо владелец.
            const shots = await page.evaluate(() => {
                const a = [...document.querySelectorAll('#wizBody img[data-g]')];
                return { n: a.length, bad: a.filter(i => !i.complete || i.naturalWidth === 0).map(i => i.dataset.g) };
            });
            if (shots.n) {
                tourShots += shots.n;
                ok(shots.bad.length === 0, 'шаг «' + title + '»: снимки отрисованы'
                   + (shots.bad.length ? ' — пусты: ' + shots.bad.join(', ') : ''));
            }

            const skip = page.locator('#wzWxSkip, #wzHubSkip').first();
            if (await skip.isVisible().catch(() => false)) { await skip.click(); await sleep(350); continue; }

            const next = page.locator('#wizNext');
            if (!(await next.isVisible().catch(() => false))) break;
            const label = ((await next.textContent()) || '').trim();
            if (/Apply/i.test(label)) break;
            await next.click();
            await sleep(600);                                     // шаг сети ждёт ответа о подключении
            await page.waitForFunction(() => !document.getElementById('wizNext').disabled,
                                       { timeout: 25000 }).catch(() => {});
        }
        ok(seen.length >= 5, 'мастер прошёл ' + seen.length + ' шагов: ' + seen.join(' / '));
        ok(seen.some(s => /button/i.test(s)), 'экран про кнопку в мастере остался');
        ok(tourShots === 0, 'снимков в мастере больше нет, их ' + tourShots);
        // Экран про кнопку рисуется встроенным SVG, а не картинкой, поэтому он и остался: кнопка —
        // единственный орган управления, и объяснить её надо до того, как человек уйдёт с этой сети.
        ok(!seen.some(s => /your screen|air quality|something is wrong/i.test(s)),
           'три экрана про чтение дисплея уехали на сайт: ' + seen.join(' / '));
        ok(seen[seen.length - 1] === 'Ready to finish', 'мастер доходит до итогового шага');
        // Шагов в таблице девять, а видно восемь — и это не пропажа: на подключённом приборе мастер
        // берёт время и пояс с телефона сам и перескакивает через экран часов. Проверка держит это
        // решение на месте: если оно пропадёт, у человека появится лишний вопрос, на который он уже
        // ответил тем, что у него в руках телефон с верными часами.
        ok(!seen.includes('Set the clock'),
           'шаг часов пропущен: время взято с телефона (видно шагов ' + seen.length + ')');
        await page.request.get(base + '/mock?setup=0');

        area('батарея: одна подгонка на всю карточку');
        // До 2026-10-03 три верхних числа считал второй наклон, который обнулялся каждой
        // перезагрузкой и заговаривал через шесть часов: «523 days, 0.16 %/day, 3 mAh» под
        // заметкой «Collecting». Теперь прибор их придерживает, а страница рисует прочерк.
        await page.evaluate(() => go('dash'));
        await page.request.get(base + '/mock?bat=collecting');
        await page.evaluate(() => load());
        await sleep(400);
        const batTxt = async () => [await page.locator('#batTte').innerText(),
                                    await page.locator('#batRate').innerText(),
                                    await page.locator('#batDaily').innerText()].map(x => x.trim());
        let bt = await batTxt();
        ok(bt.every(x => x === '—'), 'пока копится — три прочерка, без чисел и без «estimating» (' + bt.join(' | ') + ')');
        await page.request.get(base + '/mock?bat=measured');
        await page.evaluate(() => load());
        await sleep(400);
        bt = await batTxt();
        ok(bt[0] === '166 days' && bt[1] === '0.50 %/day' && bt[2] === '10 mAh',
           'когда прибор отдал числа — страница их показывает как есть (' + bt.join(' | ') + ')');
        await page.request.get(base + '/mock?bat=collecting');

        area('возврат на страницу из фона');
        // Найдено на телефоне 2026-10-03: ушёл в другое приложение, вернулся — окно «The device
        // stopped answering» поверх шапки, где написано «Kept open» и «Connected». Телефон
        // замораживает страницу в фоне, опросов нет вовсе, и молчание считалось от последнего
        // ответа по настенным часам; первый опрос после возврата падал (Wi-Fi телефона ещё
        // просыпается) — и одного падения хватало.
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        const setVis = v => page.evaluate(v => {
            if (v === 'visible') delete document.visibilityState;
            else Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => v });
            document.dispatchEvent(new Event('visibilitychange'));
        }, v);
        const goneUp = async () => (await page.locator('#modal.show').count()) > 0
            && /stopped answering/i.test(await page.locator('#mTitle').innerText());
        // В фоне 27 с при молчащем приборе. Срок не круглый: опрос раз в 5 с, первое падение
        // может прийти на пятой секунде, а правилу нужно 15 с от него — меньшее ожидание
        // пропускало именно тот слом, ради которого эта проверка написана. Chromium под тестом страницу не замораживает, и опросы
        // продолжают падать — ровно случай браузера, который в фоне ещё живёт, а сети уже нет.
        // Ни одно такое падение не считается: прибору нечего сказать странице, которую не видно.
        await page.request.get(base + '/mock?down=1');
        await setVis('hidden');
        await sleep(27000);
        ok(!(await goneUp()), 'опросы, упавшие в фоне, ничего не объявляют');
        await setVis('visible');                 // возврат сам зовёт опрос — и тот падает
        await sleep(1500);
        ok(!(await goneUp()), 'первый неудачный опрос после фона не объявляет прибор пропавшим');
        await page.request.get(base + '/mock?down=0');
        await sleep(5600);
        ok((await page.locator('#connt').innerText()).trim() === 'Connected' && !(await goneUp()),
           'следующий опрос ответил — страница живёт дальше, никаких окон');
        // А прибор, ушедший по-настоящему, по-прежнему называется: три неудачи подряд на экране.
        await page.request.get(base + '/mock?down=1');
        await page.waitForSelector('#modal.show', { timeout: 32000 }).catch(() => {});
        ok(await goneUp(), 'настоящее молчание на экране (≥15 с) по-прежнему объявляется');
        await page.request.get(base + '/mock?down=0');
        await page.waitForSelector('#modal.show', { state: 'hidden', timeout: 8000 }).catch(() => {});
        ok(!(await goneUp()), 'прибор ответил снова — окно закрывается само, не дожидаясь «Got it»');
        ok(await page.locator('#closeBtn').isVisible(), 'и крестик возвращается: окно есть, его можно закрыть');

        area('шапка: один ряд, одна высота, говорит один элемент');
        // Поводом была не только теснота. Мерено на подставном приборе 2026-10-10: на 360 и 375 px
        // запас справа был РОВНО НОЛЬ, а на 320 px со словом «Reconnecting…» кнопка темы уходила
        // на 16 px за край, и таблетка отсчёта сминалась в две строки — у .pill нет ни flex:none,
        // ни nowrap. Общий прогон вёрстки этого не ловил: он меряет нормальное состояние, а
        // длинное слово приходит только когда связь пропала.
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(900);
        const hdrFit = () => page.evaluate(() => {
            const vw = document.documentElement.clientWidth;
            const items = [...document.querySelector('.hd').children]
                .filter(c => getComputedStyle(c).display !== 'none' && c.className !== 'spacer');
            return {
                past: items.filter(c => c.getBoundingClientRect().right > vw - 1)
                           .map(c => c.id || c.className),
                heights: [...new Set(items.filter(c => !c.classList.contains('logo'))
                           .map(c => Math.round(c.getBoundingClientRect().height)))],
                timer: !!document.querySelector('#portalTimer:not([style*="display: none"])'),
                connCls: document.getElementById('conn').className,
                wordShown: getComputedStyle(document.getElementById('connt')).display !== 'none',
                lines: Math.round(document.getElementById('portalTimer').getBoundingClientRect().height),
            };
        });
        let h = await hdrFit();
        ok(h.past.length === 0, 'на 375 px ничего не выходит за правый край');
        ok(h.heights.length === 1, 'все органы шапки одного роста (' + h.heights.join('/') + ')');
        ok(h.connCls.includes('hdot') && !h.wordShown,
           'пока связь есть, статус — точка, а слово занимает место только когда есть что сказать');

        // Нажатие показывает СВОЁ действие. До 2026-10-10 оно перечитывало ответ, снятый ДО
        // нажатия, и шапка три секунды показывала состояние, противоположное нажатому.
        await page.click('#portalTimer');
        await sleep(400);
        ok((await page.locator('#portalTimer').getAttribute('class')).includes('held'),
           'нажатие «держать окно» видно сразу, а не через опрос');
        await page.click('#portalTimer');
        await sleep(400);
        ok(!(await page.locator('#portalTimer').getAttribute('class')).includes('held'),
           'и повторное возвращает отсчёт');

        // Янтарь остался — но на последнюю минуту, то есть значит ровно то, что значит.
        await page.evaluate(() => { portalDeadline = Date.now() + 41000; renderPortalTimer(); });
        ok((await page.locator('#portalTimer').getAttribute('class')).includes('soon'),
           'последняя минута — янтарём');

        // Худший случай: самый узкий телефон и самое длинное слово. Прибор молчит по-настоящему,
        // страница доходит до «Reconnecting…» своим путём, а не подставленным классом.
        await page.setViewportSize({ width: 320, height: 800 });
        await page.request.get(base + '/mock?down=1');
        await page.waitForSelector('#conn.hword', { timeout: 12000 });
        h = await hdrFit();
        ok(h.past.length === 0,
           'на 320 px без связи ничего не за краем' + (h.past.length ? ': ' + h.past.join(', ') : ''));
        ok(h.wordShown && !h.timer,
           'говорит ровно один элемент: слово есть, отсчёт убран (он всё равно замер бы)');
        ok(h.heights.length === 1, 'и рост по-прежнему один (' + h.heights.join('/') + ')');
        await page.request.get(base + '/mock?down=0');
        await page.waitForSelector('#conn.hdot', { timeout: 12000 });
        await page.setViewportSize({ width: 375, height: 812 });
        await sleep(400);

        area('закрытие консоли из шапки');
        // С чистой страницы: перед этим отрабатывает мастер, и его подложка накрывает шапку.
        // Перезагрузка именно ЗДЕСЬ, а не после закрытия — после закрытия она заставляет Chromium
        // писать своё предупреждение про beforeunload.
        await page.reload({ waitUntil: 'networkidle' });
        await sleep(700);
        // До 2026-09-30 выйти можно было только с кнопки прибора или дождавшись таймаута, а все
        // три минуты ожидания радио остаётся поднятым — самое дорогое, что делает прибор.
        ok(await page.locator('#closeBtn').isVisible(), 'крестик есть в шапке');
        let closePosts = 0;
        page.on('request', r => { if (r.method() === 'POST' && r.url().includes('/api/close')) closePosts++; });
        await page.click('#closeBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        const askText = await page.locator('#modal').innerText();
        ok(/3 seconds/i.test(askText),
           'вопрос называет путь обратно: удержать кнопку прибора 3 секунды');
        await page.click('#mCancel');
        await page.waitForSelector('#modal.show', { state: 'hidden', timeout: 4000 }).catch(() => {});
        await sleep(300);
        ok(closePosts === 0, 'отмена ничего не закрывает');
        await page.click('#closeBtn');
        await page.waitForSelector('#modal.show', { timeout: 4000 });
        await page.click('#mOk');
        await sleep(600);
        ok(closePosts === 1, 'подтверждение отправляет ровно один запрос');
        ok((await page.locator('#connt').innerText()).trim() === 'Closed',
           'страница сразу говорит, что соединение закрыто');
        ok(!(await page.locator('#closeBtn').isVisible()),
           'и прячет крестик: кнопка, которой нечего закрывать, хуже её отсутствия');
        // Ничего после этого не делаем: страница закрыта, и перезагрузка здесь заставляла Chromium
        // писать в консоль своё предупреждение про beforeunload — ошибку теста, а не прошивки.

        area('исключения');
        ok(errors.length === 0, 'ни одной ошибки в скрипте'
           + (errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''));
    } catch (e) {
        ok(false, 'тест оборвался: ' + e.message);
    } finally {
        await browser.close();
        mock.kill();
    }

    console.log('браузер          проверок %d, провалов %d', checks, fails);
    process.exit(fails ? 1 : 0);
})();
