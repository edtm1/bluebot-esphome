/* Bluebot kaart-card voor Home Assistant (HACS, categorie Dashboard).
 * Leest een sensor met de attributen map en track (base64, formaat van de Bluebot-firmware),
 * decodeert die in de browser en tekent kaart, track en robot op een canvas.
 * De decodeerfuncties staan los van het tekenen, zodat ze ook buiten Home Assistant bruikbaar zijn. */

const BLUEBOT_CARD_VERSION = "0.1.0";

function bbB64ToBytes(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/* Kaart: 9 bytes kop (breedte op 6, hoogte op 8), daarna RLE. Een byte met de bovenste twee bits
 * op 1 is een lengtebyte (6 bits, hoogste stuk eerst), gevolgd door de waarde. Elke rasterbyte
 * bevat 4 cellen van 2 bit, x%4 == 0 in de hoogste bits. Celwaarden: 0 onbekend, 1 obstakel, 2 schoon. */
function decodeMap(b64) {
  const d = bbB64ToBytes(b64);
  const w = d[6] || 100, h = d[8] || 100;
  const raw = new Uint8Array((w * h) >> 2);
  let o = 0;
  for (let i = 9; i < d.length && o < raw.length;) {
    if ((d[i] & 0xc0) === 0xc0) {
      let n = 0;
      while (i < d.length && (d[i] & 0xc0) === 0xc0) n = (n << 6) | (d[i++] & 0x3f);
      const v = d[i++];
      for (let k = 0; k < n && o < raw.length; k++) raw[o++] = v;
    } else {
      raw[o++] = d[i++];
    }
  }
  const cells = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) cells[i] = (raw[i >> 2] >> ((3 - (i & 3)) * 2)) & 3;
  return { w, h, cells };
}

/* Track: byte 0 = versie, byte 1 = clearArea, bytes 2-3 = aantal punten (LE), daarna (x, rij) per
 * hoekpunt. Rij 0 is boven, net als in de kaart. */
function decodeTrack(b64) {
  const d = bbB64ToBytes(b64);
  const n = d[2] | (d[3] << 8);
  const pts = [];
  for (let k = 0; k < n && 5 + 2 * k < d.length; k++) pts.push({ x: d[4 + 2 * k], y: d[5 + 2 * k] });
  return { version: d[0], clearArea: d[1], points: pts };
}

/* Het gebied dat getekend moet worden: alle bekende cellen en alle trackpunten, met marge. */
function mapBounds(map, track, margin) {
  let x0 = map.w, x1 = -1, y0 = map.h, y1 = -1;
  for (let r = 0; r < map.h; r++)
    for (let c = 0; c < map.w; c++)
      if (map.cells[r * map.w + c]) {
        if (c < x0) x0 = c; if (c > x1) x1 = c;
        if (r < y0) y0 = r; if (r > y1) y1 = r;
      }
  for (const p of track.points) {
    if (p.x < x0) x0 = p.x; if (p.x > x1) x1 = p.x;
    if (p.y < y0) y0 = p.y; if (p.y > y1) y1 = p.y;
  }
  if (x1 < 0) return null;
  return {
    x0: Math.max(0, x0 - margin), x1: Math.min(map.w - 1, x1 + margin),
    y0: Math.max(0, y0 - margin), y1: Math.min(map.h - 1, y1 + margin),
  };
}

if (typeof module !== "undefined") module.exports = { decodeMap, decodeTrack, mapBounds };

if (typeof customElements !== "undefined") {
  class BluebotMapCard extends HTMLElement {
    constructor() {
      super();
      this.attachShadow({ mode: "open" });
      this._key = "";
      this._data = null;
    }

    setConfig(config) {
      if (!config || !config.entity) throw new Error("Geef een entity op, bijvoorbeeld sensor.bluebot_kaart");
      this._config = Object.assign(
        {
          title: "Bluebot",
          show_toggles: true,
          show_track: true,
          show_clean: true,
          show_obstacles: true,
          max_cell: 14,
          margin: 2,
          track_color: "#D85A30",
          clean_color: "rgba(93, 202, 165, 0.55)",
        },
        config
      );
      this._show = {
        track: this._config.show_track,
        clean: this._config.show_clean,
        obs: this._config.show_obstacles,
      };
      this._build();
    }

    getCardSize() { return 6; }
    static getStubConfig() { return { entity: "sensor.bluebot_kaart" }; }

    _build() {
      const c = this._config;
      const toggle = (id, label, on) =>
        `<label><input type="checkbox" data-k="${id}" ${on ? "checked" : ""}>${label}</label>`;
      this.shadowRoot.innerHTML = `
        <style>
          ha-card { padding: 12px 16px 16px; }
          .hdr { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; flex-wrap: wrap; }
          .ttl { font-size: 16px; font-weight: 500; color: var(--primary-text-color); }
          .tg { display: flex; gap: 12px; font-size: 13px; color: var(--secondary-text-color); }
          .tg label { display: flex; align-items: center; gap: 4px; cursor: pointer; }
          .box { margin-top: 8px; text-align: center; }
          canvas { max-width: 100%; border-radius: 8px; background: var(--secondary-background-color); }
          .ftr { margin-top: 8px; font-size: 12px; color: var(--secondary-text-color); display: flex; gap: 12px; flex-wrap: wrap; }
          .msg { padding: 24px 0; text-align: center; color: var(--secondary-text-color); }
        </style>
        <ha-card>
          <div class="hdr">
            <span class="ttl">${c.title}</span>
            ${c.show_toggles ? `<span class="tg">${toggle("track", "Track", this._show.track)}${toggle("clean", "Schoon", this._show.clean)}${toggle("obs", "Obstakels", this._show.obs)}</span>` : ""}
          </div>
          <div class="box"><canvas></canvas><div class="msg" hidden></div></div>
          <div class="ftr"></div>
        </ha-card>`;
      this._canvas = this.shadowRoot.querySelector("canvas");
      this._msg = this.shadowRoot.querySelector(".msg");
      this._ftr = this.shadowRoot.querySelector(".ftr");
      this.shadowRoot.querySelectorAll("input[data-k]").forEach((el) =>
        el.addEventListener("change", () => { this._show[el.dataset.k] = el.checked; this._draw(); })
      );
      if (this._ro) this._ro.disconnect();
      this._ro = new ResizeObserver(() => this._draw());
      this._ro.observe(this.shadowRoot.querySelector("ha-card"));
    }

    set hass(hass) {
      this._hass = hass;
      const st = hass.states[this._config.entity];
      if (!st) return this._say(`Entiteit ${this._config.entity} niet gevonden`);
      const a = st.attributes || {};
      if (!a.map || !a.track) return this._say("Nog geen kaart ontvangen. Start een schoonmaakbeurt.");
      const key = a.map + "|" + a.track + "|" + a.x + "|" + a.y;
      if (key === this._key) return;
      this._key = key;
      try {
        this._data = {
          map: decodeMap(a.map),
          track: decodeTrack(a.track),
          x: a.x !== undefined ? Number(a.x) : -1,
          y: a.y !== undefined ? Number(a.y) : -1,
          area: a.area_m2, session: a.session, live: String(a.live) === "1", reason: a.reason,
          updated: st.last_updated,
        };
      } catch (e) {
        return this._say("Kaart kon niet gedecodeerd worden: " + e.message);
      }
      this._draw();
    }

    _say(text) {
      this._data = null; this._key = "";
      if (!this._canvas) return;
      this._canvas.hidden = true; this._msg.hidden = false; this._msg.textContent = text; this._ftr.textContent = "";
    }

    _draw() {
      const d = this._data;
      if (!d || !this._canvas) return;
      const { map, track } = d;
      const b = mapBounds(map, track, this._config.margin);
      if (!b) return this._say("De kaart is leeg");
      this._canvas.hidden = false; this._msg.hidden = true;
      const cols = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1;
      const avail = Math.max(120, (this.shadowRoot.querySelector("ha-card").clientWidth || 320) - 32);
      const cell = Math.max(3, Math.min(this._config.max_cell, Math.floor(avail / cols)));
      const dpr = window.devicePixelRatio || 1;
      const cv = this._canvas;
      cv.style.width = cols * cell + "px"; cv.style.height = rows * cell + "px";
      cv.width = Math.round(cols * cell * dpr); cv.height = Math.round(rows * cell * dpr);
      const g = cv.getContext("2d");
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, cols * cell, rows * cell);
      const css = getComputedStyle(this);
      const obsColor = css.getPropertyValue("--secondary-text-color").trim() || "#727272";
      const robotColor = css.getPropertyValue("--primary-color").trim() || "#03a9f4";
      for (let r = b.y0; r <= b.y1; r++)
        for (let c = b.x0; c <= b.x1; c++) {
          const v = map.cells[r * map.w + c];
          if (v === 2 && this._show.clean) { g.fillStyle = this._config.clean_color; g.fillRect((c - b.x0) * cell, (r - b.y0) * cell, cell, cell); }
          else if (v === 1 && this._show.obs) { g.fillStyle = obsColor; g.fillRect((c - b.x0) * cell, (r - b.y0) * cell, cell, cell); }
        }
      const px = (p) => (p.x - b.x0 + 0.5) * cell, py = (p) => (p.y - b.y0 + 0.5) * cell;
      if (this._show.track && track.points.length) {
        g.strokeStyle = this._config.track_color; g.lineWidth = Math.max(1.5, cell / 5);
        g.lineJoin = "round"; g.lineCap = "round"; g.beginPath();
        track.points.forEach((p, i) => (i ? g.lineTo(px(p), py(p)) : g.moveTo(px(p), py(p))));
        g.stroke();
      }
      const dot = (p, color) => {
        g.fillStyle = color; g.strokeStyle = "#fff"; g.lineWidth = 1.5;
        g.beginPath(); g.arc(px(p), py(p), Math.max(3, cell / 2.2), 0, 2 * Math.PI); g.fill(); g.stroke();
      };
      if (track.points.length) dot(track.points[0], "#639922");
      if (d.x >= 0 && d.y >= 0) dot({ x: d.x, y: map.h - 1 - d.y }, robotColor);
      const t = d.updated ? new Date(d.updated).toLocaleTimeString() : "";
      this._ftr.innerHTML =
        (d.area !== undefined ? `<span>${d.area} m²</span>` : "") +
        `<span>${track.points.length} hoekpunten</span>` +
        (d.live ? "<span>live</span>" : `<span>${d.reason || ""}</span>`) +
        (t ? `<span>bijgewerkt ${t}</span>` : "");
    }

    disconnectedCallback() { if (this._ro) this._ro.disconnect(); }
    connectedCallback() { if (this._config && this._ro) this._ro.observe(this.shadowRoot.querySelector("ha-card")); }
  }

  if (!customElements.get("bluebot-map-card")) customElements.define("bluebot-map-card", BluebotMapCard);
  window.customCards = window.customCards || [];
  window.customCards.push({
    type: "bluebot-map-card",
    name: "Bluebot kaart",
    description: "Plattegrond en gereden pad van de Bluebot 790T (ESPHome)",
  });
  console.info(`%c BLUEBOT-MAP-CARD %c ${BLUEBOT_CARD_VERSION} `, "color:white;background:#D85A30;font-weight:700", "color:#D85A30");
}
