(function () {
  "use strict";

  var PERIODS = [
    ["0", "关闭 (0)"],
    ["50", "50 ms"],
    ["100", "100 ms"],
    ["200", "200 ms"],
    ["500", "500 ms"],
    ["1000", "1000 ms"]
  ];

  var ERR_MAP = {
    bad_json: "报文格式错误 (bad_json)",
    too_long: "报文过长 (too_long)",
    unknown_key: "未知配置项 (unknown_key)",
    bad_value: "参数值无效 (bad_value)",
    locked: "工厂模式未解锁 (locked)",
    busy: "设备忙，请稍后重试 (busy)",
    unsupported: "当前设备不支持 (unsupported)",
    failed: "执行失败 (failed)",
    timeout: "操作超时 (timeout)"
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
    var ids = ["cfg-gga", "cfg-gsa", "cfg-gsv", "cfg-gst", "cfg-rmc"];
    for (var i = 0; i < ids.length; i++) {
      var sel = $(ids[i]);
      for (var j = 0; j < PERIODS.length; j++) {
        var opt = document.createElement("option");
        opt.value = PERIODS[j][0];
        opt.textContent = PERIODS[j][1];
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
      setMsg(cur.msgEl, false, "失败: " + ERR_MAP.too_long);
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
      setMsg(cur.msgEl, false, "失败: " + ERR_MAP.timeout);
      notifyDropped(queue, "未发送：请求超时，正在重连", cur.msgEl);
      queue = [];
      syncControls(lastConfig);
      if (ws) ws.close();
    }, ACK_TIMEOUT_MS);
    ws.send(raw);
  }

  function sendReq(payload, msgEl, onOk) {
    if (byeReceived || rebootPending) {
      setMsg(msgEl, false, "设备已重启，请关闭本页");
      return;
    }
    if (!ws || ws.readyState !== 1 || !helloOk) {
      setMsg(msgEl, false, "未连接到设备");
      return;
    }
    setMsg(msgEl, true, "正在发送...");
    queue.push({ payload: payload, msgEl: msgEl, onOk: onOk || null });
    pump();
  }

  function syncRateSeg(val) {
    var btns = document.querySelectorAll(".seg-btn[data-rate]");
    for (var i = 0; i < btns.length; i++) {
      btns[i].classList.toggle("active", btns[i].getAttribute("data-rate") === String(val));
    }
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
        btn.textContent = "删除";
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
          var minTxt = d.heap_min !== undefined ? " (最低 " + fmtKb(d.heap_min) + ")" : "";
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
        $("chip-fix").textContent = "定位: " + (g.fix !== undefined ? g.fix : "--");
        $("g-sats").textContent = g.sats !== undefined ? String(g.sats) : "--";
        $("chip-sats").textContent = "卫星: " + (g.sats !== undefined ? String(g.sats) : "--");
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
        setMsg(cur.msgEl, true, "操作成功");
        if (cur.onOk) cur.onOk();
        if (cur.payload.t === "cmd" && (cur.payload.name === "exit" || cur.payload.name === "freset")) {
          rebootPending = true;
          notifyDropped(queue, "未发送：设备即将重启", cur.msgEl);
          queue = [];
          return;
        }
      } else {
        setMsg(cur.msgEl, false, "失败: " + (ERR_MAP[msg.err] || msg.err || "unknown"));
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
      setConn("warn", "设备已重启");
      var txt = msg.reason === "freset"
        ? "设备已恢复出厂设置并重启，可以关闭本页"
        : "设备已切回蓝牙模式，可以关闭本页";
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
    setConn("warn", "正在连接...");
    ws = new WebSocket(proto + host + "/ws");

    ws.onopen = function () {
      setConn("ok", "已连接");
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
      notifyDropped(dropped, "未发送：连接已断开", null);
      setConn("err", "连接断开，2 秒后重连");
      showBanner("warn", "WebSocket 连接已断开，正在尝试重新连接...");
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
        setMsg($("msg-bt"), false, "请输入有效 MAC 地址，例如 AA:BB:CC:DD:EE:FF");
        return;
      }
      sendReq({ t: "set", key: "bt.wl.add", value: mac }, $("msg-bt"), function () {
        $("bt-mac").value = "";
      });
    });

    $("btn-unlock").addEventListener("click", function () {
      var tok = $("fac-token").value;
      if (!tok) {
        setMsg($("msg-fac"), false, "请输入解锁口令");
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
        setMsg($("msg-fac"), false, "SN 须为 1-16 位字母、数字或连字符");
        return;
      }
      sendReq({ t: "set", key: "factory.sn", value: sn }, $("msg-fac"));
    });

    $("btn-freset").addEventListener("click", function () {
      if (!confirm("确认恢复出厂设置？设备将清除配置并重启。")) return;
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
