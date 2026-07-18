/* ===========================================================
   PORTFOLIO ENGINE — FRONTEND LOGIC
=========================================================== */

const API_BASE = (window.location.port === "8080") ? "" : "http://localhost:8080";
const REFRESH_INTERVAL_MS = 30000;
const MAX_CHART_POINTS = 60;

const TICKER_NAMES = {
  AAPL:"Apple Inc.",TSLA:"Tesla Inc.",GOOGL:"Alphabet Inc.",GOOG:"Alphabet Inc.",
  MSFT:"Microsoft Corp.",NVDA:"NVIDIA Corp.",AMZN:"Amazon.com Inc.",
  META:"Meta Platforms",NFLX:"Netflix Inc.",AMD:"Advanced Micro Devices",
  INTC:"Intel Corp.",ORCL:"Oracle Corp.",CRM:"Salesforce Inc.",PYPL:"PayPal",
  UBER:"Uber Technologies",LYFT:"Lyft Inc.",SNAP:"Snap Inc.",SPOT:"Spotify",
};

/* ===========================================================
   STATE
=========================================================== */
let chart = null;
const chartLabels = [];
const chartData   = [];
let lastPortfolio  = [];
let lastChartLen   = 0;
let lastAlertIds   = new Set();
let audioCtx       = null;

/* ===========================================================
   HELPERS
=========================================================== */
function money(n, decimals = 2) {
  if (n == null || isNaN(n)) return "$0.00";
  const sign = n < 0 ? "-" : "";
  return sign + "$" + Math.abs(n).toLocaleString(undefined,
    { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
}
function pct(n, decimals = 2) {
  if (n == null || isNaN(n)) return "0.00%";
  return (n >= 0 ? "▲ " : "▼ ") + Math.abs(n).toFixed(decimals) + "%";
}
function companyName(t) { return TICKER_NAMES[t.toUpperCase()] || t.toUpperCase(); }
function timeAgo(ts) {
  if (!ts) return "";
  const t = new Date(ts.replace(" ", "T") + "Z");
  if (isNaN(t)) return ts;
  const s = Math.floor((Date.now() - t) / 1000);
  if (s < 60) return s + "s ago";
  if (s < 3600) return Math.floor(s/60) + "m ago";
  if (s < 86400) return Math.floor(s/3600) + "h ago";
  return Math.floor(s/86400) + "d ago";
}
function setIfChanged(el, text) { if (el && el.textContent !== text) el.textContent = text; }
function setDot(dotEl, textEl, ok, okT, badT) {
  if (!dotEl) return;
  dotEl.classList.toggle("ok",  ok);
  dotEl.classList.toggle("bad", !ok);
  if (textEl) textEl.textContent = ok ? okT : badT;
}
function showToast(msg, type = "success") {
  const t = document.getElementById("toast");
  t.textContent = msg;
  t.className = "toast show " + type;
  clearTimeout(showToast._t);
  showToast._t = setTimeout(() => { t.className = "toast " + type; }, 4000);
}

/* ===========================================================
   ALERT SOUND
=========================================================== */
function playAlertSound() {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const ctx = audioCtx;
    if (ctx.state === "suspended") ctx.resume();
    [[880, 0, 0.15], [660, 0.2, 0.15]].forEach(([freq, startOffset, dur]) => {
      const osc  = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.frequency.value = freq;
      osc.type = "sine";
      gain.gain.setValueAtTime(0.4, ctx.currentTime + startOffset);
      gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + startOffset + dur);
      osc.start(ctx.currentTime + startOffset);
      osc.stop(ctx.currentTime + startOffset + dur);
    });
  } catch(e) { console.warn("Audio failed:", e); }
}

// Browsers block audio from playing until a real user gesture has occurred
// on the page. Alerts fire from an automatic 30s timer, which doesn't count
// as a gesture - so without this, the very first alert sound (and often
// every one after it) would silently fail. This unlocks/resumes the audio
// context on the user's first click anywhere on the page.
function unlockAudioOnFirstInteraction() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  if (audioCtx.state === "suspended") audioCtx.resume();
  document.removeEventListener("click", unlockAudioOnFirstInteraction);
}
document.addEventListener("click", unlockAudioOnFirstInteraction);

/* ===========================================================
   BROWSER NOTIFICATIONS
=========================================================== */
function requestNotificationPermission() {
  if ("Notification" in window && Notification.permission === "default")
    Notification.requestPermission();
}
function sendBrowserNotification(title, body) {
  if ("Notification" in window && Notification.permission === "granted")
    new Notification(title, { body, icon: "" });
}

/* ===========================================================
   API
=========================================================== */
async function apiGet(path) {
  const res = await fetch(API_BASE + path);
  if (!res.ok) throw new Error("HTTP " + res.status);
  return res.json();
}
async function apiPost(path, body) {
  const res = await fetch(API_BASE + path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error("HTTP " + res.status);
  return res.json();
}
async function safeGet(path) {
  try { return await apiGet(path); }
  catch(e) { console.warn("[safeGet]", path, e.message); return null; }
}
async function lookupPrice(ticker) {
  try {
    const data = await apiGet("/price?ticker=" + ticker.toUpperCase());
    return data && data.price > 0 ? data.price : null;
  } catch { return null; }
}

/* ===========================================================
   RENDERING — STATS
=========================================================== */
function renderStats(summary, portfolio) {
  const totalInvested = portfolio.reduce((s, x) => s + x.buyPrice * x.quantity, 0);
  let portfolioValue = (summary && summary.portfolioValue > 0)
    ? summary.portfolioValue
    : portfolio.reduce((s, x) => s + (x.livePrice > 0 ? x.livePrice : x.buyPrice) * x.quantity, 0);
  let totalPnL = (summary && summary.totalPnL != null)
    ? summary.totalPnL
    : portfolio.reduce((s, x) => s + (x.pnl || 0), 0);
  const returnPct = totalInvested > 0 ? (totalPnL / totalInvested) * 100 : 0;

  setIfChanged(document.getElementById("statPortfolioValue"), money(portfolioValue));
  setIfChanged(document.getElementById("statTotalPnl"),       money(totalPnL));
  setIfChanged(document.getElementById("statTotalInvested"),  money(totalInvested));

  const retEl = document.getElementById("statTotalReturn");
  const retText = pct(returnPct) + " total return";
  if (retEl.textContent !== retText) retEl.textContent = retText;
  retEl.className = "stat-sub " + (returnPct >= 0 ? "green" : "red");

  const pnlEl = document.getElementById("statTotalPnl");
  pnlEl.className = totalPnL >= 0 ? "green" : "red";

  setIfChanged(document.getElementById("statTotalPnlSub"),
    portfolio.length > 0 ? "across " + portfolio.length + " positions" : "no positions yet");
  setIfChanged(document.getElementById("statHoldingsCount"),
    String(summary?.stockCount ?? portfolio.length));
  setIfChanged(document.getElementById("statBest"),
    (summary?.bestPerformer && summary.bestPerformer !== "N/A") ? summary.bestPerformer : "—");
  setIfChanged(document.getElementById("statWorst"),
    (summary?.worstPerformer && summary.worstPerformer !== "N/A") ? summary.worstPerformer : "—");
  setIfChanged(document.getElementById("statLastUpdate"),
    new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }));
}

/* ===========================================================
   RENDERING — HOLDINGS (no-flicker smart update)
=========================================================== */
function buildRow(s) {
  const live = s.livePrice > 0 ? s.livePrice : s.buyPrice;
  const mv   = live * s.quantity;
  const pnl  = (live - s.buyPrice) * s.quantity;
  const pp   = s.buyPrice > 0 ? ((live - s.buyPrice) / s.buyPrice) * 100 : 0;
  return `<tr data-ticker="${s.ticker}">
    <td><div class="ticker-cell">
      <div class="ticker-avatar">${s.ticker.slice(0,2).toUpperCase()}</div>
      <span class="ticker-label">${s.ticker.toUpperCase()}</span>
    </div></td>
    <td>${companyName(s.ticker)}</td>
    <td>${s.quantity}</td>
    <td>${money(s.buyPrice)}</td>
    <td class="cell-current">${s.livePrice > 0 ? money(s.livePrice) : "—"}</td>
    <td class="cell-mv">${money(mv)}</td>
    <td class="cell-pnl ${pnl >= 0 ? "green" : "red"}">
      <span class="pnl-main">${pnl >= 0 ? "+" : ""}${money(pnl)}</span>
      <span class="pnl-sub" style="font-size:11px;opacity:.75;display:block;">${pp >= 0 ? "+" : ""}${pp.toFixed(2)}%</span>
    </td>
    <td>
      <button class="delete-btn" onclick="deletePosition('${s.ticker}')" title="Remove position">
        <i class="fa-solid fa-trash"></i>
      </button>
    </td>
  </tr>`;
}

function renderHoldings(portfolio) {
  const tbody = document.getElementById("holdingsTableBody");
  if (!portfolio || portfolio.length === 0) {
    tbody.innerHTML = `<tr><td colspan="8" class="empty-row">No positions yet — add one to get started.</td></tr>`;
    return;
  }
  const existingRows = tbody.querySelectorAll("tr[data-ticker]");
  if (existingRows.length !== portfolio.length) {
    tbody.innerHTML = portfolio.map(s => buildRow(s)).join("");
    return;
  }
  portfolio.forEach((s, i) => {
    const row = existingRows[i];
    if (!row || row.dataset.ticker !== s.ticker) {
      tbody.innerHTML = portfolio.map(x => buildRow(x)).join("");
      return;
    }
    const live = s.livePrice > 0 ? s.livePrice : s.buyPrice;
    const mv   = live * s.quantity;
    const pnl  = (live - s.buyPrice) * s.quantity;
    const pp   = s.buyPrice > 0 ? ((live - s.buyPrice) / s.buyPrice) * 100 : 0;
    setIfChanged(row.querySelector(".cell-current"), s.livePrice > 0 ? money(s.livePrice) : "—");
    setIfChanged(row.querySelector(".cell-mv"),      money(mv));
    const pnlEl = row.querySelector(".cell-pnl");
    if (pnlEl) {
      pnlEl.className = "cell-pnl " + (pnl >= 0 ? "green" : "red");
      const mainSpan = pnlEl.querySelector(".pnl-main");
      const subSpan  = pnlEl.querySelector(".pnl-sub");
      if (mainSpan) setIfChanged(mainSpan, (pnl >= 0 ? "+" : "") + money(pnl));
      if (subSpan)  setIfChanged(subSpan,  (pp  >= 0 ? "+" : "") + pp.toFixed(2) + "%");
    }
  });
}

async function deletePosition(ticker) {
  if (!confirm(`Remove ${ticker} from your portfolio?`)) return;
  try {
    await apiPost("/portfolio/delete", { ticker });
    showToast(`${ticker} removed from portfolio`, "success");
    refreshAll();
  } catch { showToast("Failed to remove position", "error"); }
}

/* ===========================================================
   DISMISS A TRIGGERED ALERT FROM THE PANEL
=========================================================== */
async function dismissTriggeredAlert(ticker, condition, threshold, timestamp) {
  try {
    await apiPost("/alert/dismiss", { ticker, condition, threshold, timestamp });
    refreshAll();
  } catch {
    // If endpoint fails just remove from UI
    refreshAll();
  }
}

/* ===========================================================
   RENDERING — TRIGGERED ALERTS
=========================================================== */
function renderAlerts(alerts) {
  const list  = document.getElementById("alertsList");
  const badge = document.getElementById("alertCountBadge");
  const stat  = document.getElementById("statAlertCount");

  setIfChanged(badge, String(alerts.length));
  setIfChanged(stat,  String(alerts.length));

  if (!alerts || alerts.length === 0) {
    list.innerHTML = `<div class="empty-row">No alerts triggered yet.</div>`;
    lastAlertIds = new Set();
    return;
  }

  const sorted = [...alerts].sort((a,b) => (b.timestamp||"").localeCompare(a.timestamp||""));
  const currentIds = new Set(sorted.map(a => a.ticker + a.timestamp));
  const isFirstLoad = lastAlertIds.size === 0;

  if (!isFirstLoad) {
    sorted.forEach(a => {
      const id = a.ticker + a.timestamp;
      if (!lastAlertIds.has(id)) {
        playAlertSound();
        const dir = a.condition === "ABOVE" ? "above" : "below";
        sendBrowserNotification(
          `⚡ ${a.ticker} Alert Triggered`,
          `${a.ticker} went ${dir} $${a.threshold.toFixed(2)} · Live: $${a.livePrice.toFixed(2)}`
        );
        showToast(`⚡ ${a.ticker} ${dir} $${a.threshold.toFixed(2)} — Live $${a.livePrice.toFixed(2)}`, "alert");
      }
    });
  }
  lastAlertIds = currentIds;

  list.innerHTML = sorted.slice(0, 12).map(a => {
    const isUp = a.condition === "ABOVE";
    const id = encodeURIComponent(a.ticker + "|" + a.condition + "|" + a.threshold + "|" + a.timestamp);
    return `<div class="alert-item" id="alert-${id}">
      <div class="alert-dot ${isUp ? "up" : "down"}">
        <i class="fa-solid fa-${isUp ? "arrow-up" : "arrow-down"}"></i>
      </div>
      <div class="alert-body">
        <strong>${a.ticker}</strong>
        <span>${isUp ? "Above" : "Below"} ${money(a.threshold)} · Live ${money(a.livePrice)}</span>
      </div>
      <div class="alert-time">${timeAgo(a.timestamp)}</div>
      <button class="icon-btn red-btn" title="Dismiss alert"
        onclick="dismissTriggeredAlert('${a.ticker}','${a.condition}',${a.threshold},'${a.timestamp}')">
        <i class="fa-solid fa-xmark"></i>
      </button>
    </div>`;
  }).join("");
}

/* ===========================================================
   RENDERING — ACTIVE ALERTS PANEL (manage tab)
=========================================================== */
async function renderActiveAlerts() {
  const list = document.getElementById("activeAlertsList");
  if (!list) return;
  const alerts = await safeGet("/activealerts");
  if (!alerts || alerts.length === 0) {
    list.innerHTML = `<div class="empty-row">No alerts set yet. Create one above.</div>`;
    return;
  }
  list.innerHTML = alerts.map(a => {
    const isUp = a.condition === "ABOVE";
    return `<div class="alert-item">
      <div class="alert-dot ${isUp ? "up" : "down"}">
        <i class="fa-solid fa-${isUp ? "arrow-up" : "arrow-down"}"></i>
      </div>
      <div class="alert-body">
        <strong>${a.ticker}</strong>
        <span>${isUp ? "Above" : "Below"} ${money(a.threshold)}</span>
      </div>
      <div style="display:flex;gap:6px;flex-shrink:0;">
        <button class="icon-btn green-btn" title="Reset (allow re-fire)"
          onclick="resetAlert('${a.ticker}','${a.condition}',${a.threshold})">
          <i class="fa-solid fa-rotate-right"></i>
        </button>
        <button class="icon-btn red-btn" title="Delete alert"
          onclick="deleteAlert('${a.ticker}','${a.condition}',${a.threshold})">
          <i class="fa-solid fa-trash"></i>
        </button>
      </div>
    </div>`;
  }).join("");
}

async function clearAllActiveAlerts() {
  if (!confirm("Delete ALL active alerts? This cannot be undone.")) return;
  try {
    await apiGet("/clearactivealerts");
    showToast("All alerts cleared", "success");
    renderActiveAlerts();
    refreshAll();
  } catch { showToast("Failed to clear alerts", "error"); }
}

async function deleteAlert(ticker, condition, threshold) {
  if (!confirm(`Delete alert: ${ticker} ${condition} $${threshold}?`)) return;
  try {
    await apiPost("/alert/delete", { ticker, condition, threshold });
    showToast(`Alert deleted: ${ticker} ${condition} $${threshold}`, "success");
    renderActiveAlerts();
  } catch { showToast("Failed to delete alert", "error"); }
}

async function resetAlert(ticker, condition, threshold) {
  try {
    await apiPost("/alert/reset", { ticker, condition, threshold });
    showToast(`Alert reset — ${ticker} ${condition} $${threshold} will fire again on next crossing`, "success");
  } catch { showToast("Failed to reset alert", "error"); }
}

/* ===========================================================
   CHART
=========================================================== */
function initChart() {
  const ctx = document.getElementById("portfolioChart");
  if (!ctx) return;
  chart = new Chart(ctx, {
    type: "line",
    data: { labels: chartLabels, datasets: [{
      label: "Portfolio Value", data: chartData,
      borderColor: "#18f2b2", backgroundColor: "rgba(24,242,178,.15)",
      fill: true, tension: 0.4, pointRadius: 3, pointHoverRadius: 6, borderWidth: 2.5,
    }]},
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      plugins: {
        legend: { display: false },
        tooltip: { callbacks: { label: c => "  " + money(c.parsed.y) } },
      },
      interaction: { intersect: false, mode: "index" },
      scales: {
        x: { grid: { color: "rgba(255,255,255,.04)" }, ticks: { color: "#8f9cab", maxTicksLimit: 8 } },
        y: { grid: { color: "rgba(255,255,255,.04)" }, ticks: { color: "#8f9cab", callback: v => "$" + v.toLocaleString() } },
      },
    },
  });
}

async function loadChartHistory(force = false) {
  const history = await safeGet("/history?limit=" + MAX_CHART_POINTS);
  if (!history || history.length === 0) return;
  if (!force && history.length === lastChartLen) return;
  lastChartLen = history.length;
  const labels = history.map(p => {
    const t = new Date(p.timestamp.replace(" ", "T") + "Z");
    return isNaN(t) ? p.timestamp : t.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  });
  chartLabels.splice(0, chartLabels.length, ...labels);
  chartData.splice(0, chartData.length, ...history.map(p => p.value));
  if (chart) chart.update("none");
}

document.getElementById("clearChartBtn")?.addEventListener("click", () => {
  lastChartLen = 0; loadChartHistory(true);
});

/* ===========================================================
   MAIN REFRESH
=========================================================== */
async function refreshAll() {
  const [summary, portfolio, alerts] = await Promise.all([
    safeGet("/summary"), safeGet("/portfolio"), safeGet("/alerts"),
  ]);
  const ok = !!(summary || portfolio);
  setDot(document.getElementById("backendDot"), document.getElementById("backendStatusText"), ok, "Backend: Connected", "Backend: Offline");
  setDot(document.getElementById("dbDot"),      document.getElementById("dbStatusText"),      ok, "SQLite: Healthy",   "SQLite: Unreachable");

  if (portfolio) { lastPortfolio = portfolio; renderHoldings(portfolio); }
  if (summary || portfolio) renderStats(summary || {}, lastPortfolio);
  if (alerts) renderAlerts(alerts);
  loadChartHistory();
  // Keep active alerts panel live if modal is open
  const alertModal = document.getElementById("setAlertModal");
  if (alertModal && alertModal.classList.contains("open")) renderActiveAlerts();
}

/* ===========================================================
   MODALS
=========================================================== */
const overlay          = document.getElementById("modalOverlay");
const addPositionModal = document.getElementById("addPositionModal");
const setAlertModal    = document.getElementById("setAlertModal");

function openModal(m) {
  overlay.classList.add("open");
  document.querySelectorAll(".modal").forEach(x => x.classList.remove("open"));
  m.classList.add("open");
  if (m === setAlertModal) renderActiveAlerts();
}
function closeModals() {
  overlay.classList.remove("open");
  document.querySelectorAll(".modal").forEach(x => x.classList.remove("open"));
  document.querySelectorAll(".modal-error").forEach(x => x.textContent = "");
}

["navAddPosition","openAddPositionBtn","qaAddPosition"].forEach(id => {
  document.getElementById(id)?.addEventListener("click", e => { e.preventDefault(); openModal(addPositionModal); });
});
["navSetAlert","qaSetAlert"].forEach(id => {
  document.getElementById(id)?.addEventListener("click", e => { e.preventDefault(); openModal(setAlertModal); });
});
overlay.addEventListener("click", e => { if (e.target === overlay) closeModals(); });
document.querySelectorAll("[data-close]").forEach(b => b.addEventListener("click", closeModals));
document.addEventListener("keydown", e => { if (e.key === "Escape") closeModals(); });

/* --- Add Position --- */
document.getElementById("apTicker")?.addEventListener("blur", async function() {
  const ticker = this.value.trim().toUpperCase();
  if (!ticker) return;
  const priceEl = document.getElementById("apBuyPrice");
  const hintEl  = document.getElementById("apPriceHint");
  if (hintEl) hintEl.textContent = "Fetching price…";
  const price = await lookupPrice(ticker);
  if (price) {
    priceEl.value = price.toFixed(2);
    if (hintEl) hintEl.textContent = `Current market price: $${price.toFixed(2)}`;
  } else {
    if (hintEl) hintEl.textContent = "Could not fetch price — enter manually.";
  }
});

document.getElementById("addPositionForm")?.addEventListener("submit", async e => {
  e.preventDefault();
  const ticker   = document.getElementById("apTicker").value.trim().toUpperCase();
  const buyPrice = parseFloat(document.getElementById("apBuyPrice").value);
  const quantity = parseInt(document.getElementById("apQuantity").value, 10);
  const errEl    = document.getElementById("apError");
  const btn      = e.target.querySelector(".modal-submit");
  if (!ticker || isNaN(buyPrice) || isNaN(quantity) || quantity <= 0) {
    errEl.textContent = "Please fill in all fields."; return;
  }
  btn.disabled = true; btn.textContent = "Adding…";
  try {
    await apiPost("/stock", { ticker, buyPrice, quantity });
    showToast(`${ticker} added to portfolio`, "success");
    e.target.reset();
    const hintEl = document.getElementById("apPriceHint");
    if (hintEl) hintEl.textContent = "";
    closeModals(); refreshAll();
  } catch { errEl.textContent = "Couldn't reach backend."; }
  finally { btn.disabled = false; btn.textContent = "Add to Portfolio"; }
});

/* --- Set Alert --- */
document.getElementById("setAlertForm")?.addEventListener("submit", async e => {
  e.preventDefault();
  const ticker    = document.getElementById("saTicker").value.trim().toUpperCase();
  const condition = document.getElementById("saCondition").value;
  const threshold = parseFloat(document.getElementById("saThreshold").value);
  const errEl     = document.getElementById("saError");
  const btn       = e.target.querySelector(".modal-submit");
  if (!ticker || isNaN(threshold)) { errEl.textContent = "Please fill in all fields."; return; }
  btn.disabled = true; btn.textContent = "Creating…";
  try {
    await apiPost("/alert", { ticker, threshold, condition });
    showToast(`⚡ Alert created: ${ticker} ${condition} $${threshold.toFixed(2)}`, "success");
    e.target.reset();
    await renderActiveAlerts();
  } catch { errEl.textContent = "Couldn't reach backend."; }
  finally { btn.disabled = false; btn.textContent = "Create Alert"; }
});

/* ===========================================================
   SIDEBAR NAV
=========================================================== */
const sectionHeadings = {
  overview:    { title: "Overview",      sub: "Real-time portfolio summary and performance" },
  holdings:    { title: "Holdings",      sub: "All your current stock positions" },
  performance: { title: "Performance",   sub: "Portfolio value over time" },
  alerts:      { title: "Alert History", sub: "Price alerts that have triggered" },
};

document.querySelectorAll(".nav-link[data-section]").forEach(link => {
  link.addEventListener("click", e => {
    e.preventDefault();
    document.querySelectorAll(".nav-link").forEach(l => l.classList.remove("active"));
    link.classList.add("active");
    const targets = { overview:".stats-grid", holdings:".holdings-section", performance:".graph-card", alerts:".alerts-card" };
    document.querySelector(targets[link.dataset.section])?.scrollIntoView({ behavior:"smooth", block:"start" });

    const heading = sectionHeadings[link.dataset.section];
    if (heading) {
      const topbar = document.querySelector(".topbar-left");
      if (topbar) {
        const h1 = topbar.querySelector("h1");
        const p  = topbar.querySelector("p");
        if (h1) h1.textContent = heading.title;
        if (p)  p.textContent  = heading.sub;
      }
    }
  });
});

/* ===========================================================
   TILT / GLOW / CLOCK
=========================================================== */
document.querySelectorAll(".stat-card,.mini-card,.quick-action").forEach(card => {
  card.addEventListener("mousemove", e => {
    const r = card.getBoundingClientRect();
    card.style.transform = `perspective(1000px) rotateX(${-(e.clientY-r.top-r.height/2)/24}deg) rotateY(${(e.clientX-r.left-r.width/2)/24}deg) translateY(-4px)`;
  });
  card.addEventListener("mouseleave", () => { card.style.transform = ""; });
});

const glow = document.createElement("div");
glow.className = "mouseGlow";
document.body.appendChild(glow);
window.addEventListener("mousemove", e => { glow.style.left = e.clientX+"px"; glow.style.top = e.clientY+"px"; });

setInterval(() => {
  const t = new Date().toLocaleTimeString([], { hour:"2-digit", minute:"2-digit", second:"2-digit" });
  document.querySelectorAll(".liveClock").forEach(el => el.textContent = t);
}, 1000);

/* ===========================================================
   INIT
=========================================================== */
requestNotificationPermission();
initChart();
refreshAll();
setInterval(refreshAll, REFRESH_INTERVAL_MS);