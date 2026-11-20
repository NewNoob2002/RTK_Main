/*
 * RTK 网页配置界面（协议见 docs/web/ws-protocol.md v1）。
 *
 * 结构：
 *   SCHEMA  —— 卡片与设置项的声明表。新增一个可改键 = 在对应卡片 items 里加一行，
 *              渲染、乐观更新、回滚、禁用、"不支持"提示全部由通用代码处理。
 *   widgets —— 每种控件类型（switch/select/seg/stepper/...）一个构造函数，返回 DOM 并注册刷新。
 *   请求队列 —— 一次只发一条，等 ack 或 2 s 超时；超时后迟到的 ack 被忽略，下一帧 config 为准。
 * 约束：离线、无框架、ES2017；存储失败（iOS 门户小窗）静默降级；不使用 alert/confirm/innerHTML。
 */
(function () {
  "use strict";

  // ---------------------------------------------------------------- 常量
  const ACK_MS = 2000;
  const MAX_FRAME = 1024;
  const BACKOFF_S = [1, 2, 4, 8];
  const ERR = {
    bad_json: "Malformed request", too_long: "Request too long", unknown_key: "Setting not recognised by firmware",
    bad_value: "Invalid value", locked: "Factory functions are locked", busy: "Receiver is busy, try again",
    unsupported: "Not supported by this receiver", failed: "Receiver rejected the change",
    timeout: "No response within 2 s, reverted", disconnected: "Connection lost"
  };
  const FIX = { none: ["NONE", "bad"], single: ["SINGLE", "warn"], dgps: ["DGPS", "info"], float: ["FLOAT", "info"], fixed: ["FIXED", "ok"] };
  const MAC_RE = /^([0-9A-F]{2}:){5}[0-9A-F]{2}$/;
  const SN_RE = /^[0-9A-Za-z-]{1,16}$/;
  const ICONS = {
    gnss: "M12 2v4M12 18v4M2 12h4M18 12h4M12 8a4 4 0 1 0 0 8a4 4 0 1 0 0-8",
    gate: "M12 3l8 3v6c0 5-3.5 8-8 9c-4.5-1-8-4-8-9V6zM9 12l2 2l4-4",
    dev: "M7 3h10a1 1 0 0 1 1 1v16a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1M10 18h4",
    bt: "M7 7l10 10l-5 4V3l5 4L7 17",
    fac: "M6 11h12v10H6zM9 11V7a3 3 0 0 1 6 0v4",
    sys: "M12 3v8M6.3 6.3a8 8 0 1 0 11.4 0"
  };

  // ---------------------------------------------------------------- 状态
  let ws = null;
  let ready = false;     // 收到 hello 之后为 true
  let configured = false; // 收到过 config
  let leaving = false;   // 收到 bye 或已发出 exit/freset
  let attempt = 0;
  let retryTimer = null;
  let hello = {};
  let caps = {};
  let config = {};
  const status = { dev: {}, gnss: {}, bt: {} };
  let unlocked = false;
  let reqId = 0;
  const queue = [];
  let inflight = null;
  let ackTimer = null;
  const optimistic = {}; // key -> 已发出但未确认的值
  const unsupported = {}; // key -> true（设备回 unsupported）
  const fields = [];     // 已注册设置项
  const views = [];      // 其它刷新函数

  // ---------------------------------------------------------------- 工具
  const byId = (id) => document.getElementById(id);
  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

  function h(tag, attrs, kids) {
    const e = document.createElement(tag);
    for (const k in attrs || {}) {
      const v = attrs[k];
      if (v === null || v === undefined || v === false) continue;
      if (k === "class") e.className = v;
      else if (k === "text") e.textContent = v;
      else if (k.slice(0, 2) === "on") e.addEventListener(k.slice(2), v);
      else e.setAttribute(k, v === true ? "" : v);
    }
    (kids || []).forEach((c) => {
      if (c !== null && c !== undefined && c !== false) e.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
    });
    return e;
  }

  function svg(d, cls) {
    const ns = "http://www.w3.org/2000/svg";
    const s = document.createElementNS(ns, "svg");
    s.setAttribute("viewBox", "0 0 24 24");
    s.setAttribute("class", cls);
    s.setAttribute("aria-hidden", "true");
    const p = document.createElementNS(ns, "path");
    p.setAttribute("d", d);
    s.appendChild(p);
    return s;
  }

  function store(key, val) {
    try {
      if (val === undefined) return window.localStorage.getItem(key);
      window.localStorage.setItem(key, val);
    } catch (_e) { /* 门户小窗或隐私模式禁止存储：静默降级 */ }
    return null;
  }

  function utf8Len(s) {
    return unescape(encodeURIComponent(s)).length;
  }

  function fmtUptime(s) {
    if (typeof s !== "number") return "--";
    const d = Math.floor(s / 86400), hh = Math.floor(s / 3600) % 24, mm = Math.floor(s / 60) % 60;
    return (d ? d + "d " : "") + hh + "h " + mm + "m " + (s % 60) + "s";
  }

  const kb = (b) => (typeof b === "number" ? (b / 1024).toFixed(1) + " KB" : "--");
  const dash = (v, unit) => (v === undefined || v === null || v === "" ? "--" : String(v) + (unit || ""));

  // ---------------------------------------------------------------- 配置读取
  /** 读设备 config 中的值，统一成字符串；嵌套的 gga_gate 映射到 gnss.gga_gate.* 键。 */
  function cfgVal(key) {
    let v;
    if (has(config, key)) v = config[key];
    else {
      const m = /^gnss\.gga_gate\.(\w+)$/.exec(key);
      if (m && config.gga_gate && typeof config.gga_gate === "object") v = config.gga_gate[m[1]];
    }
    if (v === undefined || v === null) return undefined;
    if (v === true) return "1";
    if (v === false) return "0";
    return String(v);
  }

  function setCfg(key, v) {
    const m = /^gnss\.gga_gate\.(\w+)$/.exec(key);
    if (m && config.gga_gate && typeof config.gga_gate === "object" && !has(config, key)) {
      config.gga_gate[m[1]] = m[1] === "enabled" ? v === "1" : Number(v);
    } else config[key] = v;
  }

  const val = (key) => (has(optimistic, key) ? optimistic[key] : cfgVal(key));
  const keyBusy = (key) => (inflight && inflight.o.key === key) || queue.some((r) => r.o.key === key);

  function gateHeld() {
    if (val("gnss.gga_gate.enabled") !== "1") return false;
    const g = status.gnss;
    return !g.fix || g.fix === "none" || typeof g.sats !== "number" || g.sats < (Number(val("gnss.gga_gate.min_sats")) || 0);
  }

  // ---------------------------------------------------------------- SCHEMA
  const outField = (n) => ({ type: "select", key: "gnss.out." + n, label: n.toUpperCase() });
  const constField = (k, l) => ({ type: "switch", key: "gnss.const." + k, label: l });

  const SCHEMA = [
    { id: "gnss", col: "a", title: "GNSS", icon: "gnss", badge: gnssBadge, items: [
      { type: "tiles" },
      { type: "kv", rows: [["Module", () => dash(status.gnss.state)], ["Model / FW", () => dash(status.gnss.model) + " / " + dash(status.gnss.fw)]] },
      { type: "heading", text: "Positioning rate", prefix: "gnss.rate" },
      { type: "seg", key: "gnss.rate", label: "Positioning rate" },
      { type: "heading", text: "NMEA output", prefix: "gnss.out." },
      { type: "group", cls: "cols", items: ["gga", "gsa", "gsv", "gst", "rmc"].map(outField) },
      { type: "heading", text: "Constellations", prefix: "gnss.const." },
      { type: "group", cls: "cols",
        items: [constField("gps", "GPS"), constField("bds", "BeiDou"), constField("glo", "GLONASS"), constField("gal", "Galileo")] }
    ] },
    { id: "gate", col: "a", title: "GGA quality gate", icon: "gate", badge: gateBadge, items: [
      { type: "switch", key: "gnss.gga_gate.enabled", label: "Enable gate", hint: "Hold GGA to the phone and NTRIP caster while there is no fix or too few satellites" },
      { type: "stepper", key: "gnss.gga_gate.min_sats", label: "Minimum satellites", hint: "0–40", min: 0, max: 40 }
    ] },
    { id: "dev", col: "b", title: "Device", icon: "dev", badge: () => ["Wi-Fi", ""], items: [
      { type: "kv", rows: [
        ["Board", () => dash(hello.board)], ["Firmware", () => dash(hello.fw)], ["Serial number", () => dash(cfgVal("factory.sn"))],
        ["Bluetooth name", () => dash(hello.bt_name)], ["Uptime", () => fmtUptime(status.dev.uptime_s)],
        ["Free memory", () => kb(status.dev.heap_free) + (typeof status.dev.heap_min === "number" ? " (min " + kb(status.dev.heap_min) + ")" : "")],
        ["Last reset", () => dash(status.dev.reset)]] }
    ] },
    { id: "bt", col: "b", title: "Bluetooth", icon: "bt", badge: btBadge, items: [
      { type: "heading", text: "Whitelist" },
      { type: "whitelist" },
      { type: "switch", key: "bt.pair_next", label: "Open pairing on next Bluetooth boot", hint: "Lets a new phone pair once after leaving this page" }
    ] },
    { id: "fac", col: "b", title: "Factory", icon: "fac", closed: true, badge: () => (unlocked ? ["UNLOCKED", "warn"] : ["LOCKED", ""]), items: [
      { type: "factory" }
    ] },
    { id: "sys", col: "b", title: "System", icon: "sys", items: [
      { type: "kv", rows: [["Console", () => dash(hello.console)]] },
      { type: "action", text: "Exit and return to Bluetooth", cls: "pri", cmd: "exit", hint: "Stops the Wi-Fi access point and restarts the receiver in Bluetooth mode." }
    ] }
  ];

  function gnssBadge() {
    const s = status.gnss.state;
    if (!s) return null;
    return [s.toUpperCase(), s === "running" ? "ok" : s === "failed" ? "bad" : "warn"];
  }
  function gateBadge() {
    if (val("gnss.gga_gate.enabled") !== "1") return ["OFF", ""];
    return gateHeld() ? ["HOLDING", "bad"] : ["PASSING", "ok"];
  }
  function btBadge() {
    const wl = status.bt.whitelist;
    return Array.isArray(wl) ? [wl.length + " paired", ""] : null;
  }

  // ---------------------------------------------------------------- 设置项通用部分
  /** 注册设置项：apply(v) 把值画到控件上；ctls 为需要统一禁用的控件。 */
  function reg(f, row, ctls, apply) {
    f.row = row;
    fields.push({ f, row, ctls, apply });
    return row;
  }

  function setKey(f, v) {
    if (v === val(f.key)) return;
    request({ t: "set", key: f.key, value: v }, { key: f.key, label: f.label, row: f.row, okText: f.label + ": " + showVal(f, v) + " saved" });
  }

  /** 设置成功提示里显示的值文本：选项取显示名，开关显示 On/Off。 */
  function showVal(f, v) {
    if (f.type === "select") {
      if (v === "0") return "Off";
      const hz = Math.trunc(100000 / Number(v)) / 100;
      return hz + " Hz";
    }
    if (f.type === "seg") return v + " Hz";
    if (f.type === "switch") return v === "1" ? "On" : "Off";
    return v;
  }

  function label(f) {
    return h("span", { class: "lbl" }, [f.label, h("small", { text: f.hint || "" })]);
  }

  const W = {};

  W.switch = (f) => {
    const inp = h("input", { type: "checkbox", role: "switch", onchange: () => setKey(f, inp.checked ? "1" : "0") });
    return reg(f, h("label", { class: "row" }, [label(f), h("span", { class: "sw" }, [inp, h("i")])]), [inp], (v) => { inp.checked = v === "1"; });
  };

  function optionValues(key) {
    return Array.isArray(caps[key]) ? caps[key].map(String) : [];
  }

  W.select = (f) => {
    const sel = h("select", { "aria-label": f.label, onchange: () => setKey(f, sel.value) });
    let shown = "";
    return reg(f, h("div", { class: "row" }, [label(f), sel]), [sel], (v) => {
      const values = optionValues(f.key);
      const signature = values.join(",");
      if (signature !== shown) {
        shown = signature;
        sel.replaceChildren(...values.map((value) => h("option", { value, text: showVal(f, value) })));
      }
      sel.value = v;
    });
  };

  W.seg = (f) => {
    const btns = [];
    const row = h("div", { class: "seg", role: "group", "aria-label": f.label });
    let shown = "";
    return reg(f, row, btns, (v) => {
      const values = optionValues(f.key);
      const signature = values.join(",");
      if (signature !== shown) {
        shown = signature;
        btns.splice(0, btns.length, ...values.map((value) => h("button", {
          type: "button", text: showVal(f, value), onclick: () => setKey(f, value)
        })));
        row.replaceChildren(...btns);
      }
      btns.forEach((b, i) => b.setAttribute("aria-pressed", String(values[i] === v)));
    });
  };

  W.stepper = (f) => {
    const inp = h("input", { type: "number", inputmode: "numeric", min: f.min, max: f.max, step: 1, "aria-label": f.label });
    let timer = null;
    const bounds = () => {
      const domain = caps[f.key];
      if (domain && Number.isInteger(domain.min) && Number.isInteger(domain.max) && domain.min <= domain.max) {
        return domain;
      }
      return f;
    };
    const commit = () => {
      clearTimeout(timer);
      timer = null;
      if (!has(caps, f.key)) return;
      const { min, max } = bounds();
      const n = Number(inp.value);
      if (inp.value === "" || !Number.isInteger(n) || n < min || n > max) {
        toast(f.label + ": enter " + min + "–" + max, "err");
        inp.value = val(f.key) || "";
        return;
      }
      setKey(f, String(n));
    };
    const bump = (d) => {
      const { min, max } = bounds();
      const n = Math.min(max, Math.max(min, (Number(inp.value) || 0) + d));
      inp.value = String(n);
      clearTimeout(timer);
      timer = setTimeout(commit, 700); // 连点时合并为一次设置
    };
    inp.addEventListener("keydown", (e) => { if (e.key === "Enter") inp.blur(); });
    inp.addEventListener("change", commit);
    const minus = h("button", { type: "button", text: "−", "aria-label": "Decrease", onclick: () => bump(-1) });
    const plus = h("button", { type: "button", text: "+", "aria-label": "Increase", onclick: () => bump(1) });
    return reg(f, h("div", { class: "row" }, [label(f), h("div", { class: "step" }, [minus, inp, plus])]), [inp, minus, plus],
      (v) => {
        const { min, max } = bounds();
        inp.min = min;
        inp.max = max;
        if (document.activeElement !== inp && !timer) inp.value = v;
      });
  };

  W.group = (f) => {
    const box = h("div", { class: f.cls || "" }, f.items.map((it) => W[it.type](it)));
    const note = h("p", { class: "note", text: f.empty || "", hidden: true });
    const subs = f.items;
    views.push(() => {
      const none = configured && subs.every((it) => it.row.hidden);
      note.hidden = !none || !f.empty;
      box.hidden = none;
    });
    return h("div", null, [box, note]);
  };

  W.heading = (f) => {
    const title = h("h3", { class: "gh", text: f.text });
    if (f.prefix) views.push(() => { title.hidden = !Object.keys(caps).some((key) => key.startsWith(f.prefix)); });
    return title;
  };

  W.hint = (f) => {
    const p = h("p", { class: "hint" });
    views.push(() => { p.textContent = f.text(); });
    return p;
  };

  W.kv = (f) => {
    const dl = h("dl", { class: "kv" });
    const dds = f.rows.map((r) => {
      const dd = h("dd");
      dl.appendChild(h("dt", { text: r[0] }));
      dl.appendChild(dd);
      return dd;
    });
    views.push(() => f.rows.forEach((r, i) => { dds[i].textContent = r[1](); }));
    return dl;
  };

  W.tiles = () => {
    const mk = (name) => {
      const b = h("b", { text: "--" });
      return [h("div", { class: "tile" }, [h("small", { text: name }), b]), b];
    };
    const t = [mk("Fix"), mk("Satellites"), mk("H. accuracy"), mk("Rate")];
    views.push(() => {
      const g = status.gnss;
      const fx = FIX[g.fix];
      t[0][1].textContent = fx ? fx[0] : "--";
      t[0][1].className = fx ? fx[1] : "";
      t[1][1].textContent = dash(g.sats);
      t[2][1].textContent = typeof g.hacc_mm === "number" ? (g.hacc_mm >= 1000 ? (g.hacc_mm / 1000).toFixed(2) + " m" : g.hacc_mm + " mm") : "--";
      t[3][1].textContent = dash(g.hz, " Hz");
    });
    return h("div", { class: "tiles" }, t.map((x) => x[0]));
  };

  /** 二次确认按钮：第一次点击进入 3 s 倒计时，期间再点才执行。 */
  function confirmBtn(text, cls, fire) {
    let left = 0;
    let timer = null;
    const b = h("button", { type: "button", class: "btn full " + cls, text });
    const reset = () => { clearInterval(timer); timer = null; b.textContent = text; b.classList.remove("arm"); };
    b.addEventListener("click", () => {
      if (!timer) {
        left = 3;
        b.classList.add("arm");
        b.textContent = "Tap again to confirm (" + left + ")";
        timer = setInterval(() => {
          left -= 1;
          if (left <= 0) reset();
          else b.textContent = "Tap again to confirm (" + left + ")";
        }, 1000);
        return;
      }
      reset();
      fire();
    });
    return b;
  }

  function command(name, btn, okText) {
    request({ t: "cmd", name }, { btn, okText, ok: () => { leaving = true; banner("The receiver is restarting. You can close this page."); } });
  }

  W.action = (f) => {
    const b = confirmBtn(f.text, f.cls, () => command(f.cmd, b, "Restarting…"));
    views.push(() => { b.disabled = !ready || leaving; });
    return h("div", null, [b, h("p", { class: "hint", text: f.hint })]);
  };

  /** MAC 输入：去掉非十六进制字符、转大写、每两位补冒号。 */
  function fmtMac(s) {
    const hex = s.toUpperCase().replace(/[^0-9A-F]/g, "").slice(0, 12);
    return hex.replace(/(..)(?=.)/g, "$1:");
  }

  W.whitelist = () => {
    const list = h("ul", { class: "list" });
    const inp = h("input", { class: "in", placeholder: "AA:BB:CC:DD:EE:FF", maxlength: 17, autocapitalize: "characters", autocomplete: "off", spellcheck: "false", "aria-label": "Phone Bluetooth MAC" });
    const add = h("button", { type: "button", class: "btn pri", text: "Add" });
    const tip = h("p", { class: "hint", text: "Bluetooth MAC address of the phone, 12 hex digits." });
    let shown = "";
    inp.addEventListener("input", () => {
      inp.value = fmtMac(inp.value);
      inp.classList.toggle("bad", inp.value.length === 17 && !MAC_RE.test(inp.value));
      add.disabled = !ready || leaving || !MAC_RE.test(inp.value);
    });
    const doAdd = () => {
      const mac = inp.value;
      if (!MAC_RE.test(mac)) return;
      if ((status.bt.whitelist || []).indexOf(mac) >= 0) { toast("Already in the whitelist", "warn"); return; }
      request({ t: "set", key: "bt.wl.add", value: mac }, { btn: add, label: "Whitelist", okText: mac + " added", ok: () => { inp.value = ""; add.disabled = true; } });
    };
    add.addEventListener("click", doAdd);
    inp.addEventListener("keydown", (e) => { if (e.key === "Enter") doAdd(); });
    views.push(() => {
      const wl = status.bt.whitelist;
      const sig = JSON.stringify(wl || null) + ready;
      add.disabled = !ready || leaving || !MAC_RE.test(inp.value);
      inp.disabled = !ready || leaving;
      if (sig === shown) return;
      shown = sig;
      list.textContent = "";
      if (!Array.isArray(wl)) list.appendChild(h("li", { class: "empty", text: "--" }));
      else if (!wl.length) list.appendChild(h("li", { class: "empty", text: "Empty — any phone may connect while pairing is open." }));
      else wl.forEach((mac) => {
        const rm = h("button", { type: "button", class: "btn sm danger", text: "Remove", "aria-label": "Remove " + mac });
        rm.addEventListener("click", () => request({ t: "set", key: "bt.wl.del", value: mac }, { btn: rm, label: "Whitelist", okText: mac + " removed" }));
        list.appendChild(h("li", null, [h("span", { text: mac }), rm]));
      });
    });
    return h("div", null, [list, h("div", { class: "line" }, [inp, add]), tip]);
  };

  W.factory = () => {
    const tok = h("input", { class: "in", type: "password", placeholder: "Unlock token", autocomplete: "off", "aria-label": "Factory unlock token" });
    const unlockBtn = h("button", { type: "button", class: "btn pri", text: "Unlock" });
    const doUnlock = () => {
      if (!tok.value) return;
      request({ t: "unlock", token: tok.value }, { btn: unlockBtn, okText: "Factory functions unlocked", ok: () => { unlocked = true; tok.value = ""; } });
    };
    unlockBtn.addEventListener("click", doUnlock);
    tok.addEventListener("keydown", (e) => { if (e.key === "Enter") doUnlock(); });
    const lockedBox = h("div", null, [h("p", { class: "hint", text: "Enter the factory token to change the serial number or reset the receiver. Unlock ends when this page disconnects." }), h("div", { class: "line" }, [tok, unlockBtn])]);

    const sn = h("input", { class: "in", maxlength: 16, placeholder: "Serial number", autocapitalize: "characters", autocomplete: "off", "aria-label": "Serial number" });
    const snBtn = h("button", { type: "button", class: "btn pri", text: "Write" });
    const snTip = h("p", { class: "hint", text: "1–16 characters: letters, digits and '-'." });
    sn.addEventListener("input", () => {
      sn.classList.toggle("bad", sn.value !== "" && !SN_RE.test(sn.value));
      snBtn.disabled = !SN_RE.test(sn.value) || sn.value === cfgVal("factory.sn");
    });
    snBtn.addEventListener("click", () => {
      if (!SN_RE.test(sn.value)) return;
      request({ t: "set", key: "factory.sn", value: sn.value }, { btn: snBtn, label: "Serial number", okText: "Serial number written" });
    });
    const reset = confirmBtn("Factory reset", "danger", () => command("freset", reset, "Factory reset started"));
    const openBox = h("div", null, [h("h3", { class: "gh", text: "Serial number" }), h("div", { class: "line" }, [sn, snBtn]), snTip,
      h("h3", { class: "gh", text: "Reset" }), reset, h("p", { class: "hint", text: "Erases all settings and restarts the receiver." })]);

    views.push(() => {
      const off = !ready || leaving;
      lockedBox.hidden = unlocked;
      openBox.hidden = !unlocked;
      tok.disabled = unlockBtn.disabled = off;
      if (document.activeElement !== sn && !sn.value) sn.value = cfgVal("factory.sn") || "";
      sn.disabled = off;
      snBtn.disabled = off || !SN_RE.test(sn.value) || sn.value === cfgVal("factory.sn");
      reset.disabled = off;
    });
    return h("div", null, [lockedBox, openBox]);
  };

  // ---------------------------------------------------------------- 卡片
  function buildCards() {
    SCHEMA.forEach((c) => {
      const badge = h("span", { class: "badge", hidden: true });
      const card = h("details", { class: "card", id: "card-" + c.id }, [
        h("summary", null, [svg(ICONS[c.icon], "ic"), h("h2", { text: c.title }), badge, svg("M6 9l6 6l6-6", "chev")]),
        h("div", { class: "body" }, c.items.map((it) => W[it.type](it)))
      ]);
      const saved = store("rtk.card." + c.id);
      card.open = saved === null ? !c.closed : saved === "1";
      card.addEventListener("toggle", () => store("rtk.card." + c.id, card.open ? "1" : "0"));
      if (c.badge) {
        views.push(() => {
          const b = c.badge();
          badge.hidden = !b;
          if (b) { badge.textContent = b[0]; badge.className = "badge " + b[1]; }
        });
      }
      byId("col-" + c.col).appendChild(card);
    });
  }

  // ---------------------------------------------------------------- 刷新
  function refresh() {
    const off = !ready || leaving;
    document.body.classList.toggle("offline", !ready && !leaving);
    fields.forEach((x) => {
      const v = val(x.f.key);
      x.row.hidden = !has(caps, x.f.key) || (configured && v === undefined);
      if (v !== undefined) x.apply(v);
      const unsup = !!unsupported[x.f.key];
      const busy = keyBusy(x.f.key);
      x.row.classList.toggle("unsup", unsup);
      x.row.classList.toggle("pending", busy);
      x.ctls.forEach((c) => { c.disabled = off || unsup || busy || v === undefined; });
    });
    views.forEach((fn) => fn());
    topBar();
  }

  function pill(id, text, cls) {
    const p = byId(id);
    p.className = "pill " + (cls || "");
    p.lastChild.textContent = text;
  }

  function topBar() {
    const g = status.gnss;
    const fx = FIX[g.fix];
    pill("p-fix", "Fix " + (fx ? fx[0] : "--"), fx ? fx[1] : "");
    pill("p-sats", "Sats " + dash(g.sats), "");
    byId("p-gate").hidden = !gateHeld();
    byId("dev-name").textContent = hello.bt_name || "--";
  }

  function setConn(kind, text) {
    pill("p-conn", text, kind);
    byId("p-conn").firstChild.className = "dot";
  }

  function banner(text) {
    const b = byId("banner");
    b.hidden = !text;
    b.textContent = text || "";
  }

  function toast(msg, kind) {
    const box = byId("toasts");
    while (box.childNodes.length >= 3) box.removeChild(box.firstChild);
    const t = h("div", { class: "toast " + (kind || ""), text: msg, role: kind === "err" ? "alert" : "status" });
    box.appendChild(t);
    setTimeout(() => { if (t.parentNode) t.parentNode.removeChild(t); }, kind === "err" ? 4500 : 3000);
  }

  // ---------------------------------------------------------------- 请求队列
  function request(payload, o) {
    if (!ready || leaving) { toast("Not connected", "err"); return; }
    if (o.key) optimistic[o.key] = payload.value;
    if (o.btn) o.btn.disabled = true;
    queue.push({ payload, o });
    refresh();
    pump();
  }

  function pump() {
    if (inflight || !queue.length || !ws || ws.readyState !== 1) return;
    const r = queue.shift();
    reqId = (reqId % 65535) + 1;
    r.payload.id = reqId;
    const raw = JSON.stringify(r.payload);
    if (utf8Len(raw) > MAX_FRAME) { settle(r, "too_long"); pump(); return; }
    inflight = r;
    ackTimer = setTimeout(() => {
      if (inflight !== r) return;
      inflight = null;
      settle(r, "timeout");
      pump();
    }, ACK_MS);
    try { ws.send(raw); } catch (_e) { clearTimeout(ackTimer); inflight = null; settle(r, "disconnected"); }
  }

  /** 结束一条请求：失败则丢弃乐观值（控件回到 config 值）并提示。 */
  function settle(r, err, quiet) {
    const o = r.o;
    if (o.btn) o.btn.disabled = false;
    if (err) {
      if (o.key) {
        delete optimistic[o.key];
        if (err === "unsupported") unsupported[o.key] = true;
      }
      if (!quiet) toast((o.label ? o.label + ": " : "") + (ERR[err] || "Failed (" + err + ")"), "err");
    } else {
      // 成功：先把值写入本地 config，随后设备推送的 config 帧再覆盖为真值。
      if (o.key && has(optimistic, o.key) && !keyBusy(o.key)) {
        setCfg(o.key, optimistic[o.key]);
        delete optimistic[o.key];
      }
      if (o.okText) toast(o.okText, "ok");
      if (o.ok) o.ok();
    }
    refresh();
  }

  function dropAll() {
    clearTimeout(ackTimer);
    const all = (inflight ? [inflight] : []).concat(queue.splice(0));
    inflight = null;
    all.forEach((r) => settle(r, "disconnected", true));
    if (all.length) toast("Connection lost, pending changes were not applied", "err");
  }

  // ---------------------------------------------------------------- 消息
  function onMessage(m) {
    switch (m.t) {
      case "hello":
        hello = m;
        caps = m.caps || {};
        unlocked = !!m.factory;
        ready = true;
        attempt = 0;
        banner("");
        setConn("ok", "Connected");
        break;
      case "caps":
        caps = m.caps || {};
        Object.keys(unsupported).forEach((key) => delete unsupported[key]);
        break;
      case "config":
        config = m;
        configured = true;
        Object.keys(optimistic).forEach((k) => { if (!keyBusy(k)) delete optimistic[k]; });
        break;
      case "status":
        // 只带变化的组；组内缺失字段表示不可得，所以整组替换而不是合并。
        ["dev", "gnss", "bt"].forEach((g) => { if (m[g] && typeof m[g] === "object") status[g] = m[g]; });
        break;
      case "ack": {
        if (!inflight || m.id !== inflight.payload.id) return; // 迟到的 ack（已超时）忽略
        clearTimeout(ackTimer);
        const r = inflight;
        inflight = null;
        settle(r, m.ok ? null : (m.err || "failed"));
        pump();
        return;
      }
      case "bye":
        leaving = true;
        dropAll();
        banner(m.reason === "freset" ? "Factory reset done. The receiver is restarting; you can close this page."
          : m.reason === "idle" ? "Closed after inactivity. The receiver is back in Bluetooth mode."
            : "The receiver is returning to Bluetooth mode. You can close this page.");
        setConn("", "Closed");
        if (ws) ws.close();
        break;
      default:
        return;
    }
    refresh();
  }

  // ---------------------------------------------------------------- 连接
  function connect() {
    clearTimeout(retryTimer);
    if (leaving) return;
    setConn("warn", attempt ? "Reconnecting" : "Connecting");
    const host = location.host || "192.168.10.12";
    let s;
    try {
      s = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + host + "/ws");
    } catch (_e) {
      retry();
      return;
    }
    ws = s;
    s.onmessage = (e) => {
      let m;
      try { m = JSON.parse(e.data); } catch (_e) { return; }
      if (m && typeof m.t === "string") onMessage(m);
    };
    s.onerror = () => { try { s.close(); } catch (_e) { /* 已关闭 */ } };
    s.onclose = () => {
      if (ws !== s) return;
      ws = null;
      ready = false;
      unlocked = false; // 协议：断开即失效
      dropAll();
      if (!leaving) retry();
      refresh();
    };
  }

  function retry() {
    const d = BACKOFF_S[Math.min(attempt, BACKOFF_S.length - 1)];
    attempt += 1;
    setConn("bad", "Offline · retry in " + d + " s");
    banner("Connection to the receiver lost. Check that this phone is still on the receiver's Wi-Fi.");
    retryTimer = setTimeout(connect, d * 1000);
  }

  buildCards();
  refresh();
  connect();
})();
