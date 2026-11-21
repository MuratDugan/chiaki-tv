// chiaki-tv — TV arayuzu. Tizen 3.0 (Chrome 47) icin ES5.
// Yerel modulle JSON metin mesajlariyla konusur: {cmd:...} gider, {ev:...} gelir.
(function () {
  'use strict';
  var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, OK: 13, BACK: 10009, RED: 403, GREEN: 404, YELLOW: 405,
              BLUE: 406, INFO: 457, EXIT: 10182 };
  var STREAM_KEYS = {};
  STREAM_KEYS[KEY.LEFT] = 'left'; STREAM_KEYS[KEY.UP] = 'up'; STREAM_KEYS[KEY.RIGHT] = 'right';
  STREAM_KEYS[KEY.DOWN] = 'down'; STREAM_KEYS[KEY.OK] = 'ok'; STREAM_KEYS[KEY.RED] = 'red';
  STREAM_KEYS[KEY.GREEN] = 'green'; STREAM_KEYS[KEY.YELLOW] = 'yellow'; STREAM_KEYS[KEY.BLUE] = 'blue';
  var RESOLUTIONS = [540, 720, 1080], FPS = [30, 60];
  var CONFIG_KEY = 'chiakitv.config';
  var isTizen = typeof window.tizen !== 'undefined';

  function $(id) { return document.getElementById(id); }
  function show(el, on) { el.className = el.className.replace(/\s*hidden/g, '') + (on ? '' : ' hidden'); }

  var S = null;             // modulden gelen son durum
  var nacl = null;
  var gotReady = false;
  var streaming = false;
  var focusIndex = 0, focusables = [], userMoved = false;
  var consolesKey = '';
  var held = {};            // akista basili tutulan tuslar
  var backTimer = null, exitOpen = false, fatalOpen = false;
  var down = {};            // su an fiziksel olarak basili tuslar (tekrar olaylarini ayirmak icin)
  var exitNeedsBackUp = false;  // cikis sorusu uzun basisla acildi: Geri birakilana kadar Geri'yi yok say
  var lastIp = '';
  var pinValue = '', pinFocus = 0, pinKeys = [], pinShown = false;
  var hudTimer = null;
  var menuOpen = false, menuFocus = 0, tipTimer = null, lastStats = null, ignoreUntilUp = {};
  var MENU_IDS = ['m-resume', 'm-disconnect', 'm-sleep'];

  // ---- dil -------------------------------------------------------------------
  // Tek dil ayari modulde durur (durumdaki 'lang'). Buradaki kayit yalnizca acilista yanlis dil gorunmesin diye.
  var LANGS = ['en', 'tr'], LANG_NAMES = { en: 'English', tr: 'Türkçe' };
  var LANG_KEY = 'chiakitv.lang';
  var I18N = {
    en: {
      brand_sub: 'PS5 Remote Play',
      pad_none: 'No controller',
      pad_name: 'Controller: {0}',
      ip_wait: 'getting address',
      ip_none: 'no network',
      url_wait: 'waiting for network',
      setup_head: 'SET UP FROM YOUR PHONE',
      setup_open: 'Open on your phone or computer',
      setup_code: 'and enter this code',
      psn_on: 'Signed in to PSN',
      psn_off: 'Not signed in to PSN',
      consoles_head: 'CONSOLES',
      settings_head: 'SETTINGS',
      opt_res: 'Resolution',
      opt_fps: 'Frame rate',
      opt_lang: 'Language',
      opt_list: 'Console list',
      opt_refresh: 'Refresh',
      rp_on: 'Remote Play on',
      rp_off: 'Remote Play off (turn it on in console settings)',
      connect_go: 'Connect ›',
      empty_login: 'Not signed in to PSN yet. Open the address on the left on your phone and follow the steps.',
      empty_none: 'No PS5 found for this account. Make sure the console is on or in rest mode, then refresh the list.',
      foot_select: 'Select',
      foot_move: 'Navigate',
      foot_change: 'Change setting',
      key_back: 'Back',
      exit: 'Exit',
      hud_live: 'LIVE',
      hud_pad_on: 'controller connected',
      hud_pad_none: 'no controller',
      hud_exit: 'Hold Back to exit',
      tip_pre: 'Press',
      tip_post: 'to open the stream menu',
      menu_connected: 'Connected to {0}',
      menu_low: 'low latency',
      st_decoded: 'decoded fps',
      st_fps: 'on screen fps',
      st_decode: 'decoder delay',
      st_screen: 'network to screen',
      st_rtt: 'round trip',
      st_lost: 'lost frames',
      m_resume: 'Back to game',
      m_disconnect: 'Disconnect',
      m_sleep: 'Disconnect and sleep',
      ms: '{0} ms',
      connecting: 'Connecting',
      cancel: 'Cancel',
      pin_title: 'Console login PIN',
      pin_sub: 'Enter the login PIN of the user on the console.',
      pin_wrong: 'Incorrect PIN. Enter it again.',
      pin_hint: 'You can also use the remote\'s number keys or the phone page.',
      pin_del: 'Delete',
      pin_ok: 'OK',
      exit_title: 'Disconnect?',
      exit_sub: 'The console stays on and the game keeps running.',
      disconnect: 'Disconnect',
      exit_stay: 'Back to game',
      fatal_title: 'The app core did not start',
      fatal_load: 'The module could not be loaded: {0}',
      fatal_unknown: 'unknown error',
      fatal_crash: 'The module closed unexpectedly (exit code {0})',
      fatal_step: ' — last step: {0}',
      fatal_timeout: 'The module did not start within 30 seconds.',
      retry: 'Retry'
    },
    tr: {
      brand_sub: 'PS5 Uzaktan Oynama',
      pad_none: 'Oyun kolu yok',
      pad_name: 'Kol: {0}',
      ip_wait: 'adres alınıyor',
      ip_none: 'ağ yok',
      url_wait: 'ağ bağlantısı bekleniyor',
      setup_head: 'TELEFONDAN KURULUM',
      setup_open: 'Telefonunda ya da bilgisayarında aç',
      setup_code: 've bu kodu gir',
      psn_on: 'PSN oturumu açık',
      psn_off: 'PSN oturumu açık değil',
      consoles_head: 'KONSOLLAR',
      settings_head: 'AYARLAR',
      opt_res: 'Çözünürlük',
      opt_fps: 'Kare hızı',
      opt_lang: 'Dil',
      opt_list: 'Konsol listesi',
      opt_refresh: 'Yenile',
      rp_on: 'Uzaktan Oynama açık',
      rp_off: 'Uzaktan Oynama kapalı (konsol ayarlarından aç)',
      connect_go: 'Bağlan ›',
      empty_login: 'Henüz PSN girişi yapılmadı. Soldaki adresi telefonunda açıp adımları izle.',
      empty_none: 'Bu hesaba bağlı PS5 bulunamadı. Konsolun açık ya da dinlenme modunda olduğundan emin olup listeyi yenile.',
      foot_select: 'Seç',
      foot_move: 'Gezin',
      foot_change: 'Ayarı değiştir',
      key_back: 'Geri',
      exit: 'Çıkış',
      hud_live: 'CANLI',
      hud_pad_on: 'kol bağlı',
      hud_pad_none: 'kol yok',
      hud_exit: 'Geri\'yi basılı tut: çıkış',
      tip_pre: 'Yayın menüsü için',
      tip_post: 'tuşlarına birlikte bas',
      menu_connected: '{0} konsoluna bağlı',
      menu_low: 'düşük gecikme',
      st_decoded: 'çözülen kare/sn',
      st_fps: 'ekrandaki kare/sn',
      st_decode: 'çözücü gecikmesi',
      st_screen: 'ağdan ekrana',
      st_rtt: 'gidiş-dönüş',
      st_lost: 'kayıp kare',
      m_resume: 'Oyuna dön',
      m_disconnect: 'Bağlantıyı kes',
      m_sleep: 'Kes ve konsolu uyut',
      ms: '{0} ms',
      connecting: 'Bağlanılıyor',
      cancel: 'Vazgeç',
      pin_title: 'Konsol giriş PIN\'i',
      pin_sub: 'Konsoldaki kullanıcının giriş PIN\'ini gir.',
      pin_wrong: 'PIN yanlış. Yeniden gir.',
      pin_hint: 'Kumandanın rakam tuşlarını ya da telefondaki sayfayı da kullanabilirsin.',
      pin_del: 'Sil',
      pin_ok: 'Tamam',
      exit_title: 'Bağlantı kesilsin mi?',
      exit_sub: 'Konsol açık kalır; oyun durmaz.',
      disconnect: 'Bağlantıyı kes',
      exit_stay: 'Oyuna dön',
      fatal_title: 'Uygulama çekirdeği çalışmadı',
      fatal_load: 'Modül yüklenemedi: {0}',
      fatal_unknown: 'bilinmeyen hata',
      fatal_crash: 'Modül beklenmedik biçimde kapandı (çıkış kodu {0})',
      fatal_step: ' — son adım: {0}',
      fatal_timeout: 'Modül 30 saniyede başlamadı.',
      retry: 'Yeniden dene'
    }
  };
  var lang = 'en';

  // t('anahtar', deger0, deger1...): metindeki {0}, {1} yerlerine degerler konur.
  function t(key) {
    var s = I18N[lang][key], i;
    if (s === undefined) { s = I18N.en[key]; }
    if (s === undefined) { return key; }
    for (i = 1; i < arguments.length; i++) { s = s.split('{' + (i - 1) + '}').join(String(arguments[i])); }
    return s;
  }
  function setT(el, key) { el.setAttribute('data-i18n', key); el.textContent = t(key); }

  // data-i18n tasiyan butun ogelerin metnini gecerli dile cevirir.
  function applyI18n() {
    var list = document.querySelectorAll('[data-i18n]'), i;
    document.documentElement.setAttribute('lang', lang);
    for (i = 0; i < list.length; i++) { list[i].textContent = t(list[i].getAttribute('data-i18n')); }
    $('val-lang').textContent = LANG_NAMES[lang];
  }
  // Bilinmeyen ya da eksik deger Ingilizce sayilir.
  function setLang(v) {
    v = LANGS.indexOf(v) >= 0 ? v : 'en';
    if (v === lang) { return; }
    lang = v;
    try { localStorage.setItem(LANG_KEY, lang); } catch (e) {}
    applyI18n();
  }

  // ---- yerlesim --------------------------------------------------------------
  function fit() {
    var k = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
    var st = $('stage').style;
    st.webkitTransform = st.transform = 'scale(' + k + ')';
    st.left = Math.round((window.innerWidth - 1920 * k) / 2) + 'px';
    st.top = Math.round((window.innerHeight - 1080 * k) / 2) + 'px';
  }
  window.addEventListener('resize', fit);

  // ---- modul -----------------------------------------------------------------
  function post(obj) {
    try { if (nacl && nacl.postMessage) { nacl.postMessage(JSON.stringify(obj)); } } catch (e) {}
  }

  var tail = [], crash = {};

  function fatal(text) {
    fatalOpen = true;
    $('fatal-text').textContent = text;
    show($('fatal'), true);
  }

  function getIp(cb) {
    var done = false;
    function fin(v) { if (!done) { done = true; cb(v || ''); } }
    if (!isTizen) { fin(''); return; }
    try {
      tizen.systeminfo.getPropertyValue('NETWORK', function (n) {
        var p = n.networkType === 'WIFI' ? 'WIFI_NETWORK' : 'ETHERNET_NETWORK';
        tizen.systeminfo.getPropertyValue(p, function (e) { fin(e.ipAddress); }, function () { fin(''); });
      }, function () { fin(''); });
    } catch (e) { fin(''); }
    setTimeout(function () { fin(''); }, 4000);
  }

  // Ag uygulama acildiktan sonra gelebilir: adres alinana kadar araliklarla yeniden sor.
  function watchIp() {
    getIp(function (ip) {
      if (ip && ip !== lastIp) { lastIp = ip; post({ cmd: 'ip', ip: ip }); }
      setTimeout(watchIp, ip ? 30000 : 3000);
    });
  }

  function sendInit() {
    getIp(function (ip) {
      var config = '';
      try { config = localStorage.getItem(CONFIG_KEY) || ''; } catch (e) {}
      lastIp = ip;
      post({ cmd: 'init', ip: ip, config: config });
      setTimeout(watchIp, 3000);
    });
  }

  function onModuleMessage(ev) {
    var m;
    if (typeof ev.data !== 'string') { return; }
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    handleEvent(m);
  }

  function handleEvent(m) {
    if (m.ev === 'ready') {
      gotReady = true;
      sendInit();
    } else if (m.ev === 'state') {
      S = m.state;
      render();
    } else if (m.ev === 'persist') {
      try { localStorage.setItem(CONFIG_KEY, JSON.stringify(m.config)); } catch (e) {}
    } else if (m.ev === 'stats') {
      $('hud-fps').textContent = m.fps + ' fps';
      lastStats = m;
      if (menuOpen) { drawMenu(); }
    } else if (m.ev === 'menu') {       // L1 + R1 + L3 + R3 ya da kol sayfasindaki Menu dugmesi
      if (streaming && !exitOpen && !(S && S.needPin)) { setMenu(!menuOpen); }
    } else if (m.ev === 'nav') {        // menu acikken kol tuslari
      if (menuOpen) { menuKey(m.key); }
    } else if (m.ev === 'log') {
      // Modul cokerse kendi gunlugu de gider: son satirlari burada tut.
      tail.push(m.line);
      if (tail.length > 200) { tail.shift(); }
      window.__log = tail;
    } else if (m.ev === 'crash' || m.ev === 'crashstack' || m.ev === 'crashraw') {
      crash[m.ev] = m;
      window.__crash = crash;
      try { localStorage.setItem('chiaki-crash', JSON.stringify({ at: new Date().toISOString(), crash: crash, log: tail.slice(-40) })); } catch (e) {}
    }
  }

  function loadModule() {
    var host = $('nacl-host');
    // Olaylar embed'den kabarciklanmaz; kapsayicida yakalama asamasinda dinlenir.
    host.addEventListener('message', onModuleMessage, true);
    host.addEventListener('error', function () {
      fatal(t('fatal_load', nacl && nacl.lastError ? nacl.lastError : t('fatal_unknown')));
    }, true);
    host.addEventListener('crash', function () {
      fatal(t('fatal_crash', nacl ? nacl.exitStatus : '?') +
        (crash.crash ? ' pc=' + crash.crash.pc + ' lr=' + crash.crash.lr : '') +
        (tail.length ? t('fatal_step', tail[tail.length - 1]) : '') + '.');
    }, true);
    nacl = document.createElement('embed');
    nacl.setAttribute('type', 'application/x-nacl');
    nacl.setAttribute('src', 'chiaki.nmf');
    nacl.setAttribute('width', '1280');
    nacl.setAttribute('height', '720');
    host.appendChild(nacl);
    setTimeout(function () {
      if (!gotReady && !fatalOpen) { fatal(t('fatal_timeout')); }
    }, 30000);
  }

  // ---- cizim -----------------------------------------------------------------
  function setFocus(i) {
    var n;
    if (!focusables.length) { return; }
    focusIndex = Math.max(0, Math.min(focusables.length - 1, i));
    for (n = 0; n < focusables.length; n++) {
      focusables[n].className = focusables[n].className.replace(/\s*focus/g, '') + (n === focusIndex ? ' focus' : '');
    }
    scrollConsoles();
  }

  // Konsol listesi ucten uzunsa kutu kayar: odaktaki konsol hep gorunur kalsin.
  function scrollConsoles() {
    var box = $('consoles'), el = focusables[focusIndex];
    if (!el || el.parentNode !== box) { return; }
    if (el.offsetTop < box.scrollTop) { box.scrollTop = el.offsetTop; }
    else if (el.offsetTop + el.offsetHeight > box.scrollTop + box.clientHeight) {
      box.scrollTop = el.offsetTop + el.offsetHeight - box.clientHeight;
    }
  }

  function buildConsoles() {
    var box = $('consoles'), list = S.consoles || [], i, c, el, badge, text, name, sub, go;
    var key = JSON.stringify(list), prev = focusables[focusIndex], prevDuid, n;
    if (key === consolesKey) { return; }
    consolesKey = key;
    prevDuid = prev ? prev.getAttribute('data-duid') : null;
    while (box.firstChild) { box.removeChild(box.firstChild); }
    for (i = 0; i < list.length; i++) {
      c = list[i];
      el = document.createElement('div');
      el.className = 'item console' + (c.remoteplay ? '' : ' disabled');
      el.setAttribute('data-duid', c.duid);
      el.setAttribute('data-ok', c.remoteplay ? '1' : '0');
      badge = document.createElement('div'); badge.className = 'badge'; badge.textContent = 'PS5';
      text = document.createElement('div');
      name = document.createElement('div'); name.className = 'console-name'; name.textContent = c.name || 'PS5';
      sub = document.createElement('div'); sub.className = 'console-sub';
      setT(sub, c.remoteplay ? 'rp_on' : 'rp_off');
      go = document.createElement('div'); go.className = 'go'; setT(go, 'connect_go');
      text.appendChild(name); text.appendChild(sub);
      el.appendChild(badge); el.appendChild(text); el.appendChild(go);
      box.appendChild(el);
    }
    // odaklanabilir liste: konsollar + ayar kutulari
    focusables = Array.prototype.slice.call(box.children).concat([$('opt-res'), $('opt-fps'), $('opt-lang'), $('opt-refresh')]);
    // Liste degisince odak ayni ogede kalsin: once ayni konsol, sonra ayni ayar kutusu, yoksa ilk oge.
    focusIndex = 0;
    if (!userMoved && list.length) {
      // Kullanici henuz gezinmedi: son baglanilan konsol (yoksa ilk konsol) hazir secili gelsin.
      for (n = 0; n < list.length; n++) {
        if (list[n].duid === S.lastDuid) { focusIndex = n; break; }
      }
    } else {
      for (n = 0; n < focusables.length; n++) {
        if ((prevDuid && focusables[n].getAttribute('data-duid') === prevDuid) || (prev && prev.id && focusables[n] === prev)) {
          focusIndex = n;
          break;
        }
      }
    }
    setFocus(focusIndex);
  }

  function hud(ms) {
    $('hud').className = 'show';
    clearTimeout(hudTimer);
    if (ms) { hudTimer = setTimeout(function () { $('hud').className = ''; }, ms); }
  }

  function screenSaver(on) {
    try {
      var a = window.webapis.appcommon;
      a.setScreenSaver(on ? a.AppCommonScreenSaverState.SCREEN_SAVER_ON : a.AppCommonScreenSaverState.SCREEN_SAVER_OFF);
    } catch (e) {}
  }

  function releaseHeld() {
    var k;
    for (k in held) { if (held.hasOwnProperty(k) && held[k]) { post({ cmd: 'key', key: k, down: false }); } }
    held = {};
    clearTimeout(backTimer);
  }

  // Uygulama arka plana gecerse (Ana ekran, kaynak degisimi) tus birakma olaylari gelmez:
  // basili kalan tus olmasin ve yayin bosuna surmesin.
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) { return; }
    releaseHeld();
    down = {};
    if (streaming || (S && S.phase === 'connecting')) { post({ cmd: 'stop' }); }
  });

  function setStreaming(on) {
    if (on === streaming) { return; }
    streaming = on;
    show($('ui'), !on);
    releaseHeld();
    exitOpen = false;
    show($('exit'), false);
    screenSaver(!on);
    if (on) { hud(7000); } else { clearTimeout(hudTimer); $('hud').className = ''; }
    // Baglaninca kisayolu ogret (chiaki-ng'deki gibi); yayin bitince menu ve bildirim kapanir.
    clearTimeout(tipTimer);
    lastStats = null;
    if (on) {
      $('tip').className = 'show';
      tipTimer = setTimeout(function () { $('tip').className = ''; }, 9000);
    } else {
      $('tip').className = '';
      setMenu(false);
    }
  }

  // ---- yayin menusu ------------------------------------------------------------
  function consoleName() {
    var i, list = (S && S.consoles) || [];
    for (i = 0; i < list.length; i++) { if (list[i].duid === S.lastDuid) { return list[i].name || 'PS5'; } }
    return 'PS5';
  }

  function drawMenu() {
    var m = lastStats || {}, s = (S && S.settings) || {}, i, sub;
    var num = function (v, digits) { return typeof v === 'number' ? v.toFixed(digits || 0) : '–'; };
    var ms = function (v) { return typeof v === 'number' && v > 0 ? t('ms', v) : '–'; };
    $('menu-title').textContent = t('menu_connected', consoleName());
    sub = (m.h ? m.h + 'p' : (s.resolution || 720) + 'p') + ' · ' + (s.resolution === 1080 ? 30 : (s.fps || 60)) + ' fps · H.264';
    if (s.lowLatency !== false) { sub += ' · ' + t('menu_low'); }
    $('menu-sub').textContent = sub;
    $('st-mbps').textContent = num(m.mbps, 1);
    $('st-decoded').textContent = num(m.decoded);
    $('st-fps').textContent = num(m.fps);
    $('st-decode').textContent = ms(m.decodeMs);
    $('st-screen').textContent = ms(m.screenMs);
    $('st-rtt').textContent = ms(m.rtt);
    $('st-lost').textContent = num(m.lost);
    for (i = 0; i < MENU_IDS.length; i++) { $(MENU_IDS[i]).className = 'mbtn' + (i === menuFocus ? ' focus' : ''); }
  }

  function setMenu(open) {
    if (!streaming) { open = false; }
    if (open === menuOpen) { return; }
    menuOpen = open;
    menuFocus = 0;
    if (open) {
      releaseHeld();
      clearTimeout(tipTimer);
      $('tip').className = '';
      drawMenu();
    }
    show($('menu'), open);
    post({ cmd: 'menu', open: open });   // kol girdisi menuye ya da konsola yonlensin
  }

  function menuKey(key) {
    if (key === 'left' || key === 'up') { menuFocus = (menuFocus + MENU_IDS.length - 1) % MENU_IDS.length; drawMenu(); }
    else if (key === 'right' || key === 'down') { menuFocus = (menuFocus + 1) % MENU_IDS.length; drawMenu(); }
    else if (key === 'back') { setMenu(false); }
    else if (key === 'ok') {
      if (menuFocus === 1) { post({ cmd: 'stop' }); }
      else if (menuFocus === 2) { post({ cmd: 'sleep' }); }
      setMenu(false);
    }
  }

  function render() {
    var i, busy, pad, url, empty = '';
    setLang(S.lang);
    S.settings = S.settings || { resolution: 720, fps: 60, bitrate: 0 };
    S.consoles = S.consoles || [];
    busy = S.phase === 'busy';
    pad = S.gamepad || '';
    $('tv-ip').textContent = S.ip || t('ip_none');
    url = S.ip ? ('http://' + S.ip + ':' + S.port) : t('url_wait');
    $('setup-url').textContent = url;
    for (i = 0; i < 4; i++) { $('d' + i).textContent = (S.code || '––––').charAt(i); }
    $('psn-dot').className = 'dot' + (S.loggedIn ? ' ok' : '');
    $('psn-text').textContent = busy ? (S.message + '…') : t(S.loggedIn ? 'psn_on' : 'psn_off');
    $('pad-dot').className = 'dot ' + (pad ? 'ok' : 'off');
    $('pad-text').textContent = pad ? t('pad_name', pad.substring(0, 26)) : t('pad_none');
    $('hud-pad').textContent = t(pad ? 'hud_pad_on' : 'hud_pad_none');
    $('val-res').textContent = S.settings.resolution + 'p';
    $('val-fps').textContent = String(S.settings.fps);

    buildConsoles();
    if (!S.loggedIn) {
      empty = t('empty_login');
    } else if (!S.consoles.length) {
      empty = busy ? (S.message + '…')
                   : t('empty_none');
    }
    $('empty').textContent = empty;
    show($('empty'), !!empty);

    $('banner').textContent = S.error || '';
    show($('banner'), !!S.error);

    setStreaming(S.phase === 'streaming');
    $('busy-text').textContent = S.message || '';
    show($('busy'), S.phase === 'connecting' && !S.needPin);
    if (S.needPin && !pinShown) { pinValue = ''; pinFocus = 0; drawPin(); }
    pinShown = !!S.needPin;
    $('pin-sub').textContent = t(S.pinIncorrect ? 'pin_wrong' : 'pin_sub');
    show($('pin'), pinShown);
  }

  // ---- PIN tuslari -----------------------------------------------------------
  function drawPin() {
    var i, dots = '';
    for (i = 0; i < pinValue.length; i++) { dots += '•'; }
    $('pin-value').textContent = dots;
    for (i = 0; i < pinKeys.length; i++) {
      pinKeys[i].className = 'item' + (i === pinFocus ? ' focus' : '');
    }
  }
  function buildKeypad() {
    // 'del' ve 'ok' tuslarinin yazisi dile gore degisir; tusun kimligi data-k'de durur.
    var labels = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'del', '0', 'ok'], i, el;
    for (i = 0; i < labels.length; i++) {
      el = document.createElement('div');
      el.className = 'item';
      el.setAttribute('data-k', labels[i]);
      if (labels[i].length > 1) { setT(el, 'pin_' + labels[i]); } else { el.textContent = labels[i]; }
      $('keypad').appendChild(el);
      pinKeys.push(el);
    }
  }
  function pinPress(label) {
    if (label === 'del') { pinValue = pinValue.slice(0, -1); }
    else if (label === 'ok') { if (pinValue.length >= 4) { post({ cmd: 'pin', pin: pinValue }); pinValue = ''; } }
    else if (pinValue.length < 8) { pinValue += label; }
    drawPin();
  }
  function pinKey(k) {
    if (k >= 48 && k <= 57) { pinPress(String(k - 48)); }
    else if (k >= 96 && k <= 105) { pinPress(String(k - 96)); }
    else if (k === KEY.LEFT) { pinFocus = (pinFocus % 3 === 0) ? pinFocus : pinFocus - 1; drawPin(); }
    else if (k === KEY.RIGHT) { pinFocus = (pinFocus % 3 === 2) ? pinFocus : pinFocus + 1; drawPin(); }
    else if (k === KEY.UP) { pinFocus = pinFocus >= 3 ? pinFocus - 3 : pinFocus; drawPin(); }
    else if (k === KEY.DOWN) { pinFocus = pinFocus <= 8 ? pinFocus + 3 : pinFocus; drawPin(); }
    else if (k === KEY.OK) { pinPress(pinKeys[pinFocus].getAttribute('data-k')); }
    else if (k === KEY.BACK) { if (pinValue) { pinPress('del'); } else { post({ cmd: 'stop' }); } }
  }

  // ---- kumanda ---------------------------------------------------------------
  function exitApp() {
    try { tizen.application.getCurrentApplication().exit(); } catch (e) {}
  }

  function cycle(list, value, dir) {
    var i = list.indexOf(value);
    if (i < 0) { i = 0; }
    return list[(i + dir + list.length) % list.length];
  }

  function changeSetting(el, dir) {
    var s = S.settings, res = s.resolution, fps = s.fps, lg = lang;
    if (el.id === 'opt-res') { res = cycle(RESOLUTIONS, res, dir); }
    else if (el.id === 'opt-fps') { fps = cycle(FPS, fps, dir); }
    else if (el.id === 'opt-lang') { lg = cycle(LANGS, lg, dir); }
    else { return; }
    post({ cmd: 'settings', resolution: res, fps: fps, bitrate: s.bitrate, lang: lg });
  }

  function homeKey(k) {
    var el = focusables[focusIndex];
    userMoved = true;
    if (k === KEY.UP) { setFocus(focusIndex - 1); }
    else if (k === KEY.DOWN) { setFocus(focusIndex + 1); }
    else if (k === KEY.LEFT || k === KEY.RIGHT) {
      if (el && el.className.indexOf('opt') >= 0 && el.id !== 'opt-refresh') { changeSetting(el, k === KEY.LEFT ? -1 : 1); }
      else if (el && el.className.indexOf('opt') >= 0) { setFocus(focusIndex + (k === KEY.LEFT ? -1 : 1)); }
    }
    else if (k === KEY.OK && el) {
      if (el.id === 'opt-refresh') { post({ cmd: 'refresh' }); }
      else if (el.id === 'opt-res' || el.id === 'opt-fps' || el.id === 'opt-lang') { changeSetting(el, 1); }
      else if (el.getAttribute('data-ok') === '1' && S.phase === 'idle') { post({ cmd: 'connect', duid: el.getAttribute('data-duid') }); }
    }
    else if (k === KEY.BACK) { exitApp(); }
  }

  function openExit() {
    releaseHeld();
    exitOpen = true;
    exitNeedsBackUp = true;
    show($('exit'), true);
  }

  function streamKeyDown(k) {
    var name = STREAM_KEYS[k];
    if (menuOpen) {   // menu acik: kumanda menuyu gezdirir, konsola tus gitmez
      ignoreUntilUp[k] = true;   // menuyu kapatan tus basili kalirsa oyuna gitmesin
      if (k === KEY.LEFT || k === KEY.UP) { menuKey('left'); }
      else if (k === KEY.RIGHT || k === KEY.DOWN) { menuKey('right'); }
      else if (k === KEY.OK) { menuKey('ok'); }
      else if (k === KEY.BACK || k === KEY.INFO) { menuKey('back'); }
      return;
    }
    if (exitOpen) {
      if (k === KEY.OK) { post({ cmd: 'stop' }); exitOpen = false; show($('exit'), false); }
      else if (k === KEY.BACK && !exitNeedsBackUp) { exitOpen = false; show($('exit'), false); }
      return;
    }
    if (k === KEY.EXIT) { post({ cmd: 'stop' }); return; }
    if (k === KEY.INFO) { ignoreUntilUp[k] = true; setMenu(true); return; }   // kumandada Info: yayin menusu
    if (k === KEY.BACK) { name = 'back'; }
    if (!name || held[name]) { return; }   // tus tekrari: zaten basili
    held[name] = true;
    post({ cmd: 'key', key: name, down: true });
    if (name === 'back') { backTimer = setTimeout(openExit, 900); }   // basili tutulursa cikis sorusu
  }

  function streamKeyUp(k) {
    var name = k === KEY.BACK ? 'back' : STREAM_KEYS[k];
    if (k === KEY.BACK) { exitNeedsBackUp = false; }
    if (!name || !held[name]) { return; }
    if (name === 'back') { clearTimeout(backTimer); }
    held[name] = false;
    post({ cmd: 'key', key: name, down: false });
  }

  document.addEventListener('keydown', function (e) {
    var k = e.keyCode, repeat = !!down[k] || e.repeat === true;
    down[k] = true;
    // Basili tutulan OK/Geri ikinci kez islenmesin (ornegin onaydan sonra arkadaki ekranda yeniden tetiklenmesin).
    // Yon tuslarinin tekrari ana ekranda istenir; yayinda zaten 'held' ile suzulur.
    if (repeat && (k === KEY.OK || k === KEY.BACK) && !(streaming && !exitOpen)) { return; }
    if (fatalOpen) {
      if (k === KEY.OK) { location.reload(); } else if (k === KEY.BACK) { exitApp(); }
      return;
    }
    if (!S) { if (k === KEY.BACK) { exitApp(); } return; }
    if (S.needPin) { pinKey(k); return; }
    if (streaming && ignoreUntilUp[k] && !menuOpen) { return; }
    if (streaming && menuOpen && repeat && (k === KEY.OK || k === KEY.BACK || k === KEY.INFO)) { return; }
    if (streaming) { streamKeyDown(k); return; }
    if (S.phase === 'connecting') { if (k === KEY.BACK) { post({ cmd: 'stop' }); } return; }
    homeKey(k);
  });
  document.addEventListener('keyup', function (e) {
    down[e.keyCode] = false;
    delete ignoreUntilUp[e.keyCode];
    if (e.keyCode === KEY.BACK) { exitNeedsBackUp = false; }
    if (streaming && S && !S.needPin) { streamKeyUp(e.keyCode); }
  });

  function registerKeys() {
    var names = ['ColorF0Red', 'ColorF1Green', 'ColorF2Yellow', 'ColorF3Blue', 'Info',
                 '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'], i;
    for (i = 0; i < names.length; i++) {
      try { tizen.tvinputdevice.registerKey(names[i]); } catch (e) {}
    }
  }

  // ---- baslat ----------------------------------------------------------------
  fit();
  buildKeypad();
  try { if (LANGS.indexOf(localStorage.getItem(LANG_KEY)) >= 0) { lang = localStorage.getItem(LANG_KEY); } } catch (e) {}
  applyI18n();
  focusables = [$('opt-res'), $('opt-fps'), $('opt-lang'), $('opt-refresh')];
  setFocus(0);
  if (isTizen) { registerKeys(); }
  if (!isTizen && location.hash === '#mock') {
    // Gelistirme: masaustu tarayicida modulsuz onizleme. window.__render(durum) ile ekran cizilir.
    window.__render = function (state) { S = state; render(); };
    window.__ev = handleEvent;   // modul olaylarini taklit etmek icin: __ev({ev:'menu'}), __ev({ev:'stats',...})
    nacl = { postMessage: function (m) { window.__sent = (window.__sent || []).concat([m]); } };
  } else {
    loadModule();
  }
})();
