# Smart Parking — Firebase izdanje

Jednostavna web kontrolna tabla za postojeći ESP32 parking projekat. Stranica prikazuje stanje mesta, rampu, trajanje parkiranja, trenutnu cenu, vreme dolaska i poslednji račun. Automobil i rampa imaju CSS animacije.

## 1. Šta se nalazi u projektu

- `public/index.html` — web aplikacija za Firebase Hosting.
- `firebase.json` — Firebase Hosting i Realtime Database konfiguracija.
- `database.rules.json` — pravila pristupa bazi.
- `esp32/smart_parking_firebase.ino` — Arduino kod za WEMOS LOLIN32 / ESP32 sa Wi-Fi i Firebase slanjem podataka.

## 2. Napravi Firebase projekat

1. Otvori [Firebase Console](https://console.firebase.google.com/) i napravi projekat.
2. U projektu dodaj **Web app** (Project settings → Your apps → Web) i sačuvaj konfiguraciju.
3. Otvori **Build → Realtime Database** i napravi bazu. Izaberi region. Kopiraj tačan Database URL koji Firebase prikaže.
4. Otvori **Build → Authentication → Sign-in method** i uključi:
   - **Anonymous** — web stranica se ovako prijavljuje samo da čita stanje.
   - **Email/Password** — ESP32 se ovako prijavljuje da upisuje podatke.
5. U **Authentication → Users** dodaj korisnika:
   - Email: `parking-device@example.com`
   - Password: napravi jaku lozinku i sačuvaj je. Tu lozinku ćeš upisati samo u ESP32 kod.

Ne menjaj email uređaja bez izmene istog emaila u `database.rules.json`.

## 3. Podesi web aplikaciju

Otvori `public/index.html` i u objektu `cfg` zameni:

- `ZAMENI_API_KEY` — `apiKey` iz Firebase Web konfiguracije.
- `ZAMENI_PROJECT_ID` — ID projekta; unesi ga u `authDomain`, `databaseURL` i `projectId`.
- `ZAMENI_APP_ID` — `appId` iz Firebase Web konfiguracije.

Za `databaseURL` kopiraj **tačan URL** iz Realtime Database stranice. Primer u fajlu koristi `europe-west1`, ali tvoja baza može biti u drugom regionu.

## 4. Podesi ESP32

U Arduino IDE instaliraj biblioteke:

- `ESP32Servo`
- `ArduinoJson` (Benoit Blanchon)

Izaberi svoju ESP32 ploču i otvori `esp32/smart_parking_firebase.ino`. Na vrhu fajla unesi:

- `WIFI_SSID` — naziv tvoje Wi-Fi mreže od 2.4 GHz.
- `WIFI_PASSWORD` — Wi-Fi lozinku.
- `FIREBASE_API_KEY` — isti Web API key kao u web aplikaciji.
- `FIREBASE_DATABASE_URL` — isti tačan Database URL iz Firebase konzole.
- `FIREBASE_DEVICE_PASSWORD` — lozinku Firebase korisnika `parking-device@example.com`.

Pinovi, pragovi senzora, brzina rampe i cena su zadržani iz tvog originalnog koda. Cena je trenutno `10 RSD` po punom minutu.

**Napomena o HTTPS-u:** radi jednostavnijeg prvog testiranja, ESP32 kod koristi `setInsecure()` i time preskače proveru TLS sertifikata. To je kompromis za prototip, ne preporuka za javnu/produkcijsku instalaciju. Za ozbiljnu instalaciju dodaj proveru CA sertifikata.

## 5. Objavi na Firebase Hosting

Instaliraj Node.js, pa u terminalu pokreni:

```bash
npm install -g firebase-tools
firebase login
```

Uđi u glavni folder `smart-parking-firebase` (onaj u kome se nalazi `firebase.json`), pa poveži Firebase projekat:

```bash
firebase use --add
```

Izaberi projekat koji si napravio. Zatim objavi web stranicu i pravila baze:

```bash
firebase deploy --only hosting,database
```

Firebase će ispisati Hosting adresu, obično oblika `https://TVOJ-PROJEKAT.web.app`.

## 6. Kako su zaštićeni podaci

`database.rules.json` dozvoljava:

- čitanje prijavljenim korisnicima — web aplikacija koristi Firebase Anonymous Auth;
- pisanje samo prijavljenom korisniku čiji je email `parking-device@example.com` — ESP32 koristi Email/Password Auth.

Pošto je anonimna prijava uključena, svako ko zna adresu sajta može da pročita status parkinga. Ne može da menja podatke kroz Firebase pravila. Ako kasnije želiš privatnu kontrolnu tablu, treba dodati korisničku prijavu umesto anonimnog čitanja.

## 7. Provera rada

1. Otvori Hosting adresu. Dok Firebase podaci nisu uneti u `index.html`, stranica prikazuje demo animaciju.
2. Kada uneseš Firebase konfiguraciju, web aplikacija prestaje da simulira podatke i sluša `/parking` u Realtime Database.
3. Uključi ESP32. U Serial Monitor-u izaberi `115200 baud`.
4. Kada ESP32 uspešno pošalje podatke, u podnožju stranice će pisati `ESP32 povezan`.
5. Ako piše `ESP32 nije povezan`, proveri Wi-Fi, NTP vreme, API key, Database URL, Email/Password korisnika i pravila baze.

Podaci koje ESP32 šalje u `/parking` su `state`, `gate`, `since`, `cost`, `price`, `last` i `seen`.
