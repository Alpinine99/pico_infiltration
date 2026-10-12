(() => {
  "use strict";

  /* ================= distance-jump detection (pure, unit-testable) =================
   * values: array of number|null (null = no echo). Returns
   *   red[i]  true when the line segment ending at point i should be red
   *   events  [{start, end, from, to, active}]   (indices into `values`)
   * A jump is confirmed when a reading differs from the previous normal reading by
   * >= thresh AND the next (k-1) readings stay within tol of it. A lone spike is ignored. */
  function analyze(values, { thresh, k, tol }) {
    const red = new Array(values.length).fill(false);
    const events = [];
    const idx = [];
    values.forEach((v, i) => {
      if (v != null) idx.push(i);
    });

    let base = null,
      inAlert = false,
      ev = null;
    for (let p = 0; p < idx.length; p++) {
      const i = idx[p],
        v = values[i];
      if (base === null) {
        base = v;
        continue;
      }

      if (!inAlert) {
        if (Math.abs(v - base) >= thresh) {
          let ok = true,
            have = 0;
          for (let q = p + 1; q < idx.length && have < k - 1; q++, have++) {
            if (Math.abs(values[idx[q]] - v) > tol) {
              ok = false;
              break;
            }
          }
          if (ok && have === k - 1) {
            // confirmed (not a spike, enough data)
            inAlert = true;
            ev = { start: i, end: i, from: base, to: v, active: true };
            events.push(ev);
            red[i] = true;
          }
        } else {
          base = v; // normal reading: follow slow drift
        }
      } else if (Math.abs(v - base) >= thresh) {
        red[i] = true;
        ev.end = i;
        ev.to = v;
      } else {
        inAlert = false;
        ev.active = false;
        base = v; // back to normal
      }
    }
    return { red, events };
  }

  if (typeof module !== "undefined" && module.exports)
    module.exports = { analyze };
  if (typeof document === "undefined") return; // running under node: stop here

  /* ================= config ================= */
  const POLL_MS = 2000;
  const MAX_POINTS = 600; // rows kept per Pico (~20 min at one row / 2 s)
  const MAX_LOG_LINES = 1000;
  const ONLINE_S = 30; // "online" = sent something this recently
  const CH = ["d0", "d1", "d2"];
  const COLOR = { d0: "#5eead4", d1: "#a78bfa", d2: "#fbbf24" };
  const RED = "#ef4444";
  const DEFAULTS = { thresh: 30, k: 3, tol: 10 };

  const $ = (s, r = document) => r.querySelector(s);
  const URLS = {
    readings: document.body.dataset.readings,
    log: document.body.dataset.log,
    download: document.body.dataset.download,
    reboot: document.body.dataset.reboot,
    sleep: document.body.dataset.sleep,
  };

  /* Build DOM with textContent only: Pico ids come off the network, never use innerHTML for them. */
  function el(tag, props = {}, ...kids) {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(props)) {
      if (k === "class") n.className = v;
      else if (k === "text") n.textContent = v;
      else n.setAttribute(k, v);
    }
    for (const c of kids) if (c != null) n.append(c);
    return n;
  }

  /* ================= formatting ================= */
  const pad2 = (n) => String(n).padStart(2, "0");
  const fmtClock = (ts) => {
    const d = new Date(ts * 1000);
    return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  };
  const fmtDate = (ts) => {
    const d = new Date(ts * 1000);
    return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
  };
  const fmtCm = (v) => (v == null ? "—" : v.toFixed(1));
  const fmtUp = (s) => {
    if (s == null) return "—";
    const d = Math.floor(s / 86400),
      h = Math.floor((s % 86400) / 3600),
      m = Math.floor((s % 3600) / 60),
      x = s % 60;
    return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : m ? `${m}m ${x}s` : `${x}s`;
  };
  const fmtBytes = (b) =>
    b == null
      ? "—"
      : b >= 1048576
        ? `${(b / 1048576).toFixed(1)} MB`
        : b >= 1024
          ? `${(b / 1024).toFixed(1)} KB`
          : `${b} B`;
  const ago = (s) =>
    s < 60
      ? `${Math.max(0, Math.floor(s))}s`
      : s < 3600
        ? `${Math.floor(s / 60)}m`
        : `${Math.floor(s / 3600)}h`;

  /* ================= state ================= */
  const tuning = (() => {
    try {
      return {
        ...DEFAULTS,
        ...JSON.parse(localStorage.getItem("pico.tuning") || "{}"),
      };
    } catch {
      return { ...DEFAULTS };
    }
  })();
  const saveTuning = () => {
    try {
      localStorage.setItem("pico.tuning", JSON.stringify(tuning));
    } catch {
      /* private mode */
    }
  };

  const S = {
    lastId: 0,
    tick: 0,
    skew: 0,
    rows: new Map(), // device -> rows, oldest first
    latest: new Map(), // device -> newest row
    analysis: new Map(), // device -> { d0:{red,events}, d1:.., d2:.. }
    notified: new Set(),
    selected: null,
    namesKey: "",
    tab: "frames",
    logOffset: null,
    srvMissing: false,
    alertsOn: false,
    audio: null,
    chart: null,
  };
  const B = { frames: [], server: [] }; // log buffers
  const nowServer = () => Date.now() / 1000 + S.skew;

  /* ================= alerts ================= */
  function activeAlerts(dev) {
    const a = S.analysis.get(dev),
      out = [];
    if (!a) return out;
    for (const ch of CH) {
      const last = a[ch].events[a[ch].events.length - 1];
      if (last && last.active) out.push({ ch, from: last.from, to: last.to });
    }
    return out;
  }

  function analyzeDevice(dev, silent) {
    const rows = S.rows.get(dev) || [],
      res = {};
    for (const ch of CH) {
      const a = analyze(
        rows.map((r) => r[ch]),
        tuning,
      );
      res[ch] = a;
      for (const ev of a.events) {
        const key = `${dev}|${ch}|${rows[ev.start].id}`;
        if (S.notified.has(key)) continue;
        S.notified.add(key);
        if (!silent) raiseAlert(dev, ch, ev);
      }
    }
    S.analysis.set(dev, res);
  }

  function raiseAlert(dev, ch, ev) {
    const change = `${fmtCm(ev.from)} → ${fmtCm(ev.to)} cm`;
    addLines("frames", [
      {
        text: `ALERT  ${dev}  ${ch}: ${change}  (held for ${tuning.k} readings)`,
        cls: "ln-alert",
      },
    ]);
    toast(`⚠ ${dev} ${ch}: ${change}`, "bad", 10000);
    if (S.alertsOn) {
      if ("Notification" in window && Notification.permission === "granted") {
        try {
          new Notification(`Distance change: ${dev}`, {
            body: `${ch}: ${change}`,
          });
        } catch {
          /* ignore */
        }
      }
      beep();
    }
  }

  function beep() {
    try {
      const ctx =
        S.audio ||
        (S.audio = new (window.AudioContext || window.webkitAudioContext)());
      const o = ctx.createOscillator(),
        g = ctx.createGain();
      o.frequency.value = 880;
      g.gain.value = 0.05;
      o.connect(g);
      g.connect(ctx.destination);
      o.start();
      o.stop(ctx.currentTime + 0.25);
    } catch {
      /* no audio available */
    }
  }

  function toast(text, kind = "", ms = 5000) {
    const t = el("div", { class: `toast ${kind}`, text });
    $("#toasts").append(t);
    setTimeout(() => t.remove(), ms);
  }

  /* ================= chart ================= */
  const isRed = (ch, i) => {
    const a = S.analysis.get(S.selected);
    return !!(a && a[ch].red[i]);
  };

  function makeChart() {
    if (typeof Chart === "undefined") {
      $("#chart-empty").textContent =
        "Chart.js failed to load (check static files)";
      return;
    }
    Chart.defaults.color = "#8b93a7";
    Chart.defaults.font.family =
      'system-ui, -apple-system, "Segoe UI", Roboto, sans-serif';
    const grid = { color: "rgba(255,255,255,.05)" };
    S.chart = new Chart($("#chart"), {
      type: "line",
      data: {
        labels: [],
        datasets: CH.map((ch, i) => ({
          label: `Sensor ${i}`,
          data: [],
          borderColor: COLOR[ch],
          backgroundColor: COLOR[ch],
          borderWidth: 2,
          tension: 0,
          spanGaps: false,
          pointRadius: (c) => (isRed(ch, c.dataIndex) ? 3.5 : 0),
          pointHoverRadius: 4,
          pointBackgroundColor: (c) =>
            isRed(ch, c.dataIndex) ? RED : COLOR[ch],
          pointBorderColor: (c) => (isRed(ch, c.dataIndex) ? RED : COLOR[ch]),
          segment: {
            borderColor: (c) => (isRed(ch, c.p1DataIndex) ? RED : COLOR[ch]),
          },
        })),
      },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: "index", intersect: false },
        plugins: {
          legend: { labels: { usePointStyle: true, boxWidth: 8 } },
          tooltip: {
            callbacks: {
              title: (items) => {
                const r = (S.rows.get(S.selected) || [])[items[0].dataIndex];
                return r ? `${fmtDate(r.ts)} ${fmtClock(r.ts)}` : "";
              },
              label: (c) => `${c.dataset.label}: ${fmtCm(c.parsed.y)} cm`,
            },
          },
        },
        scales: {
          x: { grid, ticks: { maxTicksLimit: 8, maxRotation: 0 } },
          y: { beginAtZero: true, grid, title: { display: true, text: "cm" } },
        },
      },
    });
  }

  function refreshChart() {
    const rows = S.rows.get(S.selected) || [];
    $("#chart-empty").hidden = rows.length > 0;
    if (!S.chart) return;
    S.chart.data.labels = rows.map((r) => fmtClock(r.ts));
    CH.forEach((ch, i) => {
      S.chart.data.datasets[i].data = rows.map((r) => r[ch]);
    });
    S.chart.update("none");
  }

  /* ================= device cards ================= */
  const stat = (label, value, cls = "") =>
    el(
      "div",
      {},
      el("span", { text: label }),
      el("b", { class: cls, text: value }),
    );

  function card(name) {
    const r = S.latest.get(name),
      age = nowServer() - r.ts,
      online = age <= ONLINE_S,
      alerts = activeAlerts(name);
    const low = r.battery_mv != null && r.battery_mv < 3400;
    return el(
      "div",
      {
        class: `dev${name === S.selected ? " sel" : ""}${online ? "" : " off"}${alerts.length ? " alert" : ""}`,
        "data-id": name,
        title: "Show on graph",
      },
      el(
        "div",
        { class: "dev-top" },
        el("span", { class: "dev-name", text: name }),
        el("span", {
          class: `badge ${online ? "ok" : "off"}`,
          text: online ? "online" : "offline",
        }),
      ),
      el(
        "div",
        { class: "dev-grid" },
        stat(
          "Battery",
          r.battery_mv == null ? "—" : `${r.battery_mv} mV`,
          low ? "warn" : "",
        ),
        stat("Uptime", fmtUp(r.uptime_s)),
        stat("Heap free", fmtBytes(r.heap_free)),
        stat("Last seen", `${ago(age)} ago`),
      ),
      el(
        "div",
        { class: "dev-d" },
        ...CH.map((ch) =>
          el("span", {
            class: alerts.some((a) => a.ch === ch) ? "bad" : "",
            text: `${ch} ${fmtCm(r[ch])}`,
          }),
        ),
      ),
      alerts.length
        ? el("div", {
            class: "dev-alert",
            text: alerts
              .map((a) => `⚠ ${a.ch}: ${fmtCm(a.from)} → ${fmtCm(a.to)} cm`)
              .join("   "),
          })
        : null,
    );
  }

  function renderDevices() {
    const names = [...S.latest.keys()].sort(),
      box = $("#devices");
    box.replaceChildren(
      ...(names.length
        ? names.map(card)
        : [
            el("div", {
              class: "muted pad",
              text: "No Pico has reported yet.",
            }),
          ]),
    );
    document.title = names.some((n) => activeAlerts(n).length)
      ? "(!) Pico monitor"
      : "Pico monitor";
  }

  function fill(sel, names, withAll) {
    const keep = sel.value;
    sel.replaceChildren(
      ...(withAll ? [el("option", { value: "", text: "All Picos" })] : []),
      ...names.map((n) => el("option", { value: n, text: n })),
    );
    sel.value =
      (withAll && keep === "") || names.includes(keep)
        ? keep
        : withAll
          ? ""
          : S.selected || names[0] || "";
  }

  function syncSelects(force) {
    const names = [...S.latest.keys()].sort(),
      key = names.join("\u0000");
    if (key === S.namesKey && !force) return;
    S.namesKey = key;
    fill($("#chart-device"), names, false);
    fill($("#rb-device"), names, false);
    fill($("#sl-device"), names, false);
    fill($("#dl-device"), names, true);
    if (S.selected) $("#chart-device").value = S.selected;
  }

  function selectDevice(name) {
    S.selected = name;
    if (!S.analysis.has(name)) analyzeDevice(name, true);
    $("#chart-device").value = name;
    refreshChart();
    renderDevices();
  }

  /* ================= log panel ================= */
  const logEl = $("#log");
  const atBottom = () =>
    logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 40;
  const toBottom = () => {
    logEl.scrollTop = logEl.scrollHeight;
    $("#btn-latest").hidden = true;
  };
  const lineNode = (e) => el("div", { class: `ln ${e.cls}`, text: e.text });
  logEl.addEventListener("scroll", () => {
    $("#btn-latest").hidden = atBottom();
  });
  $("#btn-latest").addEventListener("click", toBottom);

  function addLines(kind, entries) {
    if (!entries.length) return;
    const buf = B[kind];
    buf.push(...entries);
    if (buf.length > MAX_LOG_LINES) buf.splice(0, buf.length - MAX_LOG_LINES);
    if (S.tab !== kind) return;
    const stick = atBottom(); // only follow if the user is already at the bottom
    logEl.append(...entries.map(lineNode));
    while (logEl.childElementCount > MAX_LOG_LINES)
      logEl.firstElementChild.remove();
    if (stick) toBottom();
  }
  function renderLog() {
    logEl.replaceChildren(...B[S.tab].map(lineNode));
    toBottom();
  }

  function setTab(tab) {
    S.tab = tab;
    $("#tab-frames").classList.toggle("on", tab === "frames");
    $("#tab-server").classList.toggle("on", tab === "server");
    renderLog();
    if (tab === "server") pollServerLog();
  }
  $("#tab-frames").addEventListener("click", () => setTab("frames"));
  $("#tab-server").addEventListener("click", () => setTab("server"));

  const frameText = (r) =>
    `${fmtDate(r.ts)} ${fmtClock(r.ts)}  #${r.id}  ${r.device}  batt=${r.battery_mv ?? "—"}mV  up=${fmtUp(r.uptime_s)}  ` +
    `heap=${r.heap_free ?? "—"}/${r.heap_min_free ?? "—"}  d=[${CH.map((c) => fmtCm(r[c])).join(", ")}]`;

  const levelClass = (t) =>
    /\bERROR\b/.test(t)
      ? "ln-bad"
      : /\bWARNING\b/.test(t)
        ? "ln-warn"
        : /\bDEBUG\b/.test(t)
          ? "ln-dim"
          : "ln-info";

  async function pollServerLog() {
    if (S.tab !== "server") return;
    try {
      const q = new URLSearchParams();
      if (S.logOffset != null) q.set("offset", S.logOffset);
      const j = await (
        await fetch(`${URLS.log}?${q}`, { cache: "no-store" })
      ).json();
      if (!j.available) {
        if (!S.srvMissing) {
          S.srvMissing = true;
          addLines("server", [
            { text: j.error || "server log not available", cls: "ln-dim" },
          ]);
        }
        return;
      }
      S.srvMissing = false;
      if (j.reset) {
        B.server = [];
        if (S.tab === "server") logEl.replaceChildren();
      }
      S.logOffset = j.offset;
      addLines(
        "server",
        j.lines.map((t) => ({ text: t, cls: levelClass(t) })),
      );
    } catch {
      /* retry on the next tick */
    }
  }

  /* ================= polling ================= */
  function setConn(ok) {
    $("#conn-dot").className = `dot ${ok ? "ok" : "bad"}`;
    $("#conn-text").textContent = ok
      ? `live · ${fmtClock(Date.now() / 1000)}`
      : "offline, retrying…";
  }

  function ingest(rows, silent) {
    const touched = new Set();
    for (const r of rows) {
      let arr = S.rows.get(r.device);
      if (!arr) S.rows.set(r.device, (arr = []));
      arr.push(r);
      if (arr.length > MAX_POINTS) arr.shift();
      const cur = S.latest.get(r.device);
      if (!cur || r.id >= cur.id) S.latest.set(r.device, r);
      S.lastId = Math.max(S.lastId, r.id);
      touched.add(r.device);
    }
    if (rows.length)
      addLines(
        "frames",
        rows.slice(-300).map((r) => ({ text: frameText(r), cls: "ln-info" })),
      );
    for (const d of touched) analyzeDevice(d, silent);

    if (!S.selected && S.latest.size) {
      S.selected = [...S.latest.values()].sort((a, b) => b.ts - a.ts)[0].device;
      if (!S.analysis.has(S.selected)) analyzeDevice(S.selected, true);
    }
    syncSelects();
    if (touched.has(S.selected) || rows.length === 0) refreshChart();
    renderDevices();
  }

  async function poll() {
    const first = S.lastId === 0;
    const q = new URLSearchParams({
      after: S.lastId,
      limit: first ? 500 : 2000,
    });
    if (first || ++S.tick % 15 === 0) q.set("devices", "1"); // full device list now and then, not on every poll
    try {
      const res = await fetch(`${URLS.readings}?${q}`, { cache: "no-store" });
      if (!res.ok) throw new Error(res.status);
      const data = await res.json();
      S.skew = data.now - Date.now() / 1000;
      for (const d of data.devices || []) {
        if (!d) continue;
        const cur = S.latest.get(d.device);
        if (!cur || d.id >= cur.id) S.latest.set(d.device, d);
      }
      ingest(data.rows, first); // the first load is history: no alerts for it
      setConn(true);
    } catch {
      setConn(false);
    }
    setTimeout(poll, POLL_MS);
  }

  /* ================= actions: reboot / sleep / download ================= */
  function csrf() {
    return document.querySelector("meta[name=csrf-token]").content;
  }

  async function post(urlTemplate, id, data) {
    try {
      const res = await fetch(
        urlTemplate.replace("__ID__", encodeURIComponent(id)),
        {
          method: "POST",
          headers: {
            "X-CSRFToken": csrf(),
            "Content-Type": "application/x-www-form-urlencoded",
          },
          body: new URLSearchParams(data),
        },
      );
      let j = {};
      try {
        j = await res.json();
      } catch {
        /* not json */
      }
      return {
        ok: res.ok && j.ok,
        msg: j.message || j.error || res.statusText,
      };
    } catch (e) {
      return { ok: false, msg: "network error" };
    }
  }

  const openDlg = (id) => {
    syncSelects(true);
    for (const s of ["#rb-device", "#sl-device"])
      if (S.selected) $(s).value = S.selected;
    const d = $(id);
    if (d.showModal) d.showModal();
    else d.setAttribute("open", "");
  };
  const closeDlg = (id) => {
    const d = $(id);
    if (d.close) d.close();
    else d.removeAttribute("open");
  };

  $("#btn-reboot").addEventListener("click", () => openDlg("#dlg-reboot"));
  $("#btn-sleep").addEventListener("click", () => openDlg("#dlg-sleep"));
  $("#btn-download").addEventListener("click", () => openDlg("#dlg-download"));

  $("#go-reboot").addEventListener("click", async () => {
    const id = $("#rb-device").value;
    if (!id) return;
    closeDlg("#dlg-reboot");
    const r = await post(URLS.reboot, id, {});
    toast(
      r.ok ? `Reboot sent to ${id}` : `Reboot failed: ${r.msg}`,
      r.ok ? "ok" : "bad",
    );
  });

  $("#go-sleep").addEventListener("click", async () => {
    const id = $("#sl-device").value,
      sec = Number($("#sl-seconds").value);
    if (!id) return;
    if (!Number.isInteger(sec) || sec < 3 || sec > 86400) {
      toast(
        "Sleep time must be a whole number between 3 and 86400 seconds",
        "warn",
      );
      return;
    }
    closeDlg("#dlg-sleep");
    const r = await post(URLS.sleep, id, { seconds: sec });
    toast(
      r.ok ? `${id} will sleep for ${sec} s` : `Sleep failed: ${r.msg}`,
      r.ok ? "ok" : "bad",
    );
  });

  $("#go-download").addEventListener("click", () => {
    const q = new URLSearchParams({
      format: $("#dl-format").value,
      range: $("#dl-range").value,
      device: $("#dl-device").value,
    });
    closeDlg("#dlg-download");
    window.location.href = `${URLS.download}?${q}`; // Content-Disposition: attachment, so the page stays put
  });

  /* ================= alerts button, selects, tuning ================= */
  $("#btn-notify").addEventListener("click", async () => {
    S.alertsOn = !S.alertsOn;
    if (S.alertsOn) {
      beep(); // a click is the user gesture that unlocks audio
      if (!("Notification" in window))
        toast(
          "Desktop notifications need https:// or localhost. In-page alerts and sound still work.",
          "warn",
          9000,
        );
      else if (Notification.permission === "default")
        await Notification.requestPermission();
      if ("Notification" in window && Notification.permission === "denied")
        toast(
          "Notifications are blocked for this site in the browser.",
          "warn",
          9000,
        );
    }
    $("#btn-notify").textContent = S.alertsOn ? "Alerts on" : "Enable alerts";
    $("#btn-notify").classList.toggle("on", S.alertsOn);
  });

  $("#chart-device").addEventListener("change", (e) =>
    selectDevice(e.target.value),
  );
  $("#devices").addEventListener("click", (e) => {
    const c = e.target.closest(".dev");
    if (c) selectDevice(c.dataset.id);
  });

  for (const [sel, key, min] of [
    ["#tn-thresh", "thresh", 1],
    ["#tn-k", "k", 1],
    ["#tn-tol", "tol", 0],
  ]) {
    const inp = $(sel);
    inp.value = tuning[key];
    inp.addEventListener("change", () => {
      const v = Number(inp.value);
      if (!Number.isFinite(v) || v < min) {
        inp.value = tuning[key];
        return;
      }
      tuning[key] = key === "k" ? Math.round(v) : v;
      saveTuning();
      for (const d of S.rows.keys()) analyzeDevice(d, true); // silent: don't re-alert on history
      refreshChart();
      renderDevices();
    });
  }

  /* ================= go ================= */
  makeChart();
  renderLog();
  poll();
  setInterval(renderDevices, 1000); // keeps "last seen" and online/offline fresh between polls
  setInterval(pollServerLog, POLL_MS);
})();
