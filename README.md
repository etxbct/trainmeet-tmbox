# TrainMeet TMBox

ESP8266 och ESP32 kör samma serverstyrda TMBox-funktioner. **Från firmware
0.7.0 krävs TrainMeet Server 1.10.0 eller senare. Uppdatera servern först.**
Sedan TrainMeet Server 2.0.0 talar servern bara 16×2-profilen över MQTT: en box
med firmware äldre än 0.7.0 behöver flashas om. Firmware 0.7.4 tar bort den
sista koden för de äldre protokollen; 0.7.0–0.7.3 fungerar också mot 2.0.0.
Vi börjar med 16 tecken × 2 rader även på en större fysisk display.

Servern äger trafikbeslut, skärmar, språk och tangenternas betydelse.
Firmwaren visar bilden, håller sifferinmatning lokalt och hanterar hårdvara,
nätverk och återanslutning. A–D är funktionsknappar, inte destinationer.

Projektet är öppet och publicerat under MIT-licensen. Koden kan användas, granskas, ändras och distribueras enligt villkoren i [LICENSE](LICENSE).

## Benny: hämta och installera

1. Uppdatera den lokala servern till minst **1.10.0**. Avsluta aktiva äldre
   klareringar före övergången; ingen träffdata ska nollställas.
2. Öppna [Nedladdningar / Releases](https://github.com/beahead-ab/trainmeet-tmbox/releases)
   och välj **Arduino IDE – NodeMCU ESP8266** för NodeMCU-kortet. Paketets
   namn innehåller `arduino-nodemcu-i2c`. Välj inte GitHubs generiska
   ”Source code (zip)”. [Detaljerad paketguide](docs/FIRMWARE-DOWNLOADS.md).
3. Packa upp hela paketet och öppna dess `.ino` i Arduino IDE. Behåll alla
   stödfiler tillsammans. Följ paketets kort- och biblioteksanvisningar.
4. Ladda via USB. Boxen hittar servern på samma Wi-Fi; admin tilldelar
   station i serverns **Inställningar → TMBox och TKL**.
5. Prova först på testträff: begär, godkänn, avgå, ta emot och återtag före
   avgång. Ett godkänt bygge är inte ett genomfört hårdvaruprov.

För ESP32 väljer du paketet som matchar kortet: ESP32-S3, `esp32-benny` eller
`esp32-classic-safe`. **En vanlig ESP32 är inte en ESP32-S3.**
Den svenska [USB-installationsguiden](docs/INSTALLATION.md) finns också som
separat förhandsversionspaket. Ingen fysisk enhet fjärrflashas av en serverrelease.

## Användning

| Tangent eller händelse | Beteende |
|---|---|
| Skriv tågnummer, sedan `#` | Siffrorna hålls lokalt; servern hittar rätt avgång och nästa station |
| `C` / `D` | Föregående/nästa val eller kommande avgång |
| `#` | Primär åtgärd som visas, exempelvis begär, godkänn, avgå eller mottaget |
| `*` | Tillbaka/avbryt; i rätt trafiksituation neka eller återta före avgång |
| `A` | Snabbväg till förfrågningskön där den visas |
| Inkommande förfrågan | Öppnas automatiskt när boxen är ledig; köindikering vid flera |
| Mottaget hos grannen | Kort besked till avsändaren, sedan försvinner det avslutade tåget |
| `*` från översikt | Språkval, bläddra med C/D och spara med `#` |

Följ alltid skärmens aktuella tangentbeskrivningar. Godkännande är inte
avgång; avgång bekräftas först när tåget faktiskt lämnar. När tåget har
lämnat kan avsändaren inte återta det. Mottagaren kan ange avvikande spår
i samband med ankomst. Admin kan också skicka ett språkval till boxen.

Klockan visas till höger på rad två. Endast aktuella texter och nödvändiga
specialtecken laddas från servern, inklusive ÅÄÖ. Boxens egna texter, innan
servern svarat och `VÄNTAR PÅ SVAR`, ritar ÅÄÖ med samma tecken sedan 0.7.5.
En tom översta rad är tom.
Språk eller trafikregler behöver därför normalt ingen ny firmware efter
denna engångsuppgradering till den serverstyrda profilen.

## Nätverk och station

Vid första start väljer du träffens 2,4 GHz-Wi-Fi i boxens portal.
Servern upptäcks lokalt med mDNS; webbporten 8787 är inte MQTT-porten 1883.
NodeMCU har inga manuella IP-/portfält. Servern väljer station efter boxens
permanenta ID. Boxen väntar och kan inte påverka trafik före tilldelningen.
ESP32 behåller sitt befintliga Wi-Fi-installationsflöde.

Ingen upprepning av stationsuppdrag var tionde sekund behövs. Servern
skickar ändrade bilder; små liveness-kvitton kontrollerar anslutningen.
Vid avbrott spärras trafikknapparna och inga gamla kommandon återspelas.
MQTT-brokern ska inte exponeras oskyddad mot internet.

## Webbklient och provbänk

- [Webb-TMBox](https://server.trainmeet.app/tmbox/) är en riktig klient på
  testservern. Starta utan inloggning, visa koden för admin och invänta station.
- [Provbänken](https://server.trainmeet.app/tmbox-lab/) använder isolerad
  demotrafik. Den påverkar aldrig träffen, även när du nollställer enheter.
- På din lokala server finns samma sökvägar efter uppgraderingen.

Serverns `terminal16.py` och `terminal16_runtime.py` är numera auktoritativa
för skärmar och trafikflöden. `firmware/common/server_terminal.h` är den
gemensamma transporten för de fysiska korten. Den äldre lokala ESP32-kärnan
och dess golden-filer behålls för kompatibilitet och regressionstester,
men definierar inte det nya operatörsflödet.

## Hårdvara och byggning

- [ESP8266: koppling, LiquidCrystal_PCF8574 och hårdvarutest](firmware/esp8266/README.md)
- [ESP32: byggning och koppling](firmware/esp32/README.md)
- [ESP32-S3 referensprofil](docs/TMBOX-V2-HARDWARE.md)
- [Äldre boxarnas hårdvara](docs/TMBOX-V1-LEGACY.md)

De historiska namnen V1/V2 i hårdvarudokument betyder inte två olika nya
trafikprodukter. En firmwareversion byggs för flera kort. Samma 16×2-logik
gäller nu för båda; framtida funktioner kan kräva starkare hårdvara.

## Versionsnummer

`VERSION` är auktoritativ och uppdateras automatiskt vid merge till main.
FIRMWARE_VERSION och paketen härleds från den. Bidragsgivare ska normalt
inte sätta ett eget nytt versionsnummer i sina pull requests.
Kompilering, paketering och automatiska tester är inte fysiska bänktester.

Repot innehåller inte iPhone-appen. Se [dokumentationsöversikten](docs/README.md).

## Licens

MIT © Beahead AB. Se [LICENSE](LICENSE).
