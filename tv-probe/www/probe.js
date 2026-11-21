// TV'nin kendi donaniminda calisan olcum. Eski Tizen tarayicisi icin ES5.
(function () {
  var ROUNDS = 12, IDLE_MS = 4000;
  var T0 = Date.now();
  var statusEl = document.getElementById('status');
  var box = document.getElementById('box');
  var raf = window.requestAnimationFrame || window.webkitRequestAnimationFrame ||
    function (f) { return setTimeout(f, 16); };

  function now() { return (window.performance && performance.now) ? performance.now() : Date.now(); }
  function r1(v) { return Math.round(v * 10) / 10; }
  function say(s) { statusEl.textContent = s; }

  function post(obj, cb) {
    try {
      var x = new XMLHttpRequest();
      x.open('POST', '/r', true);
      x.setRequestHeader('Content-Type', 'text/plain');
      x.onreadystatechange = function () { if (x.readyState === 4 && cb) { cb(); } };
      obj.t = r1((Date.now() - T0) / 1000);
      x.send(JSON.stringify(obj));
    } catch (e) { if (cb) { cb(); } }
  }
  window.onerror = function (m, s, l) { post({ type: 'error', msg: String(m), line: l }); };

  function heap() {
    try {
      var m = performance.memory;
      return { limitMB: r1(m.jsHeapSizeLimit / 1048576), totalMB: r1(m.totalJSHeapSize / 1048576), usedMB: r1(m.usedJSHeapSize / 1048576) };
    } catch (e) { return null; }
  }

  function glInfo() {
    try {
      var c = document.createElement('canvas');
      var g = c.getContext('webgl') || c.getContext('experimental-webgl');
      if (!g) { return null; }
      var e = g.getExtension('WEBGL_debug_renderer_info');
      return {
        vendor: e ? g.getParameter(e.UNMASKED_VENDOR_WEBGL) : g.getParameter(g.VENDOR),
        renderer: e ? g.getParameter(e.UNMASKED_RENDERER_WEBGL) : g.getParameter(g.RENDERER),
        version: g.getParameter(g.VERSION),
        maxTex: g.getParameter(g.MAX_TEXTURE_SIZE)
      };
    } catch (err) { return { err: String(err) }; }
  }

  function info() {
    var n = navigator, c = n.connection || n.webkitConnection || null;
    return {
      type: 'info', ua: n.userAgent, platform: n.platform, cores: n.hardwareConcurrency || null,
      deviceMemory: n.deviceMemory || null, lang: n.language,
      screen: screen.width + 'x' + screen.height, inner: window.innerWidth + 'x' + window.innerHeight,
      dpr: window.devicePixelRatio, tizen: typeof window.tizen, webapis: typeof window.webapis,
      heap: heap(), gl: glInfo(),
      conn: c ? { type: c.type, effectiveType: c.effectiveType, downlink: c.downlink } : null,
      hasWorker: !!window.Worker, hasPerfNow: !!(window.performance && performance.now)
    };
  }

  function storage(cb) {
    var out = { type: 'storage' }, pending = 0, fired = false;
    function fin() { if (!fired && pending <= 0) { fired = true; post(out, cb); } }
    try {
      if (navigator.webkitTemporaryStorage) {
        pending++;
        navigator.webkitTemporaryStorage.queryUsageAndQuota(function (u, q) {
          out.tempUsedMB = r1(u / 1048576); out.tempQuotaMB = r1(q / 1048576); pending--; fin();
        }, function () { pending--; fin(); });
      }
      if (navigator.storage && navigator.storage.estimate) {
        pending++;
        navigator.storage.estimate().then(function (e) {
          out.estUsedMB = r1(e.usage / 1048576); out.estQuotaMB = r1(e.quota / 1048576); pending--; fin();
        }, function () { pending--; fin(); });
      }
    } catch (e) { out.err = String(e); }
    setTimeout(function () { pending = 0; fin(); }, 4000);
    fin();
  }

  // --- sabit is yukleri ---------------------------------------------------
  function cpuBench() {
    var t = now(), x = 0, i, a = [], s = 12345;
    for (i = 0; i < 3000000; i++) { x = (x + Math.sqrt(i) * Math.sin(i)) % 1000003; }
    for (i = 0; i < 60000; i++) { s = (s * 1103515245 + 12345) & 0x7fffffff; a.push(s); }
    a.sort(function (p, q) { return p - q; });
    return r1(now() - t);
  }

  function domBench() {
    var t = now(), host = document.createElement('div'), i, d, h = 0;
    host.style.cssText = 'position:absolute;left:-9999px;top:0;width:1200px';
    document.body.appendChild(host);
    for (i = 0; i < 1500; i++) {
      d = document.createElement('div');
      d.style.cssText = 'float:left;width:' + (40 + i % 60) + 'px;padding:3px;border:1px solid #333;font-size:' + (10 + i % 8) + 'px';
      d.textContent = 'oge ' + i;
      host.appendChild(d);
    }
    h += host.offsetHeight;
    for (i = 0; i < host.childNodes.length; i += 3) { host.childNodes[i].style.width = '90px'; }
    h += host.offsetHeight;
    document.body.removeChild(host);
    return r1(now() - t);
  }

  function canvasBench() {
    var t = now(), c = document.createElement('canvas'), g, i;
    c.width = 1280; c.height = 720;
    g = c.getContext('2d');
    for (i = 0; i < 3000; i++) {
      g.fillStyle = 'rgba(' + (i % 255) + ',' + ((i * 7) % 255) + ',' + ((i * 13) % 255) + ',0.6)';
      g.fillRect((i * 37) % 1200, (i * 53) % 680, 80, 40);
    }
    g.font = '24px sans-serif';
    for (i = 0; i < 300; i++) { g.fillText('Olcum ' + i, (i * 41) % 1100, 30 + (i * 29) % 680); }
    g.getImageData(0, 0, 1, 1); // cizimi zorla bitir
    return r1(now() - t);
  }

  // --- bosta pencere: zamanlayici sapmasi + kare hizi ---------------------
  function idleWindow(dur, cb) {
    var start = now(), last = start, ticks = 0, maxDrift = 0, sumDrift = 0, stalls = 0;
    var frames = 0, maxGap = 0, lastF = 0, slow = 0, run = true;
    var iv = setInterval(function () {
      var n = now(), d = n - last - 50; last = n;
      if (d < 0) { d = 0; }
      ticks++; sumDrift += d;
      if (d > maxDrift) { maxDrift = d; }
      if (d > 100) { stalls++; }
    }, 50);
    function fr() {
      if (!run) { return; }
      var n = now(), g;
      if (lastF) { g = n - lastF; if (g > maxGap) { maxGap = g; } if (g > 34) { slow++; } }
      lastF = n; frames++;
      box.style.webkitTransform = box.style.transform = 'translateX(' + ((frames % 300) * 3) + 'px)';
      raf(fr);
    }
    raf(fr);
    setTimeout(function () {
      run = false; clearInterval(iv);
      var el = now() - start;
      cb({ fps: r1(frames * 1000 / el), maxFrameGapMs: r1(maxGap), slowFrames: slow,
           timerMaxDriftMs: r1(maxDrift), timerMeanDriftMs: r1(sumDrift / (ticks || 1)), stalls: stalls });
    }, dur);
  }

  // --- cekirdek olcegi ----------------------------------------------------
  function workerTest(n, cb) {
    var res = [], ws = [], left = 0, fired = false, i, to;
    function fin() {
      if (fired) { return; }
      fired = true; clearTimeout(to);
      for (var j = 0; j < ws.length; j++) { try { ws[j].terminate(); } catch (e) {} }
      cb(res);
    }
    if (!window.Worker) { cb(null); return; }
    function mk() {
      var w;
      try { w = new Worker('/worker.js'); } catch (e) { return; }
      left++; ws.push(w);
      w.onmessage = function (ev) { res.push(ev.data); if (--left === 0) { fin(); } };
      w.onerror = function () { if (--left === 0) { fin(); } };
    }
    for (i = 0; i < n; i++) { mk(); }
    if (!ws.length) { cb(null); return; }
    to = setTimeout(fin, 60000);
    // Hepsi yuklensin diye kisa bir bekleme, sonra ayni anda baslat.
    setTimeout(function () { for (var k = 0; k < ws.length; k++) { ws[k].postMessage('go'); } }, 1500);
  }

  function workers(label, cb) {
    var n = navigator.hardwareConcurrency || 4;
    say('Cekirdek testi (' + label + ')...');
    workerTest(1, function (one) {
      workerTest(n, function (many) {
        post({ type: 'workers', when: label, n: n, singleMs: one, parallelMs: many }, cb);
      });
    });
  }

  function rounds(k) {
    if (k >= ROUNDS) {
      workers('son', function () {
        post({ type: 'done' }, function () { say('Bitti. Tarayiciyi kapatabilirsin.'); });
      });
      return;
    }
    say('Olcum ' + (k + 1) + ' / ' + ROUNDS);
    idleWindow(IDLE_MS, function (idle) {
      var cpu = cpuBench(), dom = domBench(), cv = canvasBench();
      post({ type: 'round', k: k, idle: idle, cpuMs: cpu, domMs: dom, canvasMs: cv, heap: heap() }, function () {
        rounds(k + 1);
      });
    });
  }

  say('Basliyor...');
  post(info(), function () {
    storage(function () {
      workers('bas', function () { rounds(0); });
    });
  });
})();
