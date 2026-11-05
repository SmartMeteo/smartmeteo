// Все снимки консоли для гида на сайте — одной командой, с настоящей консоли.
//
//     node tests/shot_guide.js <каталог>          PNG в каталог; webp делает shot_guide_webp.py
//
// До 2026-10-03 снимки делались по одному и руками, и через неделю половина из них показывала
// консоль, которой уже нет: мастер из девяти шагов с туром, прежнюю калибровку CO₂, прежнюю
// карточку батареи и старый знак в шапке. Гид лежит на публичной странице, и расхождение там
// замечает только покупатель — поэтому снимается всё сразу и тем же кодом, что отдаёт прибор.
//
// Прибор подставной (tests/mock.py) в режиме витрины (show=1): город задан, газ откалиброван,
// батарея измерена. Телефонная ширина 390 px при плотности 2 — как у прежних снимков.
const { chromium } = require('playwright');
const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const fs = require('fs');
const HERE = __dirname;
const PASS = 'Qw3rty!@';
const OUT = process.argv[2] || '/tmp/guide_shots';
fs.mkdirSync(OUT, { recursive: true });

const freePort = () => new Promise(res => {
  const s = net.createServer();
  s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
});

(async () => {
  const port = await freePort();
  const mock = spawn('python3', [path.join(HERE, 'mock.py'), String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
  await new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('mock не поднялся')), 10000);
    mock.stdout.on('data', d => { if (String(d).includes('mock на')) { clearTimeout(t); res(); } });
  });
  const base = 'http://127.0.0.1:' + port;
  const setMock = q => fetch(base + '/mock?' + q).then(r => r.json());
  await setMock('show=1&wl=0&fab=0&mode=STA&bat=measured&setup=0&remain=170');

  const b = await chromium.launch();
  const ctx = await b.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2,
                                   colorScheme: 'light' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  const shot = async name => { await page.waitForTimeout(350); await page.screenshot({ path: path.join(OUT, name + '.png') }); console.log('  ' + name); };
  const go = async tab => { await page.evaluate(t => { go(t); window.scrollTo(0, 0); }, tab); await page.waitForTimeout(600); };
  // A card is found by what it says, not by an id most of them do not have: a section label
  // names the card after it, an <h3> sits inside it.
  const card = async (name, tab, text) => {
    await go(tab);
    const h = await page.evaluateHandle(t => {
      for (const e of document.querySelectorAll('.sectlabel')) if (e.textContent.trim() === t) return e.nextElementSibling;
      for (const e of document.querySelectorAll('h3')) if (e.textContent.trim() === t) return e.closest('.card');
      // a folded card (the security log) is named by its <summary>; open it for the picture
      for (const e of document.querySelectorAll('summary')) if (e.textContent.trim() === t) {
        e.parentElement.open = true; e.parentElement.dispatchEvent(new Event('toggle')); return e.closest('.card'); }
      return null;
    }, text);
    const el = h.asElement();
    if (!el) throw new Error('нет карточки «' + text + '» на вкладке ' + tab);
    await el.scrollIntoViewIfNeeded(); await page.waitForTimeout(300);
    await el.screenshot({ path: path.join(OUT, name + '.png') });
    console.log('  ' + name);
  };

  await page.goto(base + '/', { waitUntil: 'domcontentloaded' });
  await page.waitForSelector('#siPass');
  await shot('signin');
  await page.fill('#siPass', PASS);
  await page.click('#siBtn');
  await page.waitForSelector('#signin', { state: 'hidden', timeout: 8000 });
  await page.waitForTimeout(1500);           // the history arrives after the first status

  // Whole tabs, the way a phone shows them
  await go('dash'); await shot('dash');
  await go('mode'); await shot('tab_mode');
  await go('net');  await shot('tab_net');
  await page.evaluate(() => { selectConn(2); window.scrollTo(0, 0); });
  await shot('tab_net_matter');
  await go('sys');  await shot('tab_sys');

  // Cards — without the fixed bottom bar, which otherwise lands across whatever card sits low on the screen
  await page.addStyleTag({ content: '#nav{display:none!important}' });
  for (const [n, t, x] of [
    ['c_aqdetails', 'sys', 'Air-quality details'], ['c_history', 'dash', 'History'],
    ['c_battery_dash', 'dash', 'Battery'], ['c_gascal', 'dash', 'Gas sensor calibration'],
    ['c_running', 'mode', 'Running now'],
    ['c_setclock', 'set', 'Clock'], ['c_strip', 'set', 'Bottom strip'], ['c_units', 'set', 'Units'],
    ['c_weather', 'set', 'Outdoor weather'], ['c_notif', 'set', 'Notifications'],
    ['c_telegram', 'set', 'Telegram'], ['c_co2cal', 'set', 'CO₂ calibration'],
    ['c_tempcal', 'set', 'Match a thermometer'],
    ['c_updates', 'sys', 'Firmware updates'], ['c_seclog', 'sys', 'Security log'],
    ['c_password', 'set', 'Device password'], ['c_maint', 'sys', 'Maintenance']]) await card(n, t, x);
  // Mode tab cards without a heading of their own
  await go('mode');
  for (const [n, id] of [['c_clock', 'lcCard'], ['c_night', 'nmCard']]) {
    const el = page.locator('#' + id); await el.scrollIntoViewIfNeeded();
    await el.screenshot({ path: path.join(OUT, n + '.png') }); console.log('  ' + n);
  }
  // The broker card is shown once Home Assistant is the chosen mode
  await setMock('wl=1'); await page.evaluate(() => load && load()); await page.waitForTimeout(800);
  await go('net'); await page.evaluate(() => selectConn(1)); await page.waitForTimeout(500);
  { const h = await page.evaluateHandle(() => { const i = document.getElementById('mqHost');
      return i ? i.closest('.card') : null; });
    const el = h.asElement(); if (!el) throw new Error('нет карточки брокера');
    await el.scrollIntoViewIfNeeded(); await el.screenshot({ path: path.join(OUT, 'c_mqtt.png') }); console.log('  c_mqtt'); }
  await setMock('wl=0'); await page.evaluate(() => load && load()); await page.waitForTimeout(800);

  await page.addStyleTag({ content: '#nav{display:flex!important}' });
  // The two questions in Maintenance
  for (const [n, id] of [['m_factory', 'factoryBtn'], ['m_off', 'offBtn']]) {
    await go('sys');
    await page.locator('#' + id).scrollIntoViewIfNeeded(); await page.click('#' + id);
    await page.waitForTimeout(500); await shot(n);
    await page.keyboard.press('Escape'); await page.evaluate(() => { const c = [...document.querySelectorAll('.modal button')]
      .find(x => /Cancel/.test(x.textContent)); if (c) c.click(); }); await page.waitForTimeout(300);
  }

  // The first-run wizard, as a new device on its own Wi-Fi sees it
  await setMock('mode=AP&ssid=&nets=&conn=0&show=0');
  await page.evaluate(() => load && load()); await page.waitForTimeout(800);
  await page.evaluate(() => wizStart(false)); await page.waitForTimeout(600);
  const step = () => page.evaluate(() => document.getElementById('wizTitle').textContent.trim());
  const next = async () => { await page.click('#wizNext'); await page.waitForTimeout(900); };
  const seen = [];
  // Step one: the scan, then a network picked
  await page.evaluate(() => { const s = [...document.querySelectorAll('#wizBody button')].find(x => /Scan/.test(x.textContent)); if (s) s.click(); });
  await page.waitForTimeout(1200); await shot('wz_scan'); seen.push('wz_scan ' + await step());
  await page.evaluate(() => { const n = [...document.querySelectorAll('#wizBody *')].find(x => x.children.length < 3 && x.textContent.trim() === 'HomeWiFi'); if (n) n.click(); });
  await page.waitForTimeout(400);
  const pw = page.locator('#wizBody input[type=password]');
  if (await pw.count()) await pw.first().fill('correct-horse');
  await shot('wz_net'); seen.push('wz_net ' + await step());
  const names = { 'Set the clock': 'wz_clock', 'Outdoor weather': 'wz_weather', 'Smart home': 'wz_hub',
                  'The button': 'wz_button', 'Ready to finish': 'wz_done' };
  for (let i = 0; i < 8; i++) {
    // Wait for the step to change, not a fixed time: the Wi-Fi step joins the network first.
    const was = await step(); await next();
    for (let k = 0; k < 40 && (await step()) === was; k++) await page.waitForTimeout(500);
    await page.waitForTimeout(500);
    const t = await step(); const n = names[t]; console.log('    шаг: ' + t);
    if (!n) { seen.push('? ' + t); continue; }
    if (n === 'wz_done') { const nm = page.locator('#wizBody input:not([type=password]):not([type=checkbox])'); if (await nm.count()) await nm.first().fill('Living room'); }
    if (!seen.some(s => s.startsWith(n + ' '))) { await shot(n); seen.push(n + ' ' + t); }
    if (n === 'wz_done') break;
  }
  console.log('мастер: ' + seen.join(' | '));
  if (errors.length) console.log('ИСКЛЮЧЕНИЯ: ' + errors.join(' || '));
  await b.close(); mock.kill();
})().catch(e => { console.error('ОШИБКА: ' + e.message); process.exit(1); });
