// Oclean coverage card: a Lovelace custom card that draws the 8 or 12 per-zone
// brushing values of an Oclean session as a colored mouth map (upper and lower
// arch, left/right side, outer/inner surface). Read-only: it only reads the
// existing zone sensor states plus recorder history, never talks to the
// brush. No build step, no dependencies; copy this single file into
// <config>/www/ and add it as a dashboard resource
// (/local/oclean-coverage-card.js, type module).
//
// Surface order (matches the component's gesture_zone_1..8 entities):
//   1 left  upper outer   5 right upper outer
//   2 left  upper inner   6 right upper inner
//   3 left  lower outer   7 right lower outer
//   4 left  lower inner   8 right lower inner
// With 12 zones (the X Ultra 20 zone_time_1..12 entities) 1-8 cover the back
// teeth in the same order and the front teeth, canine to canine, follow:
//   9 upper front outer  10 upper front inner
//  11 lower front outer  12 lower front inner
//
// History source: recorder entity history for the zone/score/coverage entities,
// grouped by the last-session timestamp sensor (time_entity). The node
// re-publishes the same last session on every reboot/poll, so one brushing shows
// up many times in entity history under the same time_entity value; the card
// dedups by that value to recover one entry per real session. The arrows or
// slider step through the sessions found. History depth is bounded by recorder
// retention; it defaults to scanning the last 183 days. Backfilled rings only
// expose their newest record through the entities, so trip history collapses to
// a single session there (that is a recorder limitation, not the card's).

const CARD_VERSION = "1.2.1";

// HTML-escape any value interpolated into the innerHTML template. States and
// labels are numeric or safe today, but escaping keeps a string entity or a
// crafted label out of the DOM as markup.
const esc = (v) =>
  String(v).replace(
    /[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c],
  );

// Geometry (SVG units): two horseshoe dental arches drawn as rows of discrete
// teeth instead of solid bands. The upper arch is the top half of a ring
// centered at CY_UP and opens downward; the lower arch is the bottom half of a
// ring centered at CY_LOW and opens upward. The two centers are offset
// vertically so the arches' open (flat) edges leave a clear horizontal mouth
// gap between them. Each arch holds 16 teeth (8 per quadrant, the real adult
// count), each tooth split into an outer facial cell and an inner lingual cell.
// The angular gaps between teeth make the two arches read as 32 separate teeth.
const CX = 160;
const R_IN = 80; // tongue-side edge of the tooth ring
const R_MID = 96; // boundary between a tooth's inner and outer cell
const R_OUT = 112; // lip-side outer radius
const LABEL_R = 128; // outer-surface value badges, outside the teeth
const LABEL_R_IN = 62; // inner-surface value badges, inside the arch
const CY_UP = 140; // center of the upper arch (top half, opens downward)
const CY_LOW = 164; // center of the lower arch (bottom half, opens upward)

// The two cells of a tooth meet with no gap or stroke between them, so a tooth
// reads as one tooth with two faces, not as two rows of teeth. A small angular
// gap separates the crowns.
const TEETH_PER_QUADRANT = 8;
const TOOTH_GAP_DEG = 3;

// Angular spans (deg, SVG y-down: 0=right, 90=bottom, 180=left, 270=top). The
// upper arch fills the top half (180..360), the lower arch the bottom half
// (0..180). A zone is one surface of a whole quadrant, so both surfaces span all
// 8 teeth of it: the outer one the lip-side cell of each tooth, the inner one
// the tongue-side cell.
const SURFACES = [
  { id: "ul_out", z: 0, label: "Upper left outer", a0: 180, a1: 270, teeth: 8, rIn: R_MID, rOut: R_OUT, labelR: LABEL_R, cy: CY_UP },
  { id: "ul_in", z: 1, label: "Upper left inner", a0: 180, a1: 270, teeth: 8, rIn: R_IN, rOut: R_MID, labelR: LABEL_R_IN, cy: CY_UP },
  { id: "ur_out", z: 4, label: "Upper right outer", a0: 270, a1: 360, teeth: 8, rIn: R_MID, rOut: R_OUT, labelR: LABEL_R, cy: CY_UP },
  { id: "ur_in", z: 5, label: "Upper right inner", a0: 270, a1: 360, teeth: 8, rIn: R_IN, rOut: R_MID, labelR: LABEL_R_IN, cy: CY_UP },
  { id: "lr_out", z: 6, label: "Lower right outer", a0: 0, a1: 90, teeth: 8, rIn: R_MID, rOut: R_OUT, labelR: LABEL_R, cy: CY_LOW },
  { id: "lr_in", z: 7, label: "Lower right inner", a0: 0, a1: 90, teeth: 8, rIn: R_IN, rOut: R_MID, labelR: LABEL_R_IN, cy: CY_LOW },
  { id: "ll_out", z: 2, label: "Lower left outer", a0: 90, a1: 180, teeth: 8, rIn: R_MID, rOut: R_OUT, labelR: LABEL_R, cy: CY_LOW },
  { id: "ll_in", z: 3, label: "Lower left inner", a0: 90, a1: 180, teeth: 8, rIn: R_IN, rOut: R_MID, labelR: LABEL_R_IN, cy: CY_LOW },
];

// 12 zones: each arch splits into the back teeth of either side (premolars and
// molars, 5 a side) and the 6 front teeth (canine to canine) across the
// midline. Even zones are drawn on the outer cell, as on the X Pro Elite.
const TOOTH_DEG = 90 / TEETH_PER_QUADRANT;
const BACK_DEG = 5 * TOOTH_DEG;
const SURFACES_12 = [
  ["Upper left back", 0, 180, 180 + BACK_DEG, 5, CY_UP],
  ["Upper front", 8, 180 + BACK_DEG, 360 - BACK_DEG, 6, CY_UP],
  ["Upper right back", 4, 360 - BACK_DEG, 360, 5, CY_UP],
  ["Lower right back", 6, 0, BACK_DEG, 5, CY_LOW],
  ["Lower front", 10, BACK_DEG, 180 - BACK_DEG, 6, CY_LOW],
  ["Lower left back", 2, 180 - BACK_DEG, 180, 5, CY_LOW],
].flatMap(([label, z, a0, a1, teeth, cy]) => [
  { z, label: `${label} outer`, a0, a1, teeth, rIn: R_MID, rOut: R_OUT, labelR: LABEL_R, cy },
  { z: z + 1, label: `${label} inner`, a0, a1, teeth, rIn: R_IN, rOut: R_MID, labelR: LABEL_R_IN, cy },
]);

function clamp01(v) {
  return Math.max(0, Math.min(1, v));
}

// Green (well brushed) to red (missed) by ratio in [0,1]; null -> neutral grey.
function ratioColor(ratio) {
  if (ratio === null || ratio === undefined) {
    return "var(--disabled-color, #9e9e9e)";
  }
  const hue = 120 * clamp01(ratio); // 0 red .. 120 green
  return `hsl(${hue.toFixed(0)}, 60%, 45%)`;
}

function rad(deg) {
  return (deg * Math.PI) / 180;
}

// SVG path for a ring sector between two angles and two radii, centered at cy.
function sectorPath(a0, a1, rInner, rOuter, cy) {
  const x = (r, a) => (CX + r * Math.cos(rad(a))).toFixed(2);
  const y = (r, a) => (cy + r * Math.sin(rad(a))).toFixed(2);
  const large = Math.abs(a1 - a0) > 180 ? 1 : 0;
  return [
    `M ${x(rOuter, a0)} ${y(rOuter, a0)}`,
    `A ${rOuter} ${rOuter} 0 ${large} 1 ${x(rOuter, a1)} ${y(rOuter, a1)}`,
    `L ${x(rInner, a1)} ${y(rInner, a1)}`,
    `A ${rInner} ${rInner} 0 ${large} 0 ${x(rInner, a0)} ${y(rInner, a0)}`,
    "Z",
  ].join(" ");
}

function toothCellPaths(s) {
  const span = s.a1 - s.a0;
  const n = s.teeth;
  const g = TOOTH_GAP_DEG;
  // Reserve a half-gap at each surface edge so two abutting runs leave a full
  // gap between their edge teeth. Without it the midline between the two
  // central incisors has no gap and reads as one fat tooth.
  const tw = span / n - g; // single tooth angular width
  const out = [];
  for (let i = 0; i < n; i++) {
    const b0 = s.a0 + g / 2 + i * (tw + g);
    out.push(sectorPath(b0, b0 + tw, s.rIn, s.rOut, s.cy));
  }
  return out;
}

class OcleanCoverageCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._config = null;
    this._hass = null;
    this._lastSignature = null;
    // History navigation state. _sessions is oldest-first; _idx selects one,
    // null means follow the live latest. _lastTime tracks the live session
    // timestamp so a new brushing triggers a single history refresh.
    this._sessions = null;
    this._idx = null;
    this._lastTime = null;
    this._fetching = false;
  }

  // zones: list of 8 or 12 entity ids in zone order, or zone_prefix: a string
  // to which "1".."zone_count" are appended (zone_count 8, or 12 for the X Ultra
  // 20).
  setConfig(config) {
    let zones = config.zones;
    if (!zones && config.zone_prefix) {
      const count = Number(config.zone_count || 8);
      if (count !== 8 && count !== 12) {
        throw new Error("oclean-coverage-card: 'zone_count' must be 8 or 12");
      }
      zones = [];
      for (let i = 1; i <= count; i++) {
        zones.push(`${config.zone_prefix}${i}`);
      }
    }
    if (!Array.isArray(zones) || (zones.length !== 8 && zones.length !== 12)) {
      throw new Error("oclean-coverage-card: provide 'zones' (8 or 12 entity ids) or 'zone_prefix'");
    }
    const normalize = config.normalize || "share";
    if (!["share", "max", "absolute"].includes(normalize)) {
      throw new Error("oclean-coverage-card: 'normalize' must be share, max or absolute");
    }
    this._config = {
      title: config.title || "",
      zones,
      mirror: config.mirror === true,
      normalize,
      target: Number(config.target || 15),
      score_entity: config.score_entity || null,
      coverage_entity: config.coverage_entity || null,
      time_entity: config.time_entity || null,
      history_days: Number(config.history_days || 183),
      labels: config.labels || {},
    };
    this._lastSignature = null;
    this._sessions = null;
    this._idx = null;
    this._lastTime = null;
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    // Refresh history on the first hass and whenever a new session appears. The
    // live timestamp sensor changing is the cheap signal for "new brushing".
    const cfg = this._config;
    if (cfg && cfg.time_entity && !this._fetching) {
      const live = this._liveStr(cfg.time_entity);
      const firstFetch = this._sessions === null;
      const newSession = !!live && live !== this._lastTime;
      if (firstFetch || newSession) {
        this._fetchHistory();
      }
    }
    this._render();
  }

  getCardSize() {
    return 6;
  }

  static getStubConfig() {
    return { zone_prefix: "sensor.oclean_zone_", title: "Brushing coverage" };
  }

  _liveStr(entityId) {
    if (!this._hass || !entityId) {
      return null;
    }
    const st = this._hass.states[entityId];
    if (!st || st.state === "unavailable" || st.state === "unknown" || st.state === "") {
      return null;
    }
    return st.state;
  }

  _liveNum(entityId) {
    const s = this._liveStr(entityId);
    if (s === null) {
      return null;
    }
    const n = Number(s);
    return Number.isFinite(n) ? n : null;
  }

  // Pull recorder history for the zone/score/coverage/time entities and group
  // it into past sessions keyed by each distinct last-session timestamp.
  async _fetchHistory() {
    const cfg = this._config;
    if (!this._hass || !cfg.time_entity) {
      return;
    }
    this._fetching = true;
    const ids = [...cfg.zones];
    if (cfg.score_entity) ids.push(cfg.score_entity);
    if (cfg.coverage_entity) ids.push(cfg.coverage_entity);
    ids.push(cfg.time_entity);
    const end = new Date();
    const start = new Date(end.getTime() - cfg.history_days * 86400000);
    let hist;
    try {
      hist = await this._hass.callWS({
        type: "history/history_during_period",
        start_time: start.toISOString(),
        end_time: end.toISOString(),
        entity_ids: ids,
        minimal_response: true,
        no_attributes: true,
      });
    } catch (e) {
      this._fetching = false;
      return;
    }
    // Each entry is {s: state, lu: last_updated_epoch}. Build per-entity arrays
    // and a "value effective at time t" lookup (last sample at or before t,
    // with a few seconds tolerance for the spread across one poll).
    const arr = (id) => (hist[id] || []).map((p) => ({ t: p.lu, v: p.s }));
    // Tolerance MUST stay below the firmware session-publish stagger (1.5s) so a
    // session's window never reaches into the next staggered session's rows. A
    // backlog publishes records 1.5s apart; within one record zones land ~1ms
    // before its time row, so 0.5s covers intra-record spread with wide margin.
    const valueAt = (a, t) => {
      let r = null;
      for (const p of a) {
        if (p.t <= t + 0.5) {
          r = p.v;
        } else {
          break;
        }
      }
      return r;
    };
    const zoneArrs = cfg.zones.map(arr);
    const scoreArr = cfg.score_entity ? arr(cfg.score_entity) : [];
    const covArr = cfg.coverage_entity ? arr(cfg.coverage_entity) : [];
    const timeArr = arr(cfg.time_entity);
    // no sample yet (an entity newer than the session) is unknown, not 0
    const num = (a, t) => {
      const v = valueAt(a, t);
      const x = v === null ? NaN : Number(v);
      return Number.isFinite(x) ? x : null;
    };
    const sessions = [];
    const seen = new Set();
    for (const p of timeArr) {
      if (p.v === "unknown" || p.v === "unavailable" || p.v === "") {
        continue;
      }
      // Full dedup by real session time (the last_session value). The node
      // re-publishes the same session on every reboot/poll, so one brushing
      // appears many times in entity history; timeArr is oldest-first, so the
      // first occurrence is the live catch (correct zones).
      if (seen.has(p.v)) {
        continue;
      }
      seen.add(p.v);
      sessions.push({
        t: p.v,
        lu: p.t,
        zones: zoneArrs.map((a) => num(a, p.t)),
        score: num(scoreArr, p.t),
        coverage: num(covArr, p.t),
      });
    }
    this._sessions = sessions;
    this._idx = sessions.length ? sessions.length - 1 : null;
    this._lastTime = this._liveStr(cfg.time_entity);
    this._fetching = false;
    this._lastSignature = null;
    this._render();
  }

  // The session currently shown: a history snapshot when one is selected,
  // otherwise the live entity states.
  _currentView() {
    const cfg = this._config;
    if (this._sessions && this._sessions.length && this._idx !== null) {
      const s = this._sessions[Math.min(this._idx, this._sessions.length - 1)];
      return {
        values: s.zones,
        score: s.score,
        coverage: s.coverage,
        time: s.t,
        idx: this._idx,
        count: this._sessions.length,
      };
    }
    return {
      values: cfg.zones.map((e) => this._liveNum(e)),
      score: this._liveNum(cfg.score_entity),
      coverage: this._liveNum(cfg.coverage_entity),
      time: this._liveStr(cfg.time_entity),
      idx: null,
      count: 0,
    };
  }

  _step(delta) {
    if (!this._sessions || !this._sessions.length) {
      return;
    }
    const cur = this._idx === null ? this._sessions.length - 1 : this._idx;
    this._idx = Math.max(0, Math.min(this._sessions.length - 1, cur + delta));
    this._lastSignature = null;
    this._render();
  }

  _seek(idx) {
    if (!this._sessions || !this._sessions.length) {
      return;
    }
    this._idx = Math.max(0, Math.min(this._sessions.length - 1, idx));
    this._lastSignature = null;
    this._render();
  }

  // Per-surface ratio in [0,1] or null when there is no usable value.
  _ratios(values) {
    const present = values.filter((v) => v !== null);
    if (present.length === 0) {
      return values.map(() => null);
    }
    const cfg = this._config;
    if (cfg.normalize === "max") {
      const max = Math.max(...present);
      return values.map((v) => (v === null ? null : max > 0 ? clamp01(v / max) : 0));
    }
    if (cfg.normalize === "absolute") {
      const t = cfg.target > 0 ? cfg.target : 1;
      return values.map((v) => (v === null ? null : clamp01(v / t)));
    }
    // share: compare each surface against an even split of the session total.
    const total = present.reduce((a, b) => a + b, 0);
    const even = total / values.length;
    return values.map((v) => (v === null ? null : even > 0 ? clamp01(v / even) : 0));
  }

  _zoneIndex(surface) {
    // Mirror swaps left and right (xor 4 flips L<->R within each jaw/surface);
    // the front zones 8-11 span the midline and stay.
    return this._config.mirror && surface.z < 8 ? surface.z ^ 4 : surface.z;
  }

  _fireMoreInfo(entityId) {
    if (!entityId) {
      return;
    }
    const ev = new Event("hass-more-info", { bubbles: true, composed: true });
    ev.detail = { entityId };
    this.dispatchEvent(ev);
  }

  _render() {
    if (!this._config) {
      return;
    }
    const cfg = this._config;
    const view = this._currentView();
    const values = view.values;
    const ratios = this._ratios(values);

    // Skip a full re-render if nothing visible changed.
    const signature = JSON.stringify({ values, m: cfg.mirror, n: cfg.normalize, i: view.idx, c: view.count });
    if (signature === this._lastSignature) {
      return;
    }
    this._lastSignature = signature;

    const paths = [];
    const badges = [];
    const labels = [];
    const twelve = values.length === 12;
    (twelve ? SURFACES_12 : SURFACES).forEach((s) => {
      const zi = this._zoneIndex(s);
      const val = values[zi];
      const ratio = ratios[zi];
      const color = ratioColor(ratio);
      const text = val === null ? "?" : String(val);
      const tip = `${s.label}: ${val === null ? "no data" : val}`;
      toothCellPaths(s).forEach((d) => {
        paths.push(
          `<path class="surface" d="${d}" fill="${color}" data-entity="${esc(cfg.zones[zi])}"><title>${esc(tip)}</title></path>`,
        );
      });
      // One value per surface on a dark badge so the digit stays readable: the
      // outer surface's outside the arch, the inner surface's inside it.
      const am = rad((s.a0 + s.a1) / 2);
      const lx = CX + s.labelR * Math.cos(am);
      const ly = s.cy + s.labelR * Math.sin(am);
      const r = text.length > 2 ? 13 : 11;
      badges.push(`<circle class="badge" cx="${lx.toFixed(1)}" cy="${ly.toFixed(1)}" r="${r}"></circle>`);
      labels.push(`<text class="val" x="${lx.toFixed(1)}" y="${ly.toFixed(1)}">${esc(text)}</text>`);
    });
    // Draw order: filled teeth, then the value badges, then the digits on top.
    const sectors = paths.join("") + badges.join("") + labels.join("");

    const lbl = (key, fallback) => esc(cfg.labels[key] || fallback);
    const sideY = (CY_UP + CY_LOW) / 2 + 4; // mouth-line height for L/R labels
    const leftX = cfg.mirror ? CX + 128 : CX - 128;
    const rightX = cfg.mirror ? CX - 128 : CX + 128;
    // The front badges of 12 zones sit where the arch names go, so those move
    // inside the arches, and the box grows to keep the lower front badge whole.
    const upperY = twelve ? CY_UP - 12 : 18;
    const lowerY = twelve ? CY_LOW + 20 : 292;
    const boxH = twelve ? 306 : 300;

    const summary = this._summary(view);
    const nav = this._nav(view);
    const headerTitle = cfg.title ? `<div class="title">${esc(cfg.title)}</div>` : "";

    this.shadowRoot.innerHTML = `
      <style>
        ha-card { padding: 12px 12px 8px; }
        .title { font-size: 1.1rem; font-weight: 500; margin-bottom: 4px; }
        .summary { color: var(--secondary-text-color); font-size: 0.85rem; margin-bottom: 4px; text-align: center; }
        svg { width: 100%; height: auto; display: block; }
        .surface { cursor: pointer; transition: opacity 0.15s; }
        .surface:hover { opacity: 0.82; }
        .badge { fill: rgba(0,0,0,0.62); pointer-events: none; }
        .val { fill: #fff; font-size: 14px; font-weight: 700; text-anchor: middle; dominant-baseline: central; pointer-events: none; }
        .axis { fill: var(--secondary-text-color); font-size: 12px; text-anchor: middle; }
        .legend { display: flex; align-items: center; gap: 6px; font-size: 0.78rem; color: var(--secondary-text-color); margin-top: 4px; }
        .bar { flex: 1; height: 8px; border-radius: 4px; background: linear-gradient(to right, hsl(0,60%,45%), hsl(60,60%,45%), hsl(120,60%,45%)); }
        .nav { display: flex; align-items: center; gap: 8px; margin: 2px 0 6px; }
        .nav button { border: none; background: var(--secondary-background-color); color: var(--primary-text-color); border-radius: 50%; width: 28px; height: 28px; font-size: 15px; cursor: pointer; line-height: 1; }
        .nav button:disabled { opacity: 0.4; cursor: default; }
        .nav input[type=range] { flex: 1; accent-color: var(--primary-color); }
        .nav .pos { font-size: 0.78rem; color: var(--secondary-text-color); min-width: 64px; text-align: right; white-space: nowrap; }
        .nav .live { color: var(--primary-color); font-weight: 600; }
      </style>
      <ha-card>
        ${headerTitle}
        ${summary}
        ${nav}
        <svg viewBox="0 0 320 ${boxH}" role="img" aria-label="brushing coverage map">
          <text class="axis" x="${CX}" y="${upperY}">${lbl("upper", "Upper")}</text>
          <text class="axis" x="${CX}" y="${lowerY}">${lbl("lower", "Lower")}</text>
          <text class="axis" x="${leftX}" y="${sideY}">${lbl("left", "L")}</text>
          <text class="axis" x="${rightX}" y="${sideY}">${lbl("right", "R")}</text>
          ${sectors}
        </svg>
        <div class="legend">
          <span>${lbl("missed", "missed")}</span>
          <span class="bar"></span>
          <span>${lbl("brushed", "brushed")}</span>
        </div>
      </ha-card>`;

    this.shadowRoot.querySelectorAll(".surface").forEach((el) => {
      el.addEventListener("click", () => this._fireMoreInfo(el.getAttribute("data-entity")));
    });
    const prev = this.shadowRoot.querySelector(".nav .prev");
    const next = this.shadowRoot.querySelector(".nav .next");
    const range = this.shadowRoot.querySelector(".nav input[type=range]");
    if (prev) prev.addEventListener("click", () => this._step(-1));
    if (next) next.addEventListener("click", () => this._step(1));
    if (range) range.addEventListener("input", (e) => this._seek(Number(e.target.value)));
  }

  // Prev/next arrows and a slider over past sessions; hidden until at least two
  // sessions are in history.
  _nav(view) {
    if (!view.count || view.count < 2) {
      return "";
    }
    const idx = view.idx === null ? view.count - 1 : view.idx;
    const atOldest = idx <= 0;
    const atNewest = idx >= view.count - 1;
    const pos = atNewest
      ? `<span class="live">${esc(this._config.labels.live || "latest")}</span>`
      : `${idx + 1} / ${view.count}`;
    return `
      <div class="nav">
        <button class="prev" ${atOldest ? "disabled" : ""} title="older">&#9664;</button>
        <input type="range" min="0" max="${view.count - 1}" value="${idx}" step="1">
        <button class="next" ${atNewest ? "disabled" : ""} title="newer">&#9654;</button>
        <span class="pos">${pos}</span>
      </div>`;
  }

  _summary(view) {
    const cfg = this._config;
    const parts = [];
    if (cfg.score_entity && view.score !== null) {
      parts.push(`${esc(cfg.labels.score || "Score")}: ${esc(view.score)}`);
    }
    if (cfg.coverage_entity && view.coverage !== null) {
      parts.push(`${esc(cfg.labels.coverage || "Coverage")}: ${esc(view.coverage)}%`);
    }
    if (cfg.time_entity && view.time) {
      parts.push(esc(view.time));
    }
    return parts.length ? `<div class="summary">${parts.join(" &middot; ")}</div>` : "";
  }
}

customElements.define("oclean-coverage-card", OcleanCoverageCard);

window.customCards = window.customCards || [];
window.customCards.push({
  type: "oclean-coverage-card",
  name: "Oclean Coverage Card",
  description: "Mouth map of per-surface brushing coverage from an Oclean session, with history navigation.",
});

// eslint-disable-next-line no-console
console.info(`%c oclean-coverage-card ${CARD_VERSION} `, "color:#fff;background:#039be5;border-radius:3px");
