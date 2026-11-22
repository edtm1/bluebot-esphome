#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>

// =====================================================================
// Hex-hulpfuncties, gebruikt door het testpaneel (TX Type / TX Payload)
// en door de UART-logging.
// =====================================================================

inline std::vector<uint8_t> bluebot_parse_hex(const std::string &s) {
  std::string digits_only;
  for (char c : s)
    if (isxdigit((unsigned char) c)) digits_only += c;

  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < digits_only.size(); i += 2)
    out.push_back((uint8_t) strtol(digits_only.substr(i, 2).c_str(), nullptr, 16));
  return out;
}

inline std::string bluebot_hex(const uint8_t *data, size_t len) {
  std::string s;
  char byte_str[4];
  for (size_t k = 0; k < len; k++) {
    snprintf(byte_str, sizeof(byte_str), k ? " %02X" : "%02X", data[k]);
    s += byte_str;
  }
  return s;
}

// =====================================================================
// Checksum: (0 - som van alle bytes behalve index 4 en 5) & 0xFFFF
// Bevestigd via de firmware (FUN_00053064) en alle logs.
// =====================================================================

inline uint16_t bluebot_checksum(const uint8_t *frame, size_t frame_len) {
  uint32_t sum = 0;
  for (size_t k = 0; k < frame_len; k++)
    if (k != 4 && k != 5) sum += frame[k];
  return (uint16_t) ((0x10000 - sum) & 0xFFFF);
}

// =====================================================================
// Statusframe-decodering (payload van een plen=0x0C frame, p[0]..p[10])
// =====================================================================

// p[2]: hoofdmodus. Alleen de twee waarden die daadwerkelijk getest en
// bevestigd zijn in de logs. De oorspronkelijke documentatie noemde ook
// 0x01 = DOCK en 0x03 = STOP, maar testen wees uit dat p[2] == 0x01
// "gestopt" betekent, niet "gedockt" (zie p[0] voor de dockstatus).
// Andere waarden dan 0x01/0x02 zijn niet bevestigd en geven "Unknown".
inline std::string bluebot_decode_hoofdmodus(uint8_t p2) {
  switch (p2) {
    case 0x01: return "Gestopt";
    case 0x02: return "Schoonmaken";
    default:   return "Unknown";
  }
}

// p[1]: submodus, alleen betekenisvol zolang p[0] == 1 (schoonmaken).
// 0x07 en 0x08 zijn in de firmware gevonden maar nog niet geïdentificeerd,
// en krijgen daarom bewust geen naam.
inline std::string bluebot_decode_submodus(uint8_t p1) {
  switch (p1) {
    case 0x00: return "Stand-by";
    case 0x02: return "ZigZag";
    case 0x03: return "Edge";
    case 0x04: return "Area";
    case 0x05: return "Auto";
    case 0x06: return "Intensieve modus";
    default:   return "Unknown";
  }
}

// p[5]: zuigmonddetectie.
inline std::string bluebot_decode_nozzle(uint8_t p5) {
  switch (p5) {
    case 0x01: return "Zuigmond met borstel";
    case 0x02: return "Zuigmond zonder borstel";
    default:   return "Unknown";
  }
}

// p[0]: algemene robotstatus, voor de Home Assistant-weergave.
inline std::string bluebot_decode_general_status(uint8_t p0) {
  switch (p0) {
    case 1: return "Schoonmaken";
    case 2: return "Idle";
    case 3: return "Terugkeren naar dock";
    case 4: return "Docking";
    case 5: return "Gedockt, opladen";
    case 6: return "Opgeladen, stand-by";
    case 7: return "Fout";
    default: return "Idle";
  }
}

// p[8], p[9], p[10]: foutregisters (bitmaskers).
// "???" = Gereserveerd / Nog niet getest in de eigen documentatie;
// er zijn hier bewust geen namen verzonnen voor die bits.
inline std::string bluebot_decode_errors(uint8_t p8, uint8_t p9, uint8_t p10) {
  struct ErrorBit {
    uint8_t mask;
    uint8_t register_index;  // 8, 9 of 10
    const char *name;
  };

  static const ErrorBit bits[] = {
    // Register P8 - behuizingsensoren & motor-overbelasting
    {0x80, 8,  "E1  - Stofopvangreservoir niet geinstalleerd"},
    {0x40, 8,  "??? - Gereserveerd (mogelijk E3 of E45)"},
    {0x20, 8,  "E2  - Netschakelaar uit tijdens het laden"},
    {0x10, 8,  "??? - Gereserveerd (mogelijk E3 of E45)"},
    {0x08, 8,  "E44 - Rechter zijborstel overbelast"},
    {0x04, 8,  "E43 - Linker zijborstel overbelast"},
    {0x02, 8,  "E42 - Rechterwiel overbelast"},
    {0x01, 8,  "E41 - Linkerwiel overbelast"},
    // Register P9 - wielophanging, bumpers & hoofdrolborstel
    {0x80, 9,  "E46 - Rolborstel overbelast"},
    {0x40, 9,  "??? - Gereserveerd (mogelijk E33)"},
    {0x20, 9,  "??? - Gereserveerd (mogelijk E33)"},
    {0x10, 9,  "??? - Gereserveerd (mogelijk E23)"},
    {0x08, 9,  "E22 - Rechterwiel niet op de grond"},
    {0x04, 9,  "E21 - Linkerwiel niet op de grond"},
    {0x02, 9,  "E32 - Rechterbumper vooraan klem of geblokkeerd"},
    {0x01, 9,  "E31 - Linkerbumper vooraan klem of geblokkeerd"},
    // Register P10 - trapdetectie & afstandssensoren
    {0x04, 10, "E12 - Rechter trapdetectie sensorprobleem"},
    {0x02, 10, "E13 - Voorste trapdetectie sensorprobleem"},
    {0x01, 10, "E11 - Linker trapdetectie sensorprobleem"},
    // Bits 0x08/0x10/0x20/0x40/0x80 van P10: Gereserveerd / Nog niet
    // getest, bewust weggelaten.
  };

  uint8_t registers[11] = {0};
  registers[8] = p8;
  registers[9] = p9;
  registers[10] = p10;

  std::string result;
  for (const auto &bit : bits) {
    if (registers[bit.register_index] & bit.mask) {
      if (!result.empty()) result += "; ";
      result += bit.name;
    }
  }
  return result.empty() ? "Geen actieve fout" : result;
}

// =====================================================================
// Frames bouwen (één plek voor alle commando's) en klokframe D4 07
//   AA 55 A5 5A | chk (2) | type (2) | plen (2, little-endian) | 01 00 | payload
// =====================================================================

inline std::vector<uint8_t> bluebot_build_frame(uint8_t type_lo, uint8_t type_hi,
                                                const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> f = {0xAA, 0x55, 0xA5, 0x5A, 0, 0, type_lo, type_hi,
                            (uint8_t) payload.size(), 0x00, 0x01, 0x00};
  f.insert(f.end(), payload.begin(), payload.end());
  uint16_t cs = bluebot_checksum(f.data(), f.size());
  f[4] = cs & 0xFF;
  f[5] = cs >> 8;
  return f;
}

// Bouwt een frame uit hex-tekst (type = 2 bytes, payload mag leeg zijn).
// Geeft false terug als het type geen 2 bytes is.
inline bool bluebot_frame_from_hex(const std::string &type_hex, const std::string &payload_hex,
                                   std::vector<uint8_t> &out) {
  auto type = bluebot_parse_hex(type_hex);
  if (type.size() != 2) {
    out.clear();
    return false;
  }
  out = bluebot_build_frame(type[0], type[1], bluebot_parse_hex(payload_hex));
  return true;
}

// Weekdag volgens de app en de firmware: 0 = zondag ... 6 = zaterdag.
inline int bluebot_weekday(int year, int month, int day) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year - (month < 3 ? 1 : 0);
  return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

// Payload van het klokframe (type D4 07, 7 bytes):
//   [jaar-1900] [seconde=00] [minuut] [uur] [weekdag] [dag] [maand]
// De firmware vangt een foute klokstring niet af, dus eerst zelf controleren.
// Geeft false terug (en laat `out` leeg) bij een ongeldige waarde.
inline bool bluebot_clock_payload(int year, int month, int day, int hour, int minute,
                                  std::vector<uint8_t> &out) {
  out.clear();
  if (year < 2000 || year > 2155 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59)
    return false;
  out = {
      (uint8_t) ((year - 1900) & 0xFF),
      0x00,  // seconde, altijd 00 (zoals de app)
      (uint8_t) minute,
      (uint8_t) hour,
      (uint8_t) bluebot_weekday(year, month, day),
      (uint8_t) day,
      (uint8_t) month,
  };
  return true;
}

// =====================================================================
// Zuigkracht (transitCmd 110) en robotgeluid (transitCmd 123/125)
// =====================================================================

// Zuigkracht: E209 / 03 00 XX 00 00 00 00 00, XX = 01/02/03.
// Werkt alleen als de robot stilstaat (p[0] == 2). In het statusframe komt de
// waarde terug in p[2], maar alleen zolang p[0] == 2.
// AANNAME: 01 = Uit, 02 = Zacht, 03 = Hard. Klopt de volgorde niet, pas dan alleen
// deze twee functies aan.
inline std::string bluebot_decode_fan(uint8_t p2) {
  switch (p2) {
    case 0x01: return "Uit";
    case 0x02: return "Zacht";
    case 0x03: return "Hard";
    default:   return "Unknown";
  }
}

// Geeft de payload (hex) voor een zuigkrachtnaam, of een lege string als de naam onbekend is.
inline std::string bluebot_fan_payload(const std::string &name) {
  if (name == "Uit")   return "0300010000000000";
  if (name == "Zacht") return "0300020000000000";
  if (name == "Hard")  return "0300030000000000";
  return "";
}

// Robotgeluid ("voice"): E209 / 04 00 00 00 00 XX 00 00, XX = 02 aan, 01 uit.
// Status in het statusframe: p[7] (02 = aan, 01 = uit).
inline std::string bluebot_voice_payload(bool on) {
  return on ? "0400000000020000" : "0400000000010000";
}

// =====================================================================
// Planner (transitCmd 113/115/117/149)
// Opvragen: D2 07 met lege payload (antwoord: frame met 14 payload-bytes).
// Schrijven en wissen: D1 07 met 7 payload-bytes.
// =====================================================================

// Dagmasker: 0 = zondag (0x80), 1 = maandag (0x02) ... 6 = zaterdag (0x40).
inline uint8_t bluebot_day_mask(int day) {
  return day == 0 ? 0x80 : (uint8_t) (1 << day);
}

// Dag of dagengroep uit de select naar een masker. Geeft 0 bij een onbekende naam.
inline uint8_t bluebot_planner_days_from_name(const std::string &name) {
  if (name == "Maandag")   return bluebot_day_mask(1);
  if (name == "Dinsdag")   return bluebot_day_mask(2);
  if (name == "Woensdag")  return bluebot_day_mask(3);
  if (name == "Donderdag") return bluebot_day_mask(4);
  if (name == "Vrijdag")   return bluebot_day_mask(5);
  if (name == "Zaterdag")  return bluebot_day_mask(6);
  if (name == "Zondag")    return bluebot_day_mask(0);
  if (name == "Werkdagen (ma-vr)")
    return bluebot_day_mask(1) | bluebot_day_mask(2) | bluebot_day_mask(3) |
           bluebot_day_mask(4) | bluebot_day_mask(5);
  if (name == "Weekend")   return bluebot_day_mask(6) | bluebot_day_mask(0);
  if (name == "Alle dagen") return 0xFE;
  return 0;
}

// Instellen (D1 07), payload:
//   [jaar-1900] [maand] [dag] [uur] [minuut] [00] [dagmasker OR 01]
// De app stuurt geen datum mee; de firmware vult 117, 1, 1 in (1 januari 2017).
// Bit 0 van het laatste byte staat altijd aan (de bouwer doet OR 01).
inline std::vector<uint8_t> bluebot_planner_set_payload(uint8_t day_mask, int hour, int minute) {
  return {0x75, 0x01, 0x01, (uint8_t) hour, (uint8_t) minute, 0x00, (uint8_t) (day_mask | 0x01)};
}

// Wissen (D1 07): 0F in de zes velden vóór het masker, plus het dagmasker OR 01.
inline std::vector<uint8_t> bluebot_planner_clear_payload(uint8_t day_mask) {
  return {0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, (uint8_t) (day_mask | 0x01)};
}

// Planner-antwoord op D2 07: 14 payload-bytes = 7 slots van 2 bytes, hoogste byte eerst.
// Volgorde: zondag, maandag ... zaterdag (zelfde als orderId 0-6).
// Waarde per slot (bevestigd met Test A): 0xC000 | uur << 6 | minuut
//   bits 10-6 = uur, bits 5-0 = minuut, bits 15-11 = vlaggen (ingesteld: 0x18).
//   0x0000 = geen planning voor die dag.
// Wat de vlaggen 0x8000 en 0x4000 elk betekenen, is nog niet uitgezocht.
inline std::string bluebot_planner_decode(const uint8_t *p, size_t len) {
  if (len != 14) return "Onbekend";
  // Frame: slot 0 = zondag ... slot 6 = zaterdag. Weergave: maandag eerst, zondag laatst.
  static const char *days[7] = {"zo", "ma", "di", "wo", "do", "vr", "za"};
  std::string out;
  char buf[32];
  for (int k = 0; k < 7; k++) {
    int i = (k + 1) % 7;  // ma (1) ... za (6), daarna zo (0)
    uint16_t v = (uint16_t) ((p[2 * i] << 8) | p[2 * i + 1]);
    if (v == 0)
      snprintf(buf, sizeof(buf), "%s uit", days[i]);
    else
      snprintf(buf, sizeof(buf), "%s %02d:%02d", days[i], (v >> 6) & 0x1F, v & 0x3F);
    if (!out.empty()) out += " | ";
    out += buf;
  }
  return out;
}
