# Bluebot 790T met ESPHome en Home Assistant

Een ESP32-C3 Super Mini vervangt de wifi-module van een Bluebot 790T (Proscenic 790T) en praat via UART met de robot. Zo werken de functies van de oorspronkelijke app, die niet meer werkt, weer in ESPHome en Home Assistant, zonder cloud. Deze repository bevat de ESPHome-configuratie, de Home Assistant-kant en een dashboardkaart (via HACS) die de plattegrond en het gereden pad toont.

## Inhoud

| Map | Wat |
| --- | --- |
| `esphome/` | ESPHome-configuratie en de headers met het protocol, de kaart en de frame-ontvangst |
| `homeassistant/` | Sensor die de kaart uit het ESPHome-event opslaat |
| `dist/` | De dashboardkaart `bluebot-map-card.js` (HACS) |

## Wat werkt

Start, stop, dock, rijden en schoonmaakmodi, statussensoren en foutregisters, zuigkracht, robotgeluid, zoek de robot, klok instellen en planner lezen, instellen en wissen werken. De kaart en de track (positieframes, raster, RLE, base64, kaart-reset via `D7 07`) zijn gebouwd naar de firmware-analyse en getest op echte data van de robot. De live-updates en de HACS-kaart zijn nog niet breed getest.

Niet bevestigd: de volgorde Uit/Zacht/Hard bij de zuigkracht, de eenheid van `clearTime`, de betekenis van `doTime`, `clearSign` en `clearModule`, en de celgrootte (20 cm is een aanname).

## ESPHome

1. Kopieer de map `esphome/` naar je ESPHome-configuratie.
2. Kopieer `secrets.yaml.example` naar `secrets.yaml` en vul je eigen gegevens in. Dit bestand staat in `.gitignore`.
3. Compileer en flash. De headers `bluebot_helpers.h` en `bluebot_map.h` moeten naast het YAML-bestand staan.
4. Voor het event naar Home Assistant: zet in Home Assistant bij het apparaat (Instellingen, Apparaten, ESPHome, Configureren) de optie "Allow the device to perform Home Assistant actions" aan.

De logs van een schoonmaakbeurt bevatten de plattegrond als tekst (regels `KAART`) en de kaart in base64 (regels `MAPB64`). Zet de schakelaar "Bluebot RX-frames loggen" uit als de logging te veel wordt.

## Home Assistant

1. Zet de inhoud van `homeassistant/bluebot_kaart_sensor.yaml` in je configuratie. Dat maakt `sensor.bluebot_kaart`, met de kaart en de track als attributen, en houdt die uit de recorder.
2. Voeg de dashboardkaart toe (zie hieronder).

## Dashboardkaart via HACS

1. HACS, menu rechtsboven, **Aangepaste repositories**. Vul de URL van deze repository in en kies de categorie **Dashboard**.
2. Zoek "Bluebot 790T map card" en kies **Downloaden**. HACS zet de resource zelf op `/hacsfiles/bluebot-790t/bluebot-map-card.js`. Controleer dat onder Instellingen, Dashboards, Resources. Draai je dashboards in YAML-modus, dan voeg je die resource handmatig toe als module.
3. Ververs de browser met een harde refresh.

```yaml
type: custom:bluebot-map-card
entity: sensor.bluebot_kaart
title: Bluebot
```

Opties: `show_toggles`, `show_track`, `show_clean`, `show_obstacles`, `max_cell` (pixels per cel, standaard 14), `margin` (cellen marge, standaard 2), `track_color`, `clean_color`.

## Kaartformaat

De ESP stuurt kaart en track in het formaat van de oorspronkelijke firmware van de wifi-module:

- **Kaart:** base64 van een kop van 9 bytes (`00 00 00 00 00 00 64 00 64`, een raster van 100 bij 100) gevolgd door een run-length-codering van het raster. Elke rasterbyte bevat 4 cellen van 2 bit, waarbij de cel met `x mod 4 = 0` in de hoogste 2 bit staat. Celwaarden: 0 onbekend, 1 obstakel, 2 schoon. Een byte met de bovenste twee bits op 1 is een lengtebyte (6 bits, hoogste stuk eerst), gevolgd door de bytewaarde.
- **Track:** base64 van een versiebyte (`01`), de clearArea, het aantal punten (2 bytes, little-endian) en daarna (x, rij) per hoekpunt. Rij 0 is boven, net als in de kaart. De track bevat alleen de hoekpunten van rechte stukken.

De decoder staat los van het tekenen (`decodeMap`, `decodeTrack`, `mapBounds` in het JS-bestand) en is ook in Node te gebruiken.

## Herkomst en bronvermelding

Het formaat is niet door de fabrikant gedocumenteerd. Het is afgeleid door de firmware van de oorspronkelijke wifi-module (ESP8266) te analyseren met Ghidra, en gecontroleerd tegen data van een echte robot.

Als referentie voor het formaat en de bijbehorende berichten zijn gebruikt:

- [felix-engelmann/robotbona](https://github.com/felix-engelmann/robotbona): het voorbeeldbericht met kaart en track uit dit project was de enige beschikbare grondwaarheid voor het formaat. Zonder dat voorbeeld was het ontcijferen van de codering niet te toetsen geweest.
- [deblockt/hass-proscenic-790T-vacuum](https://github.com/deblockt/hass-proscenic-790T-vacuum/): Home Assistant-integratie voor de Proscenic 790T, die het lokale protocol van de robot spreekt. Gebruikt als achtergrond bij het kaartformaat en het berichtenverkeer. Het is een andere route dan deze repository (het praat via het netwerk met de robot, niet via de UART van de ESP).

Beide projecten zijn MIT-gelicentieerd. Er is geen code uit overgenomen: de decoder, de ESPHome-code en de kaart zijn zelf geschreven op basis van de firmware-analyse en het voorbeeld.

## Licentie en disclaimer

MIT, zie `LICENSE`. Dit project staat los van Bluebot, Proscenic en de fabrikant van de robot. Namen en merken zijn van hun eigenaren. De informatie komt uit eigen analyse van een eigen toestel en kan onvolledig of onjuist zijn. Gebruik op eigen risico: het protocol bevat commando's die de firmware van de robot kunnen beschadigen, en die zijn bewust niet opgenomen.
