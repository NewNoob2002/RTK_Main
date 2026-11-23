// End-to-end headless verification of main/web/ui/app.js against live tools/web_mock/web_mock.py
// Hardened with script-relative paths, per-operation timeouts, process cleanup, and a global watchdog.
import { spawn } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(SCRIPT_DIR, "../..");
const APP_JS = fs.readFileSync(path.join(REPO, "main/web/ui/app.js"), "utf8");
const APP_CSS = fs.readFileSync(path.join(REPO, "main/web/ui/app.css"), "utf8");
const MOCK_PY = path.join(SCRIPT_DIR, "web_mock.py");

const GLOBAL_TIMEOUT_MS = 60000;
let currentCase = "init";
const activeProcs = new Set();

function dumpActiveProcs() {
  return [...activeProcs].map((p) => ({
    pid: p.pid,
    killed: p.killed,
    exitCode: p.exitCode,
    signalCode: p.signalCode,
  }));
}

function killAllProcs() {
  for (const p of activeProcs) {
    if (p.exitCode === null && p.signalCode === null) {
      try { p.kill("SIGKILL"); } catch {}
    }
  }
}

const globalWatchdog = setTimeout(() => {
  console.error(`GLOBAL WATCHDOG TIMEOUT (${GLOBAL_TIMEOUT_MS}ms) in case: ${currentCase}`);
  console.error("Active child processes:", JSON.stringify(dumpActiveProcs()));
  killAllProcs();
  process.exit(2);
}, GLOBAL_TIMEOUT_MS);

function buildBin(version, project = "rtk_main", totalSize = 4096, opts = {}) {
  const {
    imgMagic = 0xE9,
    descMagic = 0xABCD5432,
    chipId = 0x0000,
    rawVersion = null,
    rawProject = null,
  } = opts;
  const buf = Buffer.alloc(Math.max(totalSize, 288));
  buf[0] = imgMagic & 0xFF;
  buf.writeUInt16LE(chipId & 0xFFFF, 12);
  buf.writeUInt32LE(descMagic >>> 0, 32);
  const vb = rawVersion ? Buffer.from(rawVersion) : Buffer.from(version, "utf8");
  const pb = rawProject ? Buffer.from(rawProject) : Buffer.from(project, "utf8");
  vb.subarray(0, 32).copy(buf, 48);
  pb.subarray(0, 32).copy(buf, 80);
  return buf.subarray(0, totalSize);
}

function makeFakeFile(buf, name = "rtk_main.bin") {
  return {
    name,
    size: buf.length,
    _buf: buf,
    slice(start = 0, end = buf.length) {
      return { _buf: buf.subarray(start, end) };
    },
  };
}

class FakeNode {
  constructor(tag, isText = false) {
    this.tagName = isText ? "#text" : tag.toUpperCase();
    this.isText = isText;
    this.attrs = {};
    this.style = {};
    this.children = [];
    this.parentNode = null;
    this.listeners = {};
    this._text = "";
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = "";
    this.open = false;
    this.files = [];
    const self = this;
    this.classList = {
      add(c) {
        const s = new Set(self.className.split(/\s+/).filter(Boolean));
        s.add(c);
        self.className = [...s].join(" ");
      },
      remove(c) {
        const s = new Set(self.className.split(/\s+/).filter(Boolean));
        s.delete(c);
        self.className = [...s].join(" ");
      },
      toggle(c, force) {
        const has = self.className.split(/\s+/).filter(Boolean).includes(c);
        const on = force === undefined ? !has : !!force;
        if (on) this.add(c);
        else this.remove(c);
      },
      contains(c) {
        return self.className.split(/\s+/).filter(Boolean).includes(c);
      },
    };
  }
  get id() { return this.attrs.id || ""; }
  set id(v) { this.attrs.id = v; }
  get childNodes() { return this.children; }
  get firstChild() { return this.children[0] || null; }
  get lastChild() { return this.children[this.children.length - 1] || null; }
  get textContent() {
    if (this.isText) return this._text;
    if (this.children.length === 0) return this._text;
    return this.children.map((c) => c.textContent).join("");
  }
  set textContent(v) {
    this._text = String(v);
    if (!this.isText) this.children = [];
  }
  setAttribute(k, v) {
    this.attrs[k] = String(v);
    if (k === "class") this.className = String(v);
    if (k === "hidden") this.hidden = true;
    if (k === "disabled") this.disabled = true;
  }
  getAttribute(k) { return this.attrs[k]; }
  appendChild(c) {
    c.parentNode = this;
    this.children.push(c);
    return c;
  }
  removeChild(c) {
    const i = this.children.indexOf(c);
    if (i >= 0) { this.children.splice(i, 1); c.parentNode = null; }
    return c;
  }
  replaceChildren(...nodes) {
    this.children = [];
    for (const n of nodes) this.appendChild(n);
  }
  addEventListener(ev, fn) {
    (this.listeners[ev] ||= []).push(fn);
  }
  dispatch(ev, evtObj = {}) {
    for (const fn of this.listeners[ev] || []) fn(evtObj);
  }
  click() {
    if (!this.disabled) this.dispatch("click", {});
  }
  querySelectorAll(sel) {
    const out = [];
    const match = (n) => {
      if (n.isText) return false;
      if (sel.startsWith(".")) return n.classList.contains(sel.slice(1));
      return n.tagName.toLowerCase() === sel.toLowerCase();
    };
    const walk = (n) => {
      for (const c of n.children) {
        if (match(c)) out.push(c);
        walk(c);
      }
    };
    walk(this);
    return out;
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
}

function startMock(extraArgs = [], timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    const proc = spawn("python3", ["-I", MOCK_PY, "--host", "127.0.0.1", "--port", "0", ...extraArgs], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    activeProcs.add(proc);
    proc.once("exit", () => activeProcs.delete(proc));

    let done = false;
    let out = "";
    let err = "";
    const finish = (fn, val) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      proc.stdout.off("data", onData);
      proc.off("error", onError);
      proc.off("exit", onExit);
      fn(val);
    };
    const onData = (d) => {
      out += d.toString();
      const m = out.match(/http:\/\/127\.0\.0\.1:(\d+)\//);
      if (m) finish(resolve, { proc, port: Number(m[1]) });
    };
    const onError = (e) => {
      finish(reject, new Error(`startMock spawn error in ${currentCase}: ${e.message}`));
    };
    const onExit = (code, sig) => {
      finish(reject, new Error(`startMock exited early in ${currentCase} (code=${code}, sig=${sig}): ${err}`));
    };
    const timer = setTimeout(() => {
      try { proc.kill("SIGKILL"); } catch {}
      finish(reject, new Error(`startMock timeout (${timeoutMs}ms) in ${currentCase}; out=${out !== "" ? out : "<empty>"} err=${err}`));
    }, timeoutMs);

    proc.stdout.on("data", onData);
    proc.stderr.on("data", (d) => { err += d.toString(); });
    proc.on("error", onError);
    proc.on("exit", onExit);
  });
}

function stopMock(proc, timeoutMs = 4000) {
  return new Promise((resolve, reject) => {
    if (!proc || proc.exitCode !== null || proc.signalCode !== null) {
      activeProcs.delete(proc);
      return resolve();
    }
    let done = false;
    const cleanup = () => {
      clearTimeout(killTimer);
      clearTimeout(failTimer);
      proc.off("exit", onDone);
      proc.off("error", onDone);
      activeProcs.delete(proc);
    };
    const onDone = () => {
      if (done) return;
      done = true;
      cleanup();
      resolve();
    };
    const killTimer = setTimeout(() => {
      if (!done && proc.exitCode === null && proc.signalCode === null) {
        try { proc.kill("SIGKILL"); } catch {}
      }
    }, Math.max(200, Math.floor(timeoutMs / 2)));
    const failTimer = setTimeout(() => {
      if (done) return;
      done = true;
      cleanup();
      reject(new Error(`stopMock timeout (${timeoutMs}ms) in ${currentCase}: pid=${proc.pid} exit=${proc.exitCode} sig=${proc.signalCode}`));
    }, timeoutMs);

    proc.once("exit", onDone);
    proc.once("error", onDone);
    try { proc.kill("SIGTERM"); } catch { onDone(); }
  });
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

function createDomEnv(port, opts = {}) {
  const root = new FakeNode("body");
  const ids = {};
  const mkId = (tag, id, kids = []) => {
    const n = new FakeNode(tag);
    n.id = id;
    ids[id] = n;
    for (const k of kids) n.appendChild(k);
    root.appendChild(n);
    return n;
  };
  mkId("span", "dev-name");
  mkId("span", "p-conn", [new FakeNode("i"), new FakeNode("span")]);
  mkId("span", "p-fix", [new FakeNode("span")]);
  mkId("span", "p-sats", [new FakeNode("span")]);
  const pGate = mkId("span", "p-gate"); pGate.hidden = true;
  const banner = mkId("div", "banner"); banner.hidden = true;
  mkId("div", "col-a");
  mkId("div", "col-b");
  mkId("div", "toasts");

  const timers = new Set();
  const intervals = new Set();
  const wsInstances = [];
  const xhrInstances = [];
  const xhrProgressEvents = [];
  const activeReqs = new Set();
  const stateOpts = { delayFileReaderMs: opts.delayFileReaderMs || 0 };

  class MockFileReader {
    readAsArrayBuffer(blob) {
      const deliver = () => {
        const b = blob._buf;
        this.result = b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
        if (this.onload) this.onload();
      };
      if (stateOpts.delayFileReaderMs > 0) {
        const t = setTimeout(() => { timers.delete(t); deliver(); }, stateOpts.delayFileReaderMs);
        timers.add(t);
      } else {
        queueMicrotask(deliver);
      }
    }
  }

  class MockXHR {
    constructor() {
      this.headers = {};
      this.upload = {};
      this.status = 0;
      this.responseText = "";
      this.timeout = 0;
      this._aborted = false;
      this._finished = false;
      this._req = null;
      this._chunkTimer = null;
      this._toTimer = null;
      xhrInstances.push(this);
    }
    open(method, url) {
      this.method = method;
      this.url = url;
    }
    setRequestHeader(k, v) { this.headers[k] = v; }
    abort() {
      if (this._finished || this._aborted) return;
      this._aborted = true;
      clearTimeout(this._chunkTimer);
      clearTimeout(this._toTimer);
      if (this._req) {
        try { this._req.destroy(); } catch {}
        activeReqs.delete(this._req);
        this._req = null;
      }
      if (this.onabort) this.onabort();
    }
    send(body) {
      const payload = body && body._buf ? body._buf : (body ? Buffer.from(body) : null);
      if (opts.forceXhrNetworkErr && this.url === "/ota") {
        const t = setTimeout(() => { timers.delete(t); if (!this._aborted && this.onerror) this.onerror(new Error("net")); }, 20);
        timers.add(t);
        return;
      }
      if (opts.stallOtaPost && this.url === "/ota") {
        if (payload && this.upload.onprogress) {
          const ev = { lengthComputable: true, loaded: payload.length, total: payload.length };
          xhrProgressEvents.push(ev);
          this.upload.onprogress(ev);
        }
        if (opts.fireXhrTimeoutMs) {
          this._toTimer = setTimeout(() => {
            timers.delete(this._toTimer);
            if (!this._aborted && !this._finished) {
              this._finished = true;
              if (this.ontimeout) this.ontimeout();
            }
          }, opts.fireXhrTimeoutMs);
          timers.add(this._toTimer);
        }
        return;
      }

      const reqHeaders = { ...this.headers };
      if (payload) reqHeaders["Content-Length"] = String(payload.length);
      const req = http.request({
        hostname: "127.0.0.1",
        port,
        path: this.url,
        method: this.method,
        headers: reqHeaders,
      }, (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          activeReqs.delete(req);
          clearTimeout(this._toTimer);
          if (this._aborted || this._finished) return;
          this._finished = true;
          this.status = res.statusCode;
          this.responseText = Buffer.concat(chunks).toString("utf8");
          if (this.onload) this.onload();
        });
      });
      this._req = req;
      activeReqs.add(req);
      const effTimeout = this.timeout > 0 ? Math.min(this.timeout, 15000) : 15000;
      req.setTimeout(effTimeout, () => {
        if (this._aborted || this._finished) return;
        this._finished = true;
        try { req.destroy(); } catch {}
        activeReqs.delete(req);
        if (this.ontimeout) this.ontimeout();
        else if (this.onerror) this.onerror(new Error("http timeout"));
      });
      req.on("error", () => {
        activeReqs.delete(req);
        clearTimeout(this._toTimer);
        if (this._aborted || this._finished) return;
        this._finished = true;
        if (this.onerror) this.onerror(new Error("req error"));
      });
      if (payload) {
        const chunkSz = opts.uploadChunkSize || 2048;
        let off = 0;
        const sendNext = () => {
          if (this._aborted || this._finished) return;
          if (off >= payload.length) {
            req.end();
            return;
          }
          const slice = payload.subarray(off, Math.min(off + chunkSz, payload.length));
          off += slice.length;
          const ok = req.write(slice);
          if (this.upload.onprogress) {
            const ev = { lengthComputable: true, loaded: off, total: payload.length };
            xhrProgressEvents.push(ev);
            this.upload.onprogress(ev);
          }
          if (ok) {
            this._chunkTimer = setTimeout(sendNext, opts.uploadChunkDelayMs || 0);
            timers.add(this._chunkTimer);
          } else {
            req.once("drain", sendNext);
          }
        };
        sendNext();
      } else {
        req.end();
      }
    }
  }

  class WrappedWS extends globalThis.WebSocket {
    constructor(url) {
      super(url);
      wsInstances.push(this);
      if (opts.holdAckCmd) {
        const origSend = this.send.bind(this);
        this._heldFrames = [];
        this.send = (data) => {
          try {
            const parsed = JSON.parse(data);
            if (parsed.t === "cmd" && parsed.name === opts.holdAckCmd) {
              this._heldFrames.push(parsed);
              return; // Hold on wire so ACK stays pending until test releases or times out
            }
          } catch {}
          return origSend(data);
        };
      }
    }
  }

  const document = {
    body: root,
    activeElement: null,
    getElementById: (id) => ids[id] || null,
    createElement: (tag) => new FakeNode(tag),
    createElementNS: (_ns, tag) => new FakeNode(tag),
    createTextNode: (t) => { const n = new FakeNode("", true); n.textContent = t; return n; },
  };
  const scaleMs = (ms) => (opts.longTimerScale && ms >= 10000 ? Math.max(20, Math.round(ms * opts.longTimerScale)) : ms);
  const win = {
    document,
    location: { host: `127.0.0.1:${port}`, protocol: "http:" },
    localStorage: { getItem: () => null, setItem: () => {} },
    FileReader: MockFileReader,
    XMLHttpRequest: MockXHR,
    WebSocket: WrappedWS,
    TextDecoder: opts.disableTextDecoder ? undefined : TextDecoder,
    TextEncoder: opts.disableTextDecoder ? undefined : TextEncoder,
    Uint8Array,
    DataView,
    ArrayBuffer,
    setTimeout: (fn, ms) => { const t = setTimeout(() => { timers.delete(t); fn(); }, scaleMs(ms)); timers.add(t); return t; },
    clearTimeout: (t) => { clearTimeout(t); timers.delete(t); },
    setInterval: (fn, ms) => { const t = setInterval(fn, ms); intervals.add(t); return t; },
    clearInterval: (t) => { clearInterval(t); intervals.delete(t); },
    encodeURIComponent,
    unescape,
  };
  win.window = win;
  const ctx = vm.createContext(win);
  vm.runInContext(APP_JS, ctx, { filename: "app.js" });

  function cleanup() {
    for (const t of timers) clearTimeout(t);
    for (const t of intervals) clearInterval(t);
    for (const r of activeReqs) { try { r.destroy(); } catch {} }
    for (const s of wsInstances) { try { s.close(); } catch {} }
  }

  return { root, ids, wsInstances, xhrInstances, xhrProgressEvents, stateOpts, cleanup };
}

function assert(cond, msg) {
  if (!cond) throw new Error(`ASSERT FAIL [${currentCase}]: ${msg}`);
}

async function waitUntil(pred, timeoutMs = 4000, msg = "timeout") {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    if (pred()) return;
    await sleep(15);
  }
  throw new Error(`WAIT TIMEOUT [${currentCase}]: ${msg}`);
}

function extractParsedVersion(otaCard) {
  const m = otaCard.textContent.match(/当前 0\.1\.0-mock → 新 (.*)（/);
  assert(m, `expected parsed version transition in card text: ${otaCard.textContent}`);
  return m[1];
}

async function runTests() {
  // 1. Legacy firmware (--no-ota -> 404)
  currentCase = "1_legacy_no_ota";
  {
    const m = await startMock(["--no-ota"]);
    const env = createDomEnv(m.port);
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("设备不支持网页升级"), 3000, "unsupported msg");
      await waitUntil(() => env.ids["p-conn"].textContent.includes("Connected"), 3000, "ws connected");
      const sysCard = env.ids["col-b"].children.find((c) => c.id === "card-sys");
      assert(!sysCard.querySelector("button").disabled, "other cards remain enabled when OTA unsupported");
      console.log("PASS 1: Legacy firmware 404 -> '设备不支持网页升级', other cards unaffected");
    } finally {
      env.cleanup();
      await stopMock(m.proc);
    }
  }

  // 2. Parsing edge vectors (short 287/288 B, BOM, invalid & truncated UTF-8 with & without TextDecoder, 32 B no-NUL, oversized, same-ver) + re-select file lock
  currentCase = "2_parsing_and_reselect";
  {
    const m = await startMock(["--ota-max", "16384"]);
    const env = createDomEnv(m.port);
    const envNoTd = createDomEnv(m.port, { disableTextDecoder: true });
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      const otaCardNoTd = envNoTd.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock") && otaCardNoTd.textContent.includes("0.1.0-mock"), 3000, "info loaded");
      const dds = otaCard.querySelectorAll("dd").map((d) => d.textContent);
      assert(dds[0] === "0.1.0-mock" && dds[1] === "ota_0" && dds[2].includes("valid") && dds[3] === "无", `bad initial kv: ${JSON.stringify(dds)}`);

      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      const fileInpNoTd = otaCardNoTd.querySelector("input");
      assert(uploadBtn.disabled, "upload btn disabled before file pick");

      const pick = async (inp, buf, delayMs = 30) => {
        inp.files = [makeFakeFile(buf)];
        inp.dispatch("change");
        await sleep(delayMs);
      };

      // 2a. Short 287 B & exact 288 B
      await pick(fileInp, buildBin("6.2.0", "rtk_main", 287));
      assert(otaCard.textContent.includes("不是本设备固件") && uploadBtn.disabled, "287 B rejected");
      await pick(fileInp, buildBin("6.2.0", "rtk_main", 288));
      assert(otaCard.textContent.includes("不是本设备固件") && uploadBtn.disabled, "288 B rejected");

      // 2b. Bad magic / desc / project / BOM+project
      await pick(fileInp, buildBin("6.2.0", "rtk_main", 4096, { imgMagic: 0x00 }));
      assert(otaCard.textContent.includes("不是本设备固件") && uploadBtn.disabled, "bad 0xE9 rejected");
      await pick(fileInp, buildBin("6.2.0", "rtk_main", 4096, { descMagic: 0x11223344 }));
      assert(otaCard.textContent.includes("不是本设备固件") && uploadBtn.disabled, "bad desc magic rejected");
      await pick(fileInp, buildBin("6.2.0", "wrong_proj", 4096));
      assert(otaCard.textContent.includes("不是本设备固件") && uploadBtn.disabled, "wrong project rejected");
      await pick(fileInp, buildBin("6.2.0", "", 4096, { rawProject: Buffer.concat([Buffer.from([0xEF, 0xBB, 0xBF]), Buffer.from("rtk_main")]) }));
      assert(otaCard.textContent.includes("不是本设备固件") && uploadBtn.disabled, "BOM+project must be rejected");
      await pick(fileInpNoTd, buildBin("6.2.0", "", 4096, { rawProject: Buffer.concat([Buffer.from([0xEF, 0xBB, 0xBF]), Buffer.from("rtk_main")]) }));
      assert(otaCardNoTd.textContent.includes("不是本设备固件"), "BOM+project must be rejected without TextDecoder");

      // 2c. UTF-8 parity between native TextDecoder and fallback: invalid bytes, truncated multi-byte sequences,
      //     non-continuation bytes after valid prefixes, overlong/surrogate/out-of-range, valid multi-byte + BOM
      const utf8Vectors = [
        { name: "single_0xFF", bytes: [0xFF, 0x31, 0x2E, 0x32], expected: "\uFFFD1.2" },
        { name: "trunc_3b_eof", bytes: [0x76, 0x31, 0xE2, 0x82], expected: "v1\uFFFD" },
        { name: "trunc_3b_ascii", bytes: [0xE2, 0x82, 0x41], expected: "\uFFFDA" },
        { name: "trunc_4b_eof", bytes: [0xF0, 0x9F, 0x98], expected: "\uFFFD" },
        { name: "trunc_4b_ascii", bytes: [0xF0, 0x9F, 0x2E, 0x31], expected: "\uFFFD.1" },
        { name: "trunc_4b_then_valid_3b", bytes: [0xF0, 0x9F, 0xE2, 0x82, 0xAC], expected: "\uFFFD\u20AC" },
        { name: "overlong_3b", bytes: [0xE0, 0x80, 0x80], expected: "\uFFFD\uFFFD\uFFFD" },
        { name: "surrogate_3b", bytes: [0xED, 0xA0, 0x80], expected: "\uFFFD\uFFFD\uFFFD" },
        { name: "above_u10ffff", bytes: [0xF4, 0x90, 0x80, 0x80], expected: "\uFFFD\uFFFD\uFFFD\uFFFD" },
        { name: "bom_and_valid_multibyte", bytes: [0xEF, 0xBB, 0xBF, 0xC3, 0xA9, 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x9A, 0x80], expected: "\uFEFF\u00E9\u4E2D\u{1F680}" },
      ];
      for (const vec of utf8Vectors) {
        const bin = buildBin("", "rtk_main", 4096, { rawVersion: Buffer.from(vec.bytes) });
        await pick(fileInp, bin);
        await pick(fileInpNoTd, bin);
        const gotNative = extractParsedVersion(otaCard);
        const gotFallback = extractParsedVersion(otaCardNoTd);
        assert(gotNative === vec.expected && !uploadBtn.disabled, `native UTF-8 [${vec.name}]: got=${JSON.stringify(gotNative)} want=${JSON.stringify(vec.expected)}`);
        assert(gotFallback === vec.expected, `fallback UTF-8 [${vec.name}]: got=${JSON.stringify(gotFallback)} want=${JSON.stringify(vec.expected)}`);
        assert(gotFallback === gotNative, `native vs fallback mismatch [${vec.name}]`);
      }

      // 2d. 32 B without NUL in version -> bounded to 32 chars, no bleed into project_name
      const v32 = "V".repeat(32);
      await pick(fileInp, buildBin("", "rtk_main", 4096, { rawVersion: Buffer.from(v32, "ascii") }));
      assert(otaCard.textContent.includes(`新 ${v32}（`) && !otaCard.textContent.includes(`${v32}rtk_main`), `32 B no-NUL bleed: ${otaCard.textContent}`);

      // 2e. Oversized & same version
      await pick(fileInp, buildBin("6.2.0", "rtk_main", 20000));
      assert(otaCard.textContent.includes("文件过大") && uploadBtn.disabled, "oversized rejected");
      await pick(fileInp, buildBin("0.1.0-mock", "rtk_main", 4096));
      assert(otaCard.textContent.includes("当前 0.1.0-mock → 新 0.1.0-mock") && !uploadBtn.disabled, "same version shown");
      assert(otaCard.querySelectorAll(".ota-msg").some((e) => !e.hidden && e.textContent === "版本相同"), "same version hint shown");

      // 2f. Finding 9: re-selecting file MUST disable uploadBtn immediately before FileReader finishes
      env.stateOpts.delayFileReaderMs = 100;
      fileInp.files = [makeFakeFile(buildBin("6.2.1", "rtk_main", 4096))];
      fileInp.dispatch("change");
      assert(uploadBtn.disabled, "uploadBtn MUST be disabled immediately on re-select before FileReader finishes");
      uploadBtn.click(); // Must not arm while disabled
      assert(!uploadBtn.textContent.includes("将替换当前固件"), "disabled uploadBtn must not arm");
      await waitUntil(() => !uploadBtn.disabled && otaCard.textContent.includes("6.2.1"), 1000, "re-select completes");

      console.log("PASS 2: Header edge vectors (287/288B, BOM, invalid/truncated UTF-8 parity, 32B no-NUL), oversized, same-ver, re-select lock");
    } finally {
      env.cleanup();
      envNoTd.cleanup();
      await stopMock(m.proc);
    }
  }

  // 3. Finding 4: Queued/inflight WS operations cannot bleed into upload
  currentCase = "3_ws_inflight_guard";
  {
    const m = await startMock(["--drop-ack", "gnss.rate"]);
    const env = createDomEnv(m.port, { holdAckCmd: "exit" });
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      const sysCard = env.ids["col-b"].children.find((c) => c.id === "card-sys");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock") && env.ids["p-conn"].textContent.includes("Connected"), 3000, "connected");
      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      fileInp.files = [makeFakeFile(buildBin("6.2.0", "rtk_main", 4096))];
      fileInp.dispatch("change");
      await waitUntil(() => !uploadBtn.disabled, 1000, "valid file ready");

      // Trigger exit command whose ACK is held -> uploadBtn must become disabled immediately
      const sysBtn = sysCard.querySelector("button");
      sysBtn.click(); // arm
      sysBtn.click(); // fire exit (held in flight)
      assert(uploadBtn.disabled, "uploadBtn must be disabled while WS cmd exit is inflight");
      // Wait for 2 s ACK timeout to clear inflight -> uploadBtn re-enables
      await waitUntil(() => !uploadBtn.disabled, 3500, "uploadBtn re-enables after WS ACK timeout");
      assert(env.ids["banner"].hidden, "timed-out exit must not show restart banner");

      // Trigger gnss.rate set (dropped ACK by mock) -> uploadBtn disabled while inflight
      const gnssBtns = env.ids["col-a"].children.find((c) => c.id === "card-gnss").querySelectorAll("button");
      const rateBtn = gnssBtns.find((b) => b.getAttribute("aria-pressed") !== "true");
      rateBtn.click();
      assert(uploadBtn.disabled, "uploadBtn must be disabled while WS set is inflight");
      await waitUntil(() => !uploadBtn.disabled, 3500, "uploadBtn re-enables after set timeout");

      console.log("PASS 3: Inflight WS exit/set disables OTA upload until drained");
    } finally {
      env.cleanup();
      await stopMock(m.proc);
    }
  }

  // 4. Slow upload + ALL controls disabled (including ALL 5 Factory controls!) + WS disconnect suppressed + reboot disconnects live WS
  currentCase = "4_slow_upload_all_locks_and_reboot";
  {
    const m = await startMock(["--ota-slow-kbps", "16", "--ota-max", "16384"]);
    const env = createDomEnv(m.port, { uploadChunkSize: 1024, uploadChunkDelayMs: 25 });
    const observerWs = new globalThis.WebSocket(`ws://127.0.0.1:${m.port}/ws`);
    let observerClosed = false;
    observerWs.addEventListener("close", () => { observerClosed = true; });
    observerWs.addEventListener("error", () => {});
    try {
      await waitUntil(() => observerWs.readyState === 1, 3000, "observer WS open");
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock") && env.ids["p-conn"].textContent.includes("Connected"), 3000, "info + ws");

      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      fileInp.files = [makeFakeFile(buildBin("6.2.0-ota", "rtk_main", 8192))];
      fileInp.dispatch("change");
      await waitUntil(() => !uploadBtn.disabled, 1000, "file valid");

      uploadBtn.click(); // arm
      assert(uploadBtn.textContent.includes("将替换当前固件"), `arm text=${uploadBtn.textContent}`);
      uploadBtn.click(); // start

      await sleep(60);
      assert(fileInp.disabled && uploadBtn.disabled, "OTA controls disabled during upload");
      const gnssCard = env.ids["col-a"].children.find((c) => c.id === "card-gnss");
      const gateCard = env.ids["col-a"].children.find((c) => c.id === "card-gate");
      const btCard = env.ids["col-b"].children.find((c) => c.id === "card-bt");
      const facCard = env.ids["col-b"].children.find((c) => c.id === "card-fac");
      const sysCard = env.ids["col-b"].children.find((c) => c.id === "card-sys");

      const checkAllDisabled = (card, name) => {
        const ctls = [
          ...card.querySelectorAll("button"),
          ...card.querySelectorAll("input"),
          ...card.querySelectorAll("select"),
        ];
        assert(ctls.length > 0, `${name} should have controls`);
        assert(ctls.every((c) => c.disabled), `${name} has enabled control during upload`);
        return ctls.length;
      };
      checkAllDisabled(gnssCard, "GNSS");
      checkAllDisabled(gateCard, "Gate");
      checkAllDisabled(btCard, "Bluetooth");
      const facCount = checkAllDisabled(facCard, "Factory");
      assert(facCount === 5, `expected 5 Factory inputs/buttons, got ${facCount}`);
      checkAllDisabled(sysCard, "System");

      // Close UI WS mid-upload -> banner stays hidden
      env.wsInstances[0].close();
      await sleep(50);
      assert(env.ids["banner"].hidden, `banner should stay hidden during upload, got: ${env.ids["banner"].textContent}`);

      // Wait for completion
      await waitUntil(() => otaCard.textContent.includes("升级完成，设备将在约"), 5000, "upload completion");
      assert(env.xhrProgressEvents.length >= 4, `expected progress events, got ${env.xhrProgressEvents.length}`);
      assert(env.ids["banner"].textContent.includes("升级完成，设备将在约 1.5 秒后重启进入蓝牙模式，热点会断开"), `banner=${env.ids["banner"].textContent}`);
      assert(!env.root.classList.contains("offline"), "should not gray out as offline");

      // Verify mock reboot actually disconnects observer WS (not timeout)
      await waitUntil(() => observerClosed, 3500, "observer WS closed by mock reboot");
      console.log(`PASS 4: Slow upload (${env.xhrProgressEvents.length} events), ALL cards incl. 5 Factory controls disabled, WS-drop suppressed, reboot WS close verified`);
    } finally {
      try { observerWs.close(); } catch {}
      env.cleanup();
      await stopMock(m.proc);
    }
  }

  // 5. All 9 --ota-err codes end-to-end + progress bar hidden on failure
  currentCase = "5_ota_err_codes";
  const expectedErrSubstr = {
    busy: "设备忙",
    state: "尚未确认",
    length: "缺少长度",
    too_large: "过大",
    bad_image: "头校验失败",
    validate: "校验失败",
    timeout: "超时",
    flash: "Flash",
    aborted: "中止",
  };
  for (const [code, expectText] of Object.entries(expectedErrSubstr)) {
    currentCase = `5_ota_err_${code}`;
    const m = await startMock(["--ota-err", code]);
    const env = createDomEnv(m.port);
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock"), 3000, `info for ${code}`);
      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      const progBox = otaCard.querySelector(".prog");
      fileInp.files = [makeFakeFile(buildBin("6.2.0", "rtk_main", 4096))];
      fileInp.dispatch("change");
      await waitUntil(() => !uploadBtn.disabled, 1000, `file valid for ${code}`);
      uploadBtn.click();
      uploadBtn.click();
      await waitUntil(() => otaCard.textContent.includes(expectText), 3000, `err=${code}`);
      assert(progBox.hidden, `progress bar must hide on err=${code}, left: ${progBox.textContent}`);
      assert(!fileInp.disabled && !uploadBtn.disabled, `controls re-enabled after err=${code}`);
    } finally {
      env.cleanup();
      await stopMock(m.proc);
    }
  }
  console.log("PASS 5: All 9 --ota-err codes verified + progress bar cleared + controls re-enabled");

  // 6. XHR network error (onerror)
  currentCase = "6_xhr_onerror";
  {
    const m = await startMock([]);
    const env = createDomEnv(m.port, { forceXhrNetworkErr: true });
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock"), 3000, "info for net err");
      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      const progBox = otaCard.querySelector(".prog");
      fileInp.files = [makeFakeFile(buildBin("6.2.0", "rtk_main", 4096))];
      fileInp.dispatch("change");
      await waitUntil(() => !uploadBtn.disabled, 1000, "file valid");
      uploadBtn.click();
      uploadBtn.click();
      await waitUntil(() => otaCard.textContent.includes("连接中断，设备仍运行原固件"), 3000, "onerror text");
      assert(progBox.hidden, "progress bar must hide on onerror");
      assert(!fileInp.disabled && !uploadBtn.disabled, "controls re-enabled after onerror");
      console.log("PASS 6: XHR onerror -> '连接中断，设备仍运行原固件', progress cleared, controls re-enabled");
    } finally {
      env.cleanup();
      await stopMock(m.proc);
    }
  }

  // 7. Finding 2: Client POST timeout (stall watchdog + xhr.ontimeout) unlocks UI, clears progress, resumes WS reconnect
  currentCase = "7a_client_stall_watchdog";
  {
    const m = await startMock([]);
    const env = createDomEnv(m.port, { stallOtaPost: true, longTimerScale: 0.004 }); // 45s -> 180ms
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock") && env.ids["p-conn"].textContent.includes("Connected"), 3000, "ready");
      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      const progBox = otaCard.querySelector(".prog");
      fileInp.files = [makeFakeFile(buildBin("6.2.0", "rtk_main", 4096))];
      fileInp.dispatch("change");
      await waitUntil(() => !uploadBtn.disabled, 1000, "file valid");

      uploadBtn.click();
      uploadBtn.click();
      const postXhr = env.xhrInstances.find((x) => x.url === "/ota");
      assert(postXhr && postXhr.timeout === 180000, `expected xhr.timeout=180000, got ${postXhr && postXhr.timeout}`);

      // Drop WS during stalled upload -> must auto-reconnect once watchdog unlocks!
      env.wsInstances[0].close();
      await waitUntil(() => otaCard.textContent.includes("确认设备状态"), 2000, "watchdog timeout message");
      assert(progBox.hidden, "progress bar hidden after watchdog timeout");
      assert(!fileInp.disabled && !uploadBtn.disabled, "OTA controls unlocked after watchdog timeout");
      await waitUntil(() => env.wsInstances.length >= 2 && env.ids["p-conn"].textContent.includes("Connected"), 3000, "WS reconnected after timeout");
      console.log("PASS 7a: Stall watchdog unlocks UI, clears progress, shows device-status prompt, resumes WS reconnect");
    } finally {
      env.cleanup();
      await stopMock(m.proc);
    }
  }

  currentCase = "7b_xhr_ontimeout";
  {
    const m = await startMock([]);
    const env = createDomEnv(m.port, { stallOtaPost: true, fireXhrTimeoutMs: 60 });
    try {
      const otaCard = env.ids["col-b"].children.find((c) => c.id === "card-ota");
      await waitUntil(() => otaCard.textContent.includes("0.1.0-mock") && env.ids["p-conn"].textContent.includes("Connected"), 3000, "ready");
      const fileInp = otaCard.querySelector("input");
      const uploadBtn = otaCard.querySelector("button");
      const progBox = otaCard.querySelector(".prog");
      fileInp.files = [makeFakeFile(buildBin("6.2.0", "rtk_main", 4096))];
      fileInp.dispatch("change");
      await waitUntil(() => !uploadBtn.disabled, 1000, "file valid");
      uploadBtn.click();
      uploadBtn.click();
      await waitUntil(() => otaCard.textContent.includes("确认设备状态"), 2000, "xhr.ontimeout message");
      assert(progBox.hidden && !fileInp.disabled && !uploadBtn.disabled, "xhr.ontimeout unlocked UI and hid progress");
      console.log("PASS 7b: XHR ontimeout unlocks UI, clears progress, shows device-status prompt");
    } finally {
      env.cleanup();
      await stopMock(m.proc);
    }
  }

  // 8. Finding 10: Banner CSS alignment check
  currentCase = "8_css_banner_alignment";
  {
    const bannerRule = APP_CSS.match(/\.banner\{[^}]+\}/);
    assert(bannerRule, ".banner rule missing");
    const rule = bannerRule[0];
    assert(rule.includes("width:calc(100% - 32px)"), `banner width missing: ${rule}`);
    assert(rule.includes("max-width:1088px"), `banner max-width missing: ${rule}`);
    assert(rule.includes("margin:12px auto 0"), `banner auto margin missing: ${rule}`);
    assert(!rule.includes("margin-left") && !rule.includes("margin-right"), `banner still overrides left/right margin: ${rule}`);
    console.log("PASS 8: .banner CSS aligned to 1088px centered card content width with 16px narrow gutter");
  }

  console.log("ALL E2E UI TESTS PASSED");
}

runTests()
  .then(() => {
    clearTimeout(globalWatchdog);
    killAllProcs();
    process.exit(0);
  })
  .catch((e) => {
    clearTimeout(globalWatchdog);
    console.error(`FAIL in case ${currentCase}:`, e);
    killAllProcs();
    process.exit(1);
  });
