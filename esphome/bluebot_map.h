#pragma once
// =====================================================================
// Bluebot 790T: frame-ontvangst, frame-inventaris en kaart/track.
//
// Nabouw van de firmware van de ESP8266 (Ghidra):
//   FUN_4023d420  positieframe -> raster en track
//   FUN_4023cfac  cel zetten (0 onbekend, 1 obstakel, 2 schoon)
//   FUN_4023cea0  clearArea (cellen met waarde 2, gedeeld door 25)
//   FUN_40253ec8  richting (8 richtingen, alleen op teken van dx en dy)
//   FUN_4023d0cc  kaart opschuiven bij een sprong in de positie
//   FUN_4023d790  RLE van het raster, FUN_4023d950 kop (9 bytes) + base64
//   FUN_4023d8dc  track = base64 van de trackbuffer
//
// Alles staat in namespace esphome::bluebot; onderaan staat een
// "using namespace", zodat de YAML-lambdas g_rx, g_inv en g_map direct kennen.
// =====================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "bluebot_helpers.h"

namespace esphome {
namespace bluebot {

static const char *const TAG_RX = "bluebot.rx";
static const char *const TAG_INV = "bluebot.frames";
static const char *const TAG_MAP = "bluebot.map";

// ---------------------------------------------------------------------
// Standaard base64 (met padding), zoals FUN_4023da64.
// ---------------------------------------------------------------------
inline std::string bluebot_b64(const uint8_t *d, size_t n) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t) d[i] << 16;
    if (i + 1 < n) v |= (uint32_t) d[i + 1] << 8;
    if (i + 2 < n) v |= d[i + 2];
    out += A[(v >> 18) & 63];
    out += A[(v >> 12) & 63];
    out += (i + 1 < n) ? A[(v >> 6) & 63] : '=';
    out += (i + 2 < n) ? A[v & 63] : '=';
  }
  return out;
}

// ---------------------------------------------------------------------
// Frame-assembler: zet een bytestroom om in complete, geldige frames.
// Een frame kan over meerdere leesbeurten verdeeld zijn; dat wordt hier opgevangen.
//   AA 55 A5 5A | chk (2) | type (2) | plen (2, LE) | 01 00 | payload
// ---------------------------------------------------------------------
class FrameRx {
 public:
  static constexpr size_t BUF = 512;
  static constexpr size_t MAX_PLEN = 400;

  uint32_t bytes_ = 0, frames_ = 0, bad_checksum_ = 0, dropped_partial_ = 0;

  void feed(const uint8_t *d, size_t n) {
    uint32_t now = millis();
    // Een half frame dat 300 ms niet afkomt, is een afgebroken frame.
    if (len_ > 0 && now - last_ms_ > 300) {
      dropped_partial_++;
      len_ = 0;
    }
    last_ms_ = now;
    bytes_ += n;
    for (size_t k = 0; k < n; k++) {
      buf_[len_++] = d[k];
      if (len_ == BUF) parse();
    }
    parse();
  }

  bool pop(std::vector<uint8_t> &out) {
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

 private:
  uint8_t buf_[BUF];
  size_t len_ = 0;
  uint32_t last_ms_ = 0;
  std::deque<std::vector<uint8_t>> q_;

  void parse() {
    size_t i = 0;
    while (true) {
      while (i + 4 <= len_ &&
             !(buf_[i] == 0xAA && buf_[i + 1] == 0x55 && buf_[i + 2] == 0xA5 && buf_[i + 3] == 0x5A))
        i++;
      if (i + 12 > len_) break;
      size_t plen = buf_[i + 8] | (buf_[i + 9] << 8);
      if (plen > MAX_PLEN) {
        i++;
        continue;
      }
      size_t flen = 12 + plen;
      if (i + flen > len_) break;  // frame is nog niet compleet
      uint16_t expected = bluebot_checksum(buf_ + i, flen);
      uint16_t got = buf_[i + 4] | (buf_[i + 5] << 8);
      if (expected != got) {
        bad_checksum_++;
        ESP_LOGW(TAG_RX, "Checksum fout: verwacht %04X, ontvangen %04X (%s)", expected, got,
                 bluebot_hex(buf_ + i, std::min(flen, (size_t) 24)).c_str());
        i++;  // misschien was dit geen echte frame-start
        continue;
      }
      q_.emplace_back(buf_ + i, buf_ + i + flen);
      frames_++;
      i += flen;
    }
    if (i > 0) {
      memmove(buf_, buf_ + i, len_ - i);
      len_ -= i;
    }
  }
};

// ---------------------------------------------------------------------
// Frame-inventaris: teller per (type, lengte), met de laatste payload.
// ---------------------------------------------------------------------
class FrameInventory {
 public:
  struct Entry {
    uint32_t count = 0, first_ms = 0, last_ms = 0;
    std::string last_hex;
  };

  std::string last_status_hex;

  // true als dit (type, lengte) nog niet eerder is gezien.
  bool record(const uint8_t *f, size_t flen) {
    size_t plen = flen - 12;
    uint32_t key = ((uint32_t) f[6] << 24) | ((uint32_t) f[7] << 16) | (uint32_t) plen;
    Entry &e = entries_[key];
    bool is_new = (e.count == 0);
    uint32_t now = millis();
    if (is_new) e.first_ms = now;
    e.count++;
    e.last_ms = now;
    e.last_hex = bluebot_hex(f + 12, std::min(plen, (size_t) 24)) + (plen > 24 ? " ..." : "");
    return is_new;
  }

  void dump() const {
    ESP_LOGI(TAG_INV, "--- Frame-inventaris: %u soorten, tijd nu %u s ---", (unsigned) entries_.size(),
             (unsigned) (millis() / 1000));
    for (const auto &kv : entries_) {
      uint32_t k = kv.first;
      const Entry &e = kv.second;
      ESP_LOGI(TAG_INV, "type %02X %02X  len %3u  aantal %6u  eerst %6us  laatst %6us  payload: %s",
               (unsigned) (k >> 24), (unsigned) ((k >> 16) & 0xFF), (unsigned) (k & 0xFFFF),
               (unsigned) e.count, (unsigned) (e.first_ms / 1000), (unsigned) (e.last_ms / 1000),
               e.last_hex.c_str());
    }
  }

  void reset() {
    entries_.clear();
    ESP_LOGI(TAG_INV, "Frame-inventaris gewist");
  }

  // true als de payload van een statusframe verschilt van het vorige.
  bool status_changed(const uint8_t *p, size_t plen) {
    std::string h = bluebot_hex(p, plen);
    if (h == last_status_hex) return false;
    last_status_hex = h;
    return true;
  }

 private:
  std::map<uint32_t, Entry> entries_;
};

// ---------------------------------------------------------------------
// Kaart en track
// ---------------------------------------------------------------------
class BluebotMap {
 public:
  static constexpr int W = 100;
  static constexpr int H = 100;
  static constexpr size_t GRID_BYTES = (size_t) W * H / 4;
  static constexpr int MAX_PTS = 799;

  struct Snapshot {
    bool valid = false;
    std::string reason, map_b64, track_b64, summary, area_m2_s;
    uint8_t area = 0;
    uint16_t points = 0;
    uint32_t clean_cells = 0, elapsed_s = 0, seq = 0;
    uint8_t session = 0;
    int x = -1, y = -1;  // laatste positie (x = kolom, y = ruwe y uit het positieframe)
    bool live = false;
  };

  // Publieke staat (handig voor tests en logging).
  uint8_t grid_[GRID_BYTES] = {0};  // 2 bit per cel, x%4 == 0 in de hoogste 2 bit
  uint8_t tx_[800] = {0}, ty_[800] = {0};  // trackpunten (x, 99 - y)
  uint16_t n_ = 0;
  uint8_t last_dir_ = 0;
  int8_t last_x_ = 0, last_y_ = 0;
  int cur_x_ = -1, cur_y_ = -1;
  uint32_t pos_frames_ = 0, clears_ = 0, shifts_ = 0;

  // ----- cellen -----
  static int idx(int x, int y) { return (H - 1 - y) * W + x; }

  static uint8_t get_in(const uint8_t *g, int x, int y) {
    if (x < 0 || x >= W || y < 0 || y >= H) return 0;
    int i = idx(x, y);
    return (g[i >> 2] >> ((3 - (x & 3)) * 2)) & 3;
  }

  static void set_in(uint8_t *g, int x, int y, uint8_t v) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    int i = idx(x, y);
    int sh = (3 - (x & 3)) * 2;
    g[i >> 2] = (uint8_t) ((g[i >> 2] & ~(3 << sh)) | ((v & 3) << sh));
  }

  // FUN_4023cfac: invoer & 3; 0 -> 0, 3 -> 1 (obstakel), 1 en 2 -> 2 (schoon).
  // Het wist de cel altijd eerst, ook als de nieuwe waarde 0 is.
  void set_fw(int x, int y, uint8_t in) {
    in &= 3;
    set_in(grid_, x, y, in == 0 ? 0 : (in == 3 ? 1 : 2));
  }

  // ----- positieframe (payload van 4 bytes: p[0] = y, p[1] = x, p[2], p[3] = buurcellen) -----
  void on_position(const uint8_t *p, size_t plen) {
    if (plen < 3) {
      ESP_LOGW(TAG_MAP, "Positieframe te kort: %u bytes", (unsigned) plen);
      return;
    }
    const int y = p[0], x = p[1];
    const uint8_t p2 = p[2], p3 = plen > 3 ? p[3] : 0;
    changed_ = true;
    pos_frames_++;

    // Herijking (firmware: eerst, voor het zetten van de cellen).
    int dx = 0, dy = 0;
    int lx = last_x_, ly = last_y_;
    if (lx < 10 && x > 20) dx = (x + 1) - lx;
    if (lx >= W - 9 && x < W - 20) dx = (x - 1) - lx;
    if (ly < 10 && y > 20) dy = y - ly;
    if (ly >= W - 9 && y < W - 20) dy = y - ly;  // de firmware gebruikt de breedte voor beide
    if (dx != 0 || dy != 0) {
      if (n_ > 0) {
        ESP_LOGW(TAG_MAP, "SPRONG in positie: (%d,%d) -> (%d,%d); kaart schuift %d,%d", lx, ly, x, y, dx, dy);
        shift(dx, dy);
      } else {
        ESP_LOGD(TAG_MAP, "Sprong op lege kaart genegeerd (%d,%d) -> (%d,%d)", lx, ly, x, y);
      }
    } else if (n_ > 0 && (std::abs(x - lx) > 3 || std::abs(y - ly) > 3)) {
      // Grote stap die de firmware NIET als herijking ziet. Is dit een herkalibratie
      // die de firmware zelf ook mist, of gewoon een gemist frame? De log zal het uitwijzen.
      ESP_LOGW(TAG_MAP, "GROTE STAP zonder herijking: (%d,%d) -> (%d,%d)", lx, ly, x, y);
    }

    // Cellen (volgorde zoals in de firmware).
    set_fw(x, y, 2);
    cur_x_ = x;
    cur_y_ = y;
    set_fw(x + 1, y, p2);
    set_fw(x, y - 1, p3 >> 4);
    set_fw(x, y + 1, p2 >> 4);
    set_fw(x + 1, y - 1, p3 >> 6);
    set_fw(x + 1, y + 1, p2 >> 2);
    set_fw(x - 1, y - 1, p3 >> 2);
    set_fw(x - 1, y + 1, p2 >> 6);

    // Track: alleen hoekpunten.
    const uint8_t ty = (uint8_t) (99 - y);
    if (n_ == 0) {
      tx_[0] = (uint8_t) x;
      ty_[0] = ty;
      n_ = 1;
      last_dir_ = 0;
      last_x_ = (int8_t) x;
      last_y_ = (int8_t) y;
      return;
    }
    if (x == last_x_ && y == last_y_) return;
    uint8_t dir = direction(x, y, last_x_, last_y_);
    if (dir == last_dir_) {
      tx_[n_ - 1] = (uint8_t) x;  // rechte lijn: laatste punt opschuiven
      ty_[n_ - 1] = ty;
    } else {
      uint16_t i = n_;
      if (n_ < MAX_PTS) n_++;
      tx_[i] = (uint8_t) x;
      ty_[i] = ty;
      last_dir_ = dir;
    }
    last_x_ = (int8_t) x;
    last_y_ = (int8_t) y;
  }

  // FUN_40253ec8: richting uit de tekens van dx en dy. 0 = geen beweging.
  static uint8_t direction(int x, int y, int lx, int ly) {
    int sx = (x > lx) - (x < lx), sy = (y > ly) - (y < ly);
    static const uint8_t t[3][3] = {{5, 1, 7}, {2, 0, 4}, {6, 3, 8}};  // [sx+1][sy+1]
    return t[sx + 1][sy + 1];
  }

  // FUN_4023d0cc: kaart en track verschuiven.
  void shift(int dx, int dy) {
    std::vector<uint8_t> tmp(GRID_BYTES, 0);
    for (int yy = 0; yy < H; yy++)
      for (int xx = 0; xx < W; xx++) {
        uint8_t v = get_in(grid_, xx, yy);
        if (v) set_in(tmp.data(), xx + dx, yy + dy, v);
      }
    memcpy(grid_, tmp.data(), GRID_BYTES);
    for (int i = 0; i < n_; i++) {
      tx_[i] = (uint8_t) (tx_[i] + dx);
      ty_[i] = (uint8_t) (ty_[i] - dy);
    }
    shifts_++;
  }

  // ----- afgeleide waarden -----
  uint32_t clean_cells() const {
    uint32_t n = 0;
    for (size_t i = 0; i < GRID_BYTES; i++) {
      uint8_t b = grid_[i];
      n += ((b & 0xC0) == 0x80) + ((b & 0x30) == 0x20) + ((b & 0x0C) == 0x08) + ((b & 0x03) == 0x02);
    }
    return n;
  }
  uint8_t clear_area() const { return (uint8_t) std::min<uint32_t>(clean_cells() / 25, 255); }
  // Oppervlak in m2 met decimalen. AANNAME: 25 cellen = 1 m2 (cel van 20 x 20 cm), afgeleid van de deling
  // door 25 in FUN_4023cea0. Controleer met een gemeten afstand (cellen in een rechte gang x 0,2 m).
  // Let op: het telt cellen die de robot als vrij meldt, dus ook de buurcellen naast zijn pad.
  float area_m2() const { return clean_cells() / 25.0f; }
  uint16_t points() const { return n_; }
  bool has_data() const { return n_ > 0; }

  // ----- codering (identiek aan het bericht van de firmware) -----
  static void rle(const uint8_t *in, size_t n, std::vector<uint8_t> &out) {
    for (size_t i = 0; i < n;) {
      size_t j = i + 1;
      while (j < n && in[j] == in[i]) j++;
      size_t run = j - i;
      if (run >= 2) {
        if (run > 0xFFF) out.push_back(0xC0 | ((run >> 12) & 0x3F));
        if (run >= 64) out.push_back(0xC0 | ((run >> 6) & 0x3F));
        out.push_back(0xC0 | (run & 0x3F));
      }
      out.push_back(in[i]);
      i = j;
    }
  }

  std::string map_b64() const {
    std::vector<uint8_t> raw = {0, 0, 0, 0, 0, 0, 100, 0, 100};
    rle(grid_, GRID_BYTES, raw);
    return bluebot_b64(raw.data(), raw.size());
  }

  std::string track_b64() const {
    std::vector<uint8_t> raw = {0x01, clear_area(), (uint8_t) (n_ & 0xFF), (uint8_t) (n_ >> 8)};
    for (int i = 0; i < n_; i++) {
      raw.push_back(tx_[i]);
      raw.push_back(ty_[i]);
    }
    return bluebot_b64(raw.data(), raw.size());
  }

  // ----- logging -----
  void log_ascii(const char *label) const {
    int c0 = W, c1 = -1, r0 = H, r1 = -1;
    auto grow = [&](int c, int r) {
      c0 = std::min(c0, c); c1 = std::max(c1, c);
      r0 = std::min(r0, r); r1 = std::max(r1, r);
    };
    for (int r = 0; r < H; r++)
      for (int c = 0; c < W; c++)
        if (get_in(grid_, c, H - 1 - r)) grow(c, r);
    for (int i = 0; i < n_; i++) grow(tx_[i] % W, ty_[i] % H);
    if (c1 < 0) {
      ESP_LOGI(TAG_MAP, "KAART (%s): leeg", label);
      return;
    }
    ESP_LOGI(TAG_MAP, "KAART (%s): kolom %d-%d, rij %d-%d, rij 0 = boven. . onbekend  # obstakel  o schoon  S start  T trackhoek  R robot",
             label, c0, c1, r0, r1);
    for (int r = r0; r <= r1; r++) {
      std::string row;
      for (int c = c0; c <= c1; c++) row += ".#o?"[get_in(grid_, c, H - 1 - r)];
      for (int i = 0; i < n_; i++)
        if (ty_[i] == r && tx_[i] >= c0 && tx_[i] <= c1) row[tx_[i] - c0] = (i == 0) ? 'S' : 'T';
      if (cur_x_ >= c0 && cur_x_ <= c1 && cur_y_ >= 0 && (H - 1 - cur_y_) == r) row[cur_x_ - c0] = 'R';
      ESP_LOGI(TAG_MAP, "%02d|%s", r, row.c_str());
    }
  }

  static void log_chunks(const char *name, const std::string &s) {
    const size_t CH = 180;
    size_t total = (s.size() + CH - 1) / CH;
    for (size_t k = 0; k < total; k++)
      ESP_LOGI(TAG_MAP, "MAPB64 %s %u/%u %s", name, (unsigned) (k + 1), (unsigned) total,
               s.substr(k * CH, CH).c_str());
  }

  // ----- snapshot: vastleggen, loggen en klaarzetten voor publicatie in Home Assistant -----
  // quiet = geen ASCII en base64 in de log (voor de live-updates).
  void take_snapshot(const char *reason, bool quiet = false) {
    snap_.valid = true;
    snap_.live = quiet;
    snap_.seq = ++seq_;
    snap_.session = resets_;
    snap_.x = cur_x_;
    snap_.y = cur_y_;
    snap_.reason = reason;
    snap_.map_b64 = map_b64();
    snap_.track_b64 = track_b64();
    snap_.area = clear_area();
    snap_.points = n_;
    snap_.clean_cells = clean_cells();
    {
      char a[16];
      snprintf(a, sizeof(a), "%.2f", area_m2());
      snap_.area_m2_s = a;
    }
    snap_.elapsed_s = (millis() - reset_ms_) / 1000;
    snap_pos_frames_ = pos_frames_;
    char buf[200];
    snprintf(buf, sizeof(buf), "%s: %s m2 (clearArea %u), %u cellen, %u trackpunten, %u positieframes, %u s sinds reset", reason,
             snap_.area_m2_s.c_str(), (unsigned) snap_.area, (unsigned) snap_.clean_cells, (unsigned) snap_.points,
             (unsigned) pos_frames_, (unsigned) snap_.elapsed_s);
    snap_.summary = buf;
    if (!quiet) {
      ESP_LOGI(TAG_MAP, "SNAPSHOT %s", buf);
      log_ascii(reason);
      log_chunks("map", snap_.map_b64);
      log_chunks("track", snap_.track_b64);
    }
    publish_pending_ = true;
    last_snap_ms_ = millis();
  }

  const Snapshot &snap() const { return snap_; }

  // ----- wissen (eerst een snapshot, zodat de kaart niet verloren gaat) -----
  void clear(const char *reason) {
    if (has_data() && pos_frames_ != snap_pos_frames_) {
      take_snapshot(reason);
    } else {
      ESP_LOGI(TAG_MAP, "Kaart wissen (%s): %s", reason,
               has_data() ? "er is al een snapshot van deze kaart" : "kaart was al leeg");
    }
    // FUN_4023d674: raster en track leeg, teller 0, richting 0.
    // last_x_ en last_y_ blijven staan, zoals in de firmware.
    memset(grid_, 0, GRID_BYTES);
    memset(tx_, 0, sizeof(tx_));
    memset(ty_, 0, sizeof(ty_));
    n_ = 0;
    last_dir_ = 0;
    cur_x_ = cur_y_ = -1;
    clears_++;
    changed_ = true;
  }

  // ----- D7 07: kaart-reset van de robot (FUN_4023eb24) -----
  // Leeg frame, meestal twee keer vlak na elkaar zodra de robot gaat rijden. De firmware werkt het
  // tijdstip in BEIDE takken bij, dus een derde frame vlak na de tweede wordt ook genegeerd.
  // Bij een geldige reset: teller (+0x61) omhoog, clearTime (+100) op 0 en FUN_4023d674 (kaart wissen).
  // Drempel uit de listing van FUN_4023eb24: `bltu 0x1387, verschil` = geldig als verschil > 4999.
  // De tijdbron FUN_4023c704 is vermoedelijk in ms (niet bevestigd), dus 5 s tussen twee resets.
  static constexpr uint32_t RESET_MIN_GAP_MS = 5000;
  uint8_t resets_ = 0;

  bool on_reset_frame() {
    uint32_t now = millis();
    uint32_t gap = now - last_reset_frame_ms_;
    bool first = !reset_frame_seen_;
    reset_frame_seen_ = true;
    last_reset_frame_ms_ = now;
    if (!first && gap < RESET_MIN_GAP_MS) {
      ESP_LOGI(TAG_MAP, "D7 07 genegeerd, %u ms na het vorige (firmware: map reset too short)", (unsigned) gap);
      return false;
    }
    if (first)
      ESP_LOGI(TAG_MAP, "D7 07 geaccepteerd (eerste sinds het opstarten): kaart reset");
    else
      ESP_LOGI(TAG_MAP, "D7 07 geaccepteerd (%u ms na het vorige): kaart reset", (unsigned) gap);
    resets_++;
    clear("D7 07: robot gaat rijden");
    reset_ms_ = now;
    return true;
  }

  // ----- sessie-overgangen uit het statusframe (p[0]: 1 = schoonmaken) -----
  // 0 = niets, 1 = beurt begint, 2 = beurt is afgelopen
  int on_status(uint8_t p0) {
    uint8_t prev = prev_p0_;
    prev_p0_ = p0;
    cleaning_ = (p0 == 1);
    if (prev == 0xFF || prev == p0) return 0;
    if (p0 == 1) return 1;
    if (prev == 1 && millis() - last_snap_ms_ > 5000) return 2;
    return 0;
  }

  // ----- hulpjes voor de YAML-lambda -----
  bool take_publish_pending() {
    bool r = publish_pending_;
    publish_pending_ = false;
    return r;
  }
  // Live-updates: tijdens het schoonmaken hoogstens elke LIVE_MS een stille snapshot als er nieuwe
  // positieframes zijn. De snapshot wordt daarna via take_publish_pending() gepubliceerd.
  static constexpr uint32_t LIVE_MS = 5000;
  bool live_due() {
    uint32_t now = millis();
    if (!cleaning_ || pos_frames_ == live_pos_frames_ || now - last_live_ms_ < LIVE_MS) return false;
    live_pos_frames_ = pos_frames_;
    last_live_ms_ = now;
    take_snapshot("live", true);
    return true;
  }
  // true hoogstens elke 2 s, en alleen als de kaart veranderd is
  bool ui_due() {
    uint32_t now = millis();
    if (!changed_ || now - last_ui_ms_ < 2000) return false;
    changed_ = false;
    last_ui_ms_ = now;
    return true;
  }

 private:
  Snapshot snap_;
  bool changed_ = true, publish_pending_ = false;
  uint8_t prev_p0_ = 0xFF;
  uint32_t last_ui_ms_ = 0, last_snap_ms_ = 0, snap_pos_frames_ = 0;
  uint32_t reset_ms_ = 0, last_reset_frame_ms_ = 0, seq_ = 0, live_pos_frames_ = 0, last_live_ms_ = 0;
  bool cleaning_ = false;
  bool reset_frame_seen_ = false;
};

inline FrameRx g_rx;
inline FrameInventory g_inv;
inline BluebotMap g_map;

}  // namespace bluebot
}  // namespace esphome

using namespace esphome::bluebot;
