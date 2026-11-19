(function () {
  "use strict";

  var OUT_RATES = [
    ["0", "Off", 0],
    ["1000", "1 Hz", 1],
    ["500", "2 Hz", 2],
    ["200", "5 Hz", 5],
    ["100", "10 Hz", 10],
    ["50", "20 Hz", 20]
  ];

  var OUT_IDS = ["cfg-gga", "cfg-gsa", "cfg-gsv", "cfg-gst", "cfg-rmc"];

  var ERR_MAP = {
    bad_json: "Invalid message format (bad_json)",
    too_long: "Message too long (too_long)",
    unknown_key: "Unknown setting (unknown_key)",
    bad_value: "Invalid value (bad_value)",
    locked: "Factory mode locked (locked)",
    busy: "Device busy, try again (busy)",
    unsupported: "Unsupported by device (unsupported)",
    failed: "Operation failed (failed)",
    timeout: "Request timed out (timeout)"
  };

  var ws = null;
  var reqId = 0;
  var queue = [];
  var inflight = null;
  var ackTimer = null;
  var ACK_TIMEOUT_MS = 8000;
  var MAX_FRAME_BYTES = 1024;
  var reconnTimer = null;
  var helloOk = false;
  var rebootPending = false;
  var byeReceived = false;
  var lastConfig = {};

  function $(id) {
    return document.getElementById(id);
  }

  function setConn(cls, text) {
    var dot = $("conn-dot");
    dot.className = "dot " + cls;
    $("conn-txt").textContent = text;
  }

  function showBanner(cls, text) {
    var b = $("banner");
    b.className = "banner show " + cls;
    b.textContent = text;
  }

  function hideBanner() {
    $("banner").className = "banner";
  }

  function setMsg(el, ok, text) {
    if (!el) return;
    el.className = "msg " + (ok ? "ok" : "err");
    el.textContent = text;
  }

  function fmtUptime(s) {
    if (typeof s !== "number") return "--";
    var h = Math.floor(s / 3600);
    var m = Math.floor((s % 3600) / 60);
    var sec = s % 60;
    return (h > 0 ? h + "h " : "") + m + "m " + sec + "s";
  }

  function fmtKb(b) {
    return typeof b === "number" ? (b / 1024).toFixed(1) + " KB" : "--";
  }

  function initSelects() {
    for (var i = 0; i < OUT_IDS.length; i++) {
      var sel = $(OUT_IDS[i]);
      for (var j = 0; j < OUT_RATES.length; j++) {
        var opt = document.createElement("option");
        opt.value = OUT_RATES[j][0];
        opt.textContent = OUT_RATES[j][1];
        opt.setAttribute("data-hz", String(OUT_RATES[j][2]));
        sel.appendChild(opt);
      }
    }
  }

  function nextId() {
    reqId = (reqId % 65535) + 1;
    return reqId;
  }

  function utf8Bytes(s) {
    if (typeof TextEncoder !== "undefined") {
      return new TextEncoder().encode(s).length;
    }
    return unescape(encodeURIComponent(s)).length;
  }

  function setControlsEnabled(enabled) {
    var els = document.querySelectorAll(".card-body select, .card-body input, .card-body button");
    for (var i = 0; i < els.length; i++) {
      els[i].disabled = !enabled;
    }
  }

  function notifyDropped(items, reason, skipEl) {
    for (var i = 0; i < items.length; i++) {
      if (items[i].msgEl && items[i].msgEl !== skipEl) {
        setMsg(items[i].msgEl, false, reason);
      }
    }
  }

  function resetSessionState() {
    helloOk = false;
    rebootPending = false;
    lastConfig = {};
    setFactoryUnlocked(false);
    setControlsEnabled(false);
    syncRateSeg("");
    $("const-box").classList.add("hide");
    $("d-sn").textContent = "--";
    $("fac-sn").value = "";
    $("bt-wl").replaceChildren();
    $("bt-wl-empty").classList.remove("hide");
  }

  function clearAckTimer() {
    if (ackTimer) {
      clearTimeout(ackTimer);
      ackTimer = null;
    }
  }

  function pump() {
    if (inflight || queue.length === 0 || rebootPending || !helloOk) return;
    if (!ws || ws.readyState !== 1) return;
    var cur = queue.shift();
    cur.payload.id = nextId();
    var raw = JSON.stringify(cur.payload);
    if (utf8Bytes(raw) > MAX_FRAME_BYTES) {
      setMsg(cur.msgEl, false, "Failed: " + ERR_MAP.too_long);
      syncControls(lastConfig);
      pump();
      return;
    }
    inflight = cur;
    clearAckTimer();
    ackTimer = setTimeout(function () {
      ackTimer = null;
      if (inflight !== cur) return;
      inflight = null;
      setMsg(cur.msgEl, false, "Failed: " + ERR_MAP.timeout);
      notifyDropped(queue, "Not sent: request timed out, reconnecting", cur.msgEl);
      queue = [];
      syncControls(lastConfig);
      if (ws) ws.close();
    }, ACK_TIMEOUT_MS);
    ws.send(raw);
  }

  function sendReq(payload, msgEl, onOk) {
    if (byeReceived || rebootPending) {
      setMsg(msgEl, false, "Device rebooted, you may close this page");
      return;
    }
    if (!ws || ws.readyState !== 1 || !helloOk) {
      setMsg(msgEl, false, "Not connected to device");
      return;
    }
    setMsg(msgEl, true, "Sending...");
    queue.push({ payload: payload, msgEl: msgEl, onOk: onOk || null });
    pump();
  }

  function syncOutRateLimits(rateVal) {
    var rate = Number(rateVal);
    for (var i = 0; i < OUT_IDS.length; i++) {
      var opts = $(OUT_IDS[i]).options;
      for (var j = 0; j < opts.length; j++) {
        var hz = Number(opts[j].getAttribute("data-hz"));
        opts[j].disabled = rate > 0 && hz > rate;
      }
    }
    var hint = $("out-rate-hint");
    if (!hint) return;
    if (rate > 0 && rate < 20) {
      hint.textContent = "Output rates above " + rate + " Hz are disabled (limited by positioning rate).";
    } else if (rate >= 20) {
      hint.textContent = "Output rates cannot exceed positioning rate (" + rate + " Hz).";
    } else {
      hint.textContent = "Output rates faster than positioning rate are disabled.";
    }
  }

  function syncRateSeg(val) {
    var btns = document.querySelectorAll(".seg-btn[data-rate]");
    for (var i = 0; i < btns.length; i++) {
      btns[i].classList.toggle("active", btns[i].getAttribute("data-rate") === String(val));
    }
    syncOutRateLimits(val);
  }

  function syncControls(cfg) {
    var sels = document.querySelectorAll("select[data-key]");
    for (var i = 0; i < sels.length; i++) {
      var k = sels[i].getAttribute("data-key");
      if (Object.prototype.hasOwnProperty.call(cfg, k)) {
        sels[i].value = String(cfg[k]);
      }
    }
    if (Object.prototype.hasOwnProperty.call(cfg, "gnss.rate")) {
      syncRateSeg(cfg["gnss.rate"]);
    }
    var chks = document.querySelectorAll("input[data-const]");
    var hasConst = false;
    for (var j = 0; j < chks.length; j++) {
      var ck = chks[j].getAttribute("data-const");
      if (Object.prototype.hasOwnProperty.call(cfg, ck)) {
        hasConst = true;
        chks[j].checked = Number(cfg[ck]) === 1;
      }
    }
    $("const-box").classList.toggle("hide", !hasConst);

    if (Object.prototype.hasOwnProperty.call(cfg, "factory.sn")) {
      var sn = String(cfg["factory.sn"] || "");
      $("d-sn").textContent = sn || "--";
      $("fac-sn").value = sn;
    }
  }

  function setFactoryUnlocked(unlocked) {
    $("fac-locked").classList.toggle("hide", unlocked);
    $("fac-unlocked").classList.toggle("hide", !unlocked);
  }

  function renderWhitelist(list) {
    var ul = $("bt-wl");
    ul.replaceChildren();
    if (!Array.isArray(list) || list.length === 0) {
      $("bt-wl-empty").classList.remove("hide");
      return;
    }
    $("bt-wl-empty").classList.add("hide");
    for (var i = 0; i < list.length; i++) {
      (function (mac) {
        var li = document.createElement("li");
        li.className = "wl-item";
        var span = document.createElement("span");
        span.textContent = mac;
        var btn = document.createElement("button");
        btn.type = "button";
        btn.className = "btn btn-danger";
        btn.textContent = "Remove";
        btn.addEventListener("click", function () {
          sendReq(
            { t: "set", key: "bt.wl.del", value: mac },
            $("msg-bt")
          );
        });
        li.appendChild(span);
        li.appendChild(btn);
        ul.appendChild(li);
      })(list[i]);
    }
  }

  function handleMsg(raw) {
    var msg;
    try {
      msg = JSON.parse(raw);
    } catch (_e) {
      return;
    }
    if (!msg || !msg.t) return;

    if (msg.t === "hello") {
      if (msg.proto !== 1) return;
      helloOk = true;
      rebootPending = false;
      lastConfig = {};
      $("hdr-sub").textContent = (msg.bt_name || "RTK") + " · " + (msg.board || "--") + " · " + (msg.fw || "--");
      $("d-board").textContent = msg.board || "--";
      $("d-fw").textContent = msg.fw || "--";
      $("d-bt").textContent = msg.bt_name || "--";
      $("s-info").textContent = (msg.board || "--") + " / " + (msg.fw || "--");
      $("s-con").textContent = msg.console || "--";
      setFactoryUnlocked(Boolean(msg.factory));
      setControlsEnabled(true);
      return;
    }

    if (msg.t === "config") {
      var nextCfg = {};
      for (var k in msg) {
        if (k !== "t" && Object.prototype.hasOwnProperty.call(msg, k)) {
          nextCfg[k] = msg[k];
        }
      }
      lastConfig = nextCfg;
      syncControls(lastConfig);
      return;
    }

    if (msg.t === "status") {
      if (msg.dev) {
        var d = msg.dev;
        $("d-up").textContent = d.uptime_s !== undefined ? fmtUptime(d.uptime_s) : "--";
        if (d.heap_free !== undefined) {
          var minTxt = d.heap_min !== undefined ? " (min " + fmtKb(d.heap_min) + ")" : "";
          $("d-heap").textContent = fmtKb(d.heap_free) + minTxt;
        } else {
          $("d-heap").textContent = "--";
        }
        $("d-reset").textContent = d.reset !== undefined ? d.reset : "--";
      }
      if (msg.gnss) {
        var g = msg.gnss;
        $("g-state").textContent = g.state !== undefined ? g.state : "--";
        $("g-model").textContent = (g.model !== undefined || g.fw !== undefined)
          ? ((g.model || "--") + " / " + (g.fw || "--")) : "--";
        $("g-fix").textContent = g.fix !== undefined ? g.fix : "--";
        $("chip-fix").textContent = "Fix: " + (g.fix !== undefined ? g.fix : "--");
        $("g-sats").textContent = g.sats !== undefined ? String(g.sats) : "--";
        $("chip-sats").textContent = "Sats: " + (g.sats !== undefined ? String(g.sats) : "--");
        $("g-hacc").textContent = g.hacc_mm !== undefined ? g.hacc_mm + " mm" : "--";
        $("g-hz").textContent = g.hz !== undefined ? g.hz + " Hz" : "--";
      }
      if (msg.bt && Array.isArray(msg.bt.whitelist)) {
        renderWhitelist(msg.bt.whitelist);
      }
      return;
    }

    if (msg.t === "ack") {
      if (!inflight || msg.id !== inflight.payload.id) return;
      clearAckTimer();
      var cur = inflight;
      inflight = null;
      if (msg.ok) {
        setMsg(cur.msgEl, true, "Success");
        if (cur.onOk) cur.onOk();
        if (cur.payload.t === "cmd" && (cur.payload.name === "exit" || cur.payload.name === "freset")) {
          rebootPending = true;
          notifyDropped(queue, "Not sent: device is rebooting", cur.msgEl);
          queue = [];
          return;
        }
      } else {
        setMsg(cur.msgEl, false, "Failed: " + (ERR_MAP[msg.err] || msg.err || "unknown"));
        syncControls(lastConfig);
      }
      pump();
      return;
    }

    if (msg.t === "bye") {
      byeReceived = true;
      if (reconnTimer) {
        clearTimeout(reconnTimer);
        reconnTimer = null;
      }
      setConn("warn", "Device rebooted");
      var txt = msg.reason === "freset"
        ? "Factory reset complete. Device is rebooting; you may close this page."
        : "Device switched back to Bluetooth mode. You may close this page.";
      showBanner("ok", txt);
      if (ws) ws.close();
    }
  }

  function connect() {
    if (byeReceived) return;
    if (reconnTimer) {
      clearTimeout(reconnTimer);
      reconnTimer = null;
    }
    var host = location.host || "192.168.4.1";
    var proto = location.protocol === "https:" ? "wss://" : "ws://";
    setConn("warn", "Connecting...");
    ws = new WebSocket(proto + host + "/ws");

    ws.onopen = function () {
      setConn("ok", "Connected");
      hideBanner();
    };

    ws.onmessage = function (ev) {
      handleMsg(ev.data);
    };

    ws.onclose = function () {
      clearAckTimer();
      var dropped = inflight ? [inflight].concat(queue) : queue.slice();
      inflight = null;
      queue = [];
      ws = null;
      resetSessionState();
      if (byeReceived) return;
      notifyDropped(dropped, "Not sent: connection lost", null);
      setConn("err", "Disconnected, reconnecting in 2s");
      showBanner("warn", "WebSocket disconnected. Attempting to reconnect...");
      reconnTimer = setTimeout(connect, 2000);
    };

    ws.onerror = function () {
      if (ws) ws.close();
    };
  }

  function bindEvents() {
    var toggles = document.querySelectorAll("[data-toggle]");
    for (var i = 0; i < toggles.length; i++) {
      toggles[i].addEventListener("click", function () {
        var sec = $(this.getAttribute("data-toggle"));
        sec.classList.toggle("collapsed");
      });
    }

    var rateBtns = document.querySelectorAll(".seg-btn[data-rate]");
    for (var r = 0; r < rateBtns.length; r++) {
      rateBtns[r].addEventListener("click", function () {
        var val = this.getAttribute("data-rate");
        syncRateSeg(val);
        $("cfg-rate").value = val;
        sendReq({ t: "set", key: "gnss.rate", value: val }, $("msg-gnss"));
      });
    }

    var sels = document.querySelectorAll("select[data-key]");
    for (var j = 0; j < sels.length; j++) {
      sels[j].addEventListener("change", function () {
        var key = this.getAttribute("data-key");
        var msgEl = key.indexOf("bt.") === 0 ? $("msg-bt") : $("msg-gnss");
        sendReq({ t: "set", key: key, value: this.value }, msgEl);
      });
    }

    var chks = document.querySelectorAll("input[data-const]");
    for (var k = 0; k < chks.length; k++) {
      chks[k].addEventListener("change", function () {
        var key = this.getAttribute("data-const");
        sendReq({ t: "set", key: key, value: this.checked ? "1" : "0" }, $("msg-gnss"));
      });
    }

    $("btn-gnss-save").addEventListener("click", function () {
      sendReq({ t: "cmd", name: "save" }, $("msg-gnss"));
    });

    $("btn-wl-add").addEventListener("click", function () {
      var mac = $("bt-mac").value.trim().toUpperCase();
      if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) {
        setMsg($("msg-bt"), false, "Enter a valid MAC address, e.g. AA:BB:CC:DD:EE:FF");
        return;
      }
      sendReq({ t: "set", key: "bt.wl.add", value: mac }, $("msg-bt"), function () {
        $("bt-mac").value = "";
      });
    });

    $("btn-unlock").addEventListener("click", function () {
      var tok = $("fac-token").value;
      if (!tok) {
        setMsg($("msg-fac"), false, "Enter unlock token");
        return;
      }
      sendReq({ t: "unlock", token: tok }, $("msg-fac"), function () {
        $("fac-token").value = "";
        setFactoryUnlocked(true);
      });
    });

    $("btn-sn").addEventListener("click", function () {
      var sn = $("fac-sn").value.trim();
      if (!/^[0-9A-Za-z-]{1,16}$/.test(sn)) {
        setMsg($("msg-fac"), false, "SN must be 1-16 alphanumeric characters or hyphens");
        return;
      }
      sendReq({ t: "set", key: "factory.sn", value: sn }, $("msg-fac"));
    });

    $("btn-freset").addEventListener("click", function () {
      if (!confirm("Confirm factory reset? All settings will be cleared and the device will reboot.")) return;
      sendReq({ t: "cmd", name: "freset" }, $("msg-fac"));
    });

    $("btn-exit").addEventListener("click", function () {
      sendReq({ t: "cmd", name: "exit" }, $("msg-sys"));
    });
  }

  initSelects();
  setControlsEnabled(false);
  bindEvents();
  connect();
})();
