// semcache dashboard. Vanilla JS, no dependencies, same-origin only (see the CSP in proxy_server.cpp).
// Prompt text never leaves this tab except as the chat request itself; the server's /stats has none.
"use strict";

(function () {
  const $ = (id) => document.getElementById(id);
  const session = Math.random().toString(36).slice(2, 10);  // separates this tab's scenario contexts
  let runCounter = 0;
  let stats = null;
  let apiKey = "";          // demo: the public key from /stats; live: typed by the user, memory only
  const sparkPoints = [];   // [uptime_s, hit_rate]
  const SPARK_MAX = 120;

  // ---------- formatting ----------
  const fmtInt = (n) => (n == null ? "–" : Math.round(n).toLocaleString());
  const fmtPct = (x) => (x == null ? "–" : (x * 100).toFixed(1) + "%");
  const fmtMs = (ms) => (ms == null ? "–" : ms < 10 ? ms.toFixed(1) + " ms" : Math.round(ms) + " ms");
  const fmtBytes = (b) => {
    if (b == null) return "";
    const u = ["B", "KB", "MB", "GB"];
    let i = 0;
    while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
    return b.toFixed(i === 0 ? 0 : 1) + " " + u[i];
  };
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };
  const badge = (result) => el("span", "badge " + (result || ""), result || "?");
  const median = (xs) => {
    if (xs.length === 0) return null;
    const s = xs.slice().sort((a, b) => a - b);
    const m = s.length >> 1;
    return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
  };

  // ---------- live stats ----------
  function render(s) {
    stats = s;
    const req = s.requests || {};
    const hits = req.HIT || 0, coalesced = req.COALESCED || 0;
    $("t-requests").textContent = fmtInt(s.requests_total);
    $("t-hitrate").textContent = fmtPct(s.hit_rate);
    $("t-hits").textContent = fmtInt(hits) + " hits · " + fmtInt(req.MISS || 0) + " misses";
    $("t-avoided").textContent = fmtInt(hits + coalesced);
    $("t-tokens").textContent = fmtInt(s.tokens_saved);
    if (s.cache) {
      $("t-entries").textContent = fmtInt(s.cache.entries);
      $("t-bytes").textContent = fmtBytes(s.cache.bytes) + " of " + fmtBytes(s.cache.max_bytes);
      $("pill-threshold").textContent = "similarity threshold " + s.cache.similarity_threshold;
    } else {
      $("t-entries").textContent = "off";
      $("pill-threshold").textContent = "cache disabled";
    }

    const recent = s.recent || [];
    const lat = (r) => recent.filter((x) => x.result === r && x.status === 200).map((x) => x.latency_ms);
    $("t-hit-ms").textContent = fmtMs(median(lat("HIT")));
    $("t-miss-ms").textContent = fmtMs(median(lat("MISS")));

    const mode = $("pill-mode");
    mode.textContent = s.mode === "demo" ? "demo mode (mock LLM)" : "live mode";
    mode.className = "pill " + (s.mode === "demo" ? "ok" : "warn");
    const emb = $("pill-embedder");
    emb.textContent = s.embedder_ready ? "embedder ready" : "embedder off: exact matches only";
    emb.className = "pill " + (s.embedder_ready ? "ok" : "warn");

    if (s.mode === "demo" && s.demo) {
      apiKey = s.demo.public_key;
      $("key-row").hidden = true;
      $("reset").hidden = false;
      if (s.demo.max_prompt_chars) $("try-prompt").maxLength = s.demo.max_prompt_chars;
    } else {
      $("key-row").hidden = false;
      $("reset").hidden = true;
    }
    const herd = herdSize();
    for (const n of document.querySelectorAll(".herd-n")) n.textContent = String(herd);

    renderRecent(recent);
    pushSpark(s.uptime_s, s.hit_rate, s.requests_total);
  }

  function renderRecent(recent) {
    const body = $("recent");
    body.replaceChildren();
    for (const r of recent.slice(0, 12)) {
      const tr = el("tr");
      const t = new Date(r.at_ms);
      tr.append(el("td", null, t.toLocaleTimeString()));
      const td = el("td");
      td.append(badge(r.result));
      if (r.streamed) td.append(" ", el("span", "sub", "stream"));
      tr.append(td, el("td", null, String(r.status)), el("td", null, fmtMs(r.latency_ms)),
        el("td", null, r.similarity == null ? "" : r.similarity.toFixed(3)));
      body.append(tr);
    }
    if (recent.length === 0) {
      const tr = el("tr");
      const td = el("td", "sub", "no requests yet");
      td.colSpan = 5;
      tr.append(td);
      body.append(tr);
    }
  }

  function pushSpark(t, rate, total) {
    if (t == null) return;
    sparkPoints.push([t, rate || 0]);
    while (sparkPoints.length > SPARK_MAX) sparkPoints.shift();
    const t0 = sparkPoints[0][0], t1 = sparkPoints[sparkPoints.length - 1][0];
    const span = Math.max(t1 - t0, 1);
    const pts = sparkPoints.map(([x, y]) => ((x - t0) / span * 300).toFixed(1) + "," + (78 - y * 76).toFixed(1));
    $("spark-line").setAttribute("points", pts.join(" "));
    $("spark-caption").textContent = "cumulative hit rate " + fmtPct(rate) + " over " + fmtInt(total) +
      " requests; last " + Math.round(span) + " s shown";
  }

  function connect() {
    fetch("/stats", { cache: "no-store" }).then((r) => r.json()).then(render).catch(() => {});
    if (!window.EventSource) return;
    const es = new EventSource("/stats/stream");
    es.onmessage = (ev) => {
      try { render(JSON.parse(ev.data)); $("status-line").textContent = ""; } catch (e) { /* skip bad frame */ }
    };
    es.onerror = () => { $("status-line").textContent = "live updates reconnecting…"; };
  }

  // ---------- calling the proxy ----------
  // Returns {result, status, similarity, serverMs, clientMs, text, error}.
  async function chat(prompt, context, stream) {
    const started = performance.now();
    const body = {
      model: "mock-1",
      messages: [{ role: "system", content: context }, { role: "user", content: prompt }],
      stream: !!stream,
    };
    const out = { result: null, status: 0, similarity: null, serverMs: null, clientMs: null, text: "", error: null };
    let res;
    try {
      res = await fetch("/v1/chat/completions", {
        method: "POST",
        headers: { "Content-Type": "application/json", "Authorization": "Bearer " + apiKey },
        body: JSON.stringify(body),
      });
    } catch (e) {
      out.error = "network error";
      return out;
    }
    out.status = res.status;
    out.result = res.headers.get("X-Semcache") || (res.ok ? "?" : "ERROR");
    const sim = res.headers.get("X-Semcache-Similarity");
    out.similarity = sim == null ? null : parseFloat(sim);
    const lat = res.headers.get("X-Semcache-Latency-Ms");
    out.serverMs = lat == null ? null : parseFloat(lat);
    if (!res.ok) {
      try { const j = await res.json(); out.error = (j.error && j.error.message) || res.statusText; }
      catch (e) { out.error = res.statusText || "error"; }
      if (res.status === 429) out.result = "LIMITED";
      out.clientMs = performance.now() - started;
      return out;
    }
    if (stream && res.body) {
      const reader = res.body.getReader();
      const dec = new TextDecoder();
      let buf = "";
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let i;
        while ((i = buf.indexOf("\n\n")) >= 0) {
          const frame = buf.slice(0, i);
          buf = buf.slice(i + 2);
          for (const line of frame.split("\n")) {
            if (!line.startsWith("data:")) continue;
            const data = line.slice(5).trim();
            if (data === "[DONE]") continue;
            try {
              const j = JSON.parse(data);
              const d = j.choices && j.choices[0] && j.choices[0].delta;
              if (d && d.content) { out.text += d.content; if (out.onToken) out.onToken(out.text); }
            } catch (e) { /* ignore partial */ }
          }
        }
      }
    } else {
      try {
        const j = await res.json();
        out.text = (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || "";
      } catch (e) { out.error = "bad response"; }
    }
    out.clientMs = performance.now() - started;
    return out;
  }

  function freshContext(name) {
    runCounter += 1;
    return "You are a support assistant. (dashboard " + name + " run " + session + "-" + runCounter + ")";
  }

  function herdSize() {
    // Stay under the per-address burst in demo mode so the rush is not rate limited by our own limiter.
    const burst = stats && stats.demo ? stats.demo.per_ip_burst : 16;
    return Math.max(2, Math.min(16, Math.floor(burst) - 2));
  }

  function stepLine(list, label, r) {
    const li = el("li");
    li.append(badge(r.result), " ");
    li.append(el("span", "q", label));
    const parts = [];
    if (r.clientMs != null) parts.push(fmtMs(r.clientMs));
    if (r.similarity != null) parts.push("similarity " + r.similarity.toFixed(3));
    if (r.error) parts.push(r.error);
    li.append(el("div", "sub", parts.join(" · ")));
    list.append(li);
  }

  const scenarios = {
    async repeat(card) {
      const ctx = freshContext("repeat");
      const q = "What is your refund policy?";
      const steps = card.querySelector(".steps");
      const a = await chat(q, ctx, false); stepLine(steps, "“" + q + "”", a);
      const b = await chat(q, ctx, false); stepLine(steps, "same question again", b);
      return b.result === "HIT"
        ? ["good", "Second answer came from the cache: " + fmtMs(b.clientMs) + " instead of " + fmtMs(a.clientMs) + "."]
        : ["bad", "Expected a HIT on the repeat, got " + b.result + "."];
    },
    async reword(card) {
      const ctx = freshContext("reword");
      const q1 = "How do I cancel my subscription?", q2 = "How can I cancel my subscription?";
      const steps = card.querySelector(".steps");
      const a = await chat(q1, ctx, false); stepLine(steps, "“" + q1 + "”", a);
      const b = await chat(q2, ctx, false); stepLine(steps, "“" + q2 + "”", b);
      if (b.result === "HIT") return ["good", "Different text, same meaning: served from the cache (similarity " + b.similarity.toFixed(3) + ")."];
      if (stats && !stats.embedder_ready) return ["bad", "The embedder is off, so only exact repeats can hit."];
      return ["bad", "Not similar enough for the threshold; went to the LLM (" + b.result + ")."];
    },
    async trap(card) {
      const ctx = freshContext("trap");
      const q1 = "What does the warranty cover?", q2 = "What does the warranty not cover?";
      const steps = card.querySelector(".steps");
      const a = await chat(q1, ctx, false); stepLine(steps, "“" + q1 + "”", a);
      const b = await chat(q2, ctx, false); stepLine(steps, "“" + q2 + "”", b);
      if (b.result === "HIT") {
        return ["bad", "Wrong answer served: the model rates these " + b.similarity.toFixed(3) +
          " similar, so the opposite question got the cached answer. This is why the threshold is set " +
          "conservatively, and why some apps should cache exact repeats only."];
      }
      return ["good", "Not matched (" + b.result + "): the second question went to the LLM."];
    },
    async herd(card) {
      const ctx = freshContext("herd");
      const n = herdSize();
      const q = "Is there an outage right now?";
      const steps = card.querySelector(".steps");
      const t0 = performance.now();
      const all = await Promise.all(Array.from({ length: n }, () => chat(q, ctx, false)));
      const wall = performance.now() - t0;
      const count = {};
      for (const r of all) count[r.result] = (count[r.result] || 0) + 1;
      for (const [k, v] of Object.entries(count)) {
        const li = el("li");
        li.append(badge(k), " ", el("span", "q", "× " + v));
        steps.append(li);
      }
      const upstream = count.MISS || 0;
      return upstream === 1
        ? ["good", n + " requests, 1 upstream call: " + (count.COALESCED || 0) + " shared the call in flight, " +
            (count.HIT || 0) + " found it cached. All answered in " + fmtMs(wall) + "."]
        : ["bad", n + " requests made " + upstream + " upstream calls (timing can split a rush)."];
    },
  };

  for (const card of document.querySelectorAll(".scenario")) {
    const btn = card.querySelector("button");
    btn.addEventListener("click", async () => {
      const fn = scenarios[card.dataset.scenario];
      if (!fn) return;
      btn.disabled = true;
      card.querySelector(".steps").replaceChildren();
      const v = card.querySelector(".verdict");
      v.textContent = "running…";
      v.className = "verdict";
      try {
        const [cls, msg] = await fn(card);
        v.textContent = msg;
        v.className = "verdict " + cls;
      } catch (e) {
        v.textContent = "failed: " + e;
        v.className = "verdict bad";
      } finally {
        btn.disabled = false;
      }
    });
  }

  // ---------- try it ----------
  const tryContext = "You are a support assistant. (dashboard try-it " + session + ")";
  $("try").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const prompt = $("try-prompt").value.trim();
    if (!prompt) return;
    if (!$("key-row").hidden) apiKey = $("try-key").value.trim();
    const btn = ev.submitter || $("try").querySelector("button");
    btn.disabled = true;
    $("try-result").hidden = false;
    $("try-badge").className = "badge";
    $("try-badge").textContent = "…";
    $("try-meta").textContent = "";
    $("try-text").textContent = "";
    const r = await chat(prompt, tryContext, $("try-stream").checked);
    btn.disabled = false;
    $("try-badge").className = "badge " + (r.result || "");
    $("try-badge").textContent = r.result || "?";
    const meta = [];
    if (r.status) meta.push("HTTP " + r.status);
    if (r.clientMs != null) meta.push(fmtMs(r.clientMs) + " in the browser");
    if (r.serverMs != null) meta.push(fmtMs(r.serverMs) + " in the proxy");
    if (r.similarity != null) meta.push("similarity " + r.similarity.toFixed(3));
    $("try-meta").textContent = meta.join(" · ");
    $("try-text").textContent = r.error ? r.error : r.text;
    const li = el("li");
    li.append(badge(r.result), " ", prompt);
    $("history").prepend(li);
  });

  // ---------- reset ----------
  $("reset").addEventListener("click", async () => {
    const btn = $("reset");
    btn.disabled = true;
    try {
      const res = await fetch("/demo/reset", { method: "POST" });
      const j = await res.json().catch(() => ({}));
      $("status-line").textContent = res.ok ? "cleared " + (j.purged || 0) + " cached answers"
        : "reset failed: " + ((j.error && j.error.message) || res.status);
    } finally {
      btn.disabled = false;
    }
  });

  connect();
})();
