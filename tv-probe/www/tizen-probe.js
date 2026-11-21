// TV'ye kurulan teshis uygulamasinin icinde calisir (Tizen web API'leri). ES5.
(function () {
  var SERVER = window.__SERVER, T0 = Date.now();
  var TICKS = 30, TICK_MS = 3000;
  var statusEl = document.getElementById('status');

  function say(s) { statusEl.textContent = s; }
  function r1(v) { return Math.round(v * 10) / 10; }
  function mb(v) { return (typeof v === 'number') ? r1(v / 1048576) : v; }
  function errStr(e) { return e ? ((e.name || '') + ' ' + (e.message || String(e))) : 'bilinmeyen hata'; }

  function post(obj, cb) {
    try {
      var x = new XMLHttpRequest();
      x.open('POST', SERVER + '/r', true);
      x.setRequestHeader('Content-Type', 'text/plain');
      x.onreadystatechange = function () { if (x.readyState === 4 && cb) { var f = cb; cb = null; f(); } };
      obj.t = r1((Date.now() - T0) / 1000);
      x.send(JSON.stringify(obj));
    } catch (e) { if (cb) { cb(); } }
  }
  window.onerror = function (m, s, l) { post({ type: 'error', msg: String(m), line: l }); };

  // Platform nesnelerini duz JSON'a cevir (ozellikler prototipte olabilir).
  function plain(o, depth) {
    var k, out, i;
    if (o === null || o === undefined) { return o; }
    if (typeof o === 'number' || typeof o === 'string' || typeof o === 'boolean') { return o; }
    if (typeof o === 'function') { return undefined; }
    if (o instanceof Date) { return o.toISOString(); }
    if (depth <= 0) { return String(o); }
    if (Object.prototype.toString.call(o) === '[object Array]') {
      out = [];
      for (i = 0; i < o.length; i++) { out.push(plain(o[i], depth - 1)); }
      return out;
    }
    out = {};
    for (k in o) {
      try { if (typeof o[k] !== 'function') { out[k] = plain(o[k], depth - 1); } } catch (e) { out[k] = 'ERR ' + errStr(e); }
    }
    return out;
  }

  function tryCall(fn) { try { return fn(); } catch (e) { return 'ERR ' + errStr(e); } }

  function getProp(name, cb) {
    var done = false;
    function fin(v) { if (!done) { done = true; cb(v); } }
    try {
      tizen.systeminfo.getPropertyValue(name, function (v) { fin(plain(v, 4)); }, function (e) { fin('ERR ' + errStr(e)); });
    } catch (e) { fin('ERR ' + errStr(e)); }
    setTimeout(function () { fin('ERR zaman asimi'); }, 5000);
  }

  // --- 1. kimlik ve yetenekler -------------------------------------------
  function info(cb) {
    var caps = {}, keys = [
      'http://tizen.org/feature/platform.version', 'http://tizen.org/feature/platform.web.api.version',
      'http://tizen.org/feature/platform.native.api.version', 'http://tizen.org/feature/platform.core.cpu.arch',
      'http://tizen.org/system/platform.processor', 'http://tizen.org/system/platform.name',
      'http://tizen.org/system/model_name', 'http://tizen.org/system/manufacturer',
      'http://tizen.org/system/build.string', 'http://tizen.org/system/build.date',
      'http://tizen.org/system/build.time', 'http://tizen.org/system/build.id',
      'http://tizen.org/system/build.release', 'http://tizen.org/system/build.type',
      'http://tizen.org/system/build.variant', 'http://tizen.org/feature/profile'
    ], i, pi = {}, fns, w = window.webapis;
    for (i = 0; i < keys.length; i++) {
      (function (k) { caps[k.replace('http://tizen.org/', '')] = tryCall(function () { return tizen.systeminfo.getCapability(k); }); })(keys[i]);
    }
    if (w && w.productinfo) {
      fns = ['getFirmware', 'getModel', 'getModelCode', 'getRealModel', 'getVersion', 'getSmartTVServerVersion',
             'getSmartTVServerType', 'getLocalSet', 'getDuid', 'isUdPanelSupported', 'is8KPanelSupported'];
      for (i = 0; i < fns.length; i++) {
        (function (f) { pi[f] = tryCall(function () { return w.productinfo[f](); }); })(fns[i]);
      }
    }
    post({
      type: 'info', ua: navigator.userAgent, tizen: typeof window.tizen, webapis: typeof w,
      cores: navigator.hardwareConcurrency || null, caps: caps, productinfo: pi,
      totalMemMB: tryCall(function () { return mb(tizen.systeminfo.getTotalMemory()); }),
      availMemMB: tryCall(function () { return mb(tizen.systeminfo.getAvailableMemory()); }),
      appId: tryCall(function () { return tizen.application.getCurrentApplication().appInfo.id; })
    }, cb);
  }

  // --- 2. sistem ozellikleri ---------------------------------------------
  function props(cb) {
    var names = ['BUILD', 'CPU', 'MEMORY', 'STORAGE', 'DISPLAY', 'NETWORK', 'ETHERNET_NETWORK', 'WIFI_NETWORK', 'LOCALE', 'PERIPHERAL'];
    function next(i) {
      if (i >= names.length) { cb(); return; }
      getProp(names[i], function (v) { post({ type: 'prop', name: names[i], value: v }, function () { next(i + 1); }); });
    }
    next(0);
  }

  function storages(cb) {
    try {
      tizen.filesystem.listStorages(function (st) { post({ type: 'storages', list: plain(st, 3) }, cb); },
        function (e) { post({ type: 'storages', err: errStr(e) }, cb); });
    } catch (e) { post({ type: 'storages', err: errStr(e) }, cb); }
  }

  // --- 3. calisan ve kurulu uygulamalar ----------------------------------
  function running(cb) {
    try {
      tizen.application.getAppsContext(function (ctx) {
        var ids = [], i;
        for (i = 0; i < ctx.length; i++) { ids.push(ctx[i].appId); }
        cb(ids);
      }, function (e) { cb('ERR ' + errStr(e)); });
    } catch (e) { cb('ERR ' + errStr(e)); }
  }

  function packages(cb) {
    try {
      tizen.package.getPackagesInfo(function (pk) {
        var list = [], i, p;
        for (i = 0; i < pk.length; i++) {
          p = pk[i];
          list.push({ id: p.id, name: p.name, version: p.version, totalMB: mb(p.totalSize), dataMB: mb(p.dataSize),
                      installed: plain(p.installDate, 1), modified: plain(p.lastModified, 1) });
        }
        post({ type: 'packages', list: list }, cb);
      }, function (e) { post({ type: 'packages', err: errStr(e) }, cb); });
    } catch (e) { post({ type: 'packages', err: errStr(e) }, cb); }
  }

  // --- 4. zaman serisi: bos bellek + islemci yuku -------------------------
  function ticks(k) {
    if (k >= TICKS) { post({ type: 'done' }, function () { say('Bitti. Geri tuşuyla çıkabilirsin.'); }); return; }
    say('Ölçüm ' + (k + 1) + ' / ' + TICKS);
    getProp('CPU', function (cpu) {
      running(function (ids) {
        post({
          type: 'tick', k: k, cpuLoad: (cpu && typeof cpu.load === 'number') ? r1(cpu.load * 100) : cpu,
          availMemMB: tryCall(function () { return mb(tizen.systeminfo.getAvailableMemory()); }),
          running: (ids && ids.length !== undefined && typeof ids !== 'string') ? ids.length : ids
        }, function () { setTimeout(function () { ticks(k + 1); }, TICK_MS); });
      });
    });
  }

  // --- izleme modu: baska uygulamalar acilirken arka planda bellek/islemci kaydi ---
  function watch() {
    var end = Date.now() + (CFG.seconds || 240) * 1000, k = 0;
    function step() {
      if (Date.now() > end) { post({ type: 'done' }, function () { say('Bitti. Geri tuşuyla çıkabilirsin.'); }); return; }
      getProp('CPU', function (cpu) {
        getProp('MEMORY', function (mem) {
          running(function (ids) {
            post({
              type: 'w', k: k++, cpuLoad: (cpu && typeof cpu.load === 'number') ? r1(cpu.load * 100) : cpu,
              availMemMB: tryCall(function () { return mb(tizen.systeminfo.getAvailableMemory()); }),
              mem: (mem && mem.status) ? mem.status : mem, running: ids, hidden: !!document.hidden
            }, function () { setTimeout(step, CFG.interval || 2000); });
          });
        });
      });
    }
    step();
  }

  var CFG = window.__CFG || {};
  if (typeof window.tizen === 'undefined') {
    say('Tizen API yok');
    post({ type: 'error', msg: 'tizen nesnesi yok' });
    return;
  }
  if (CFG.watch) {
    say('İzleme modu: uygulamalar sırayla açılacak...');
    watch();
    return;
  }
  say('Sistem bilgisi okunuyor...');
  info(function () {
    props(function () {
      storages(function () {
        running(function (ids) {
          post({ type: 'running', apps: ids }, function () {
            packages(function () { ticks(0); });
          });
        });
      });
    });
  });
})();
