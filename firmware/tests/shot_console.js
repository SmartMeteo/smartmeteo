// Снимок карточки консоли для гида на сайте — настоящей консолью, а не рисованием.
//
//     node tests/shot_console.js <id элемента> <куда.png>
//     node tests/shot_console.js stripCard /tmp/c_strip.png
//
// Берёт тот же подставной прибор (tests/mock.py) и тот же браузер, что t_browser, и снимает один
// элемент живой страницы. Рисованный от руки снимок разошёлся бы с консолью в первый же день —
// а он лежит на публичной странице, где расхождение заметит только покупатель.
//
// Прибор мока по умолчанию в режиме без погоды, и карточка полосы честно пишет об этом. Для гида
// нужен обычный случай, поэтому режим и город задаются ниже перед съёмкой: иначе читатель решит,
// что плашка «в этом режиме погоды нет» — нормальное состояние.
//
// Готовый PNG уменьшается и переводится в webp под остальные снимки гида (716 px по ширине).
// разошёлся бы с консолью в первый же день — а этот лежит на публичной странице.
const { chromium } = require('playwright');
const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const HERE = __dirname;
const PASS = 'Qw3rty!@';

const freePort = () => new Promise(res => {
  const s = net.createServer();
  s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
});

(async () => {
  const port = await freePort();
  const mock = spawn('python3', [path.join(HERE, 'mock.py'), String(port)], { stdio: ['ignore','pipe','pipe'] });
  await new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('mock не поднялся')), 10000);
    mock.stdout.on('data', d => { if (String(d).includes('mock на')) { clearTimeout(t); res(); } });
  });
  const base = 'http://127.0.0.1:' + port;
  const b = await chromium.launch();
  const page = await b.newPage({ viewport: { width: 752, height: 1400 }, deviceScaleFactor: 2 });
  await page.goto(base + '/', { waitUntil: 'domcontentloaded' });
  await page.fill('#siPass', PASS);
  await page.click('#siBtn');
  await page.waitForSelector('#signin', { state: 'hidden', timeout: 8000 });
  await page.evaluate(() => go('set'));
  await page.waitForTimeout(800);
  // Прибор в подставном режиме без погоды, и карточка честно пишет об этом. Для гида нужен
  // обычный случай: Wi-Fi, город задан, погода есть — иначе читатель решит, что так и должно быть.
  await page.evaluate(() => {
    ST.data.cfg.wl_mode = 0; ST.data.cfg.profile = 1; ST.data.cfg.city = 'Darmstadt, DE';
    ST.data.cfg.wx_lat = 49.8728; ST.data.cfg.wx_lon = 8.6512;
    ST.strip = [2, 3, 4];
    renderStrip();
  });
  await page.waitForTimeout(300);
  const target = process.argv[2] || 'stripCard';
  const out = process.argv[3] || '/tmp/shot.png';
  await page.locator('#' + target).screenshot({ path: out });
  console.log('снято: ' + out);
  await b.close(); mock.kill();
})().catch(e => { console.error('ОШИБКА: ' + e.message); process.exit(1); });
