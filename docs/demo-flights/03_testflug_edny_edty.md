# Testflug EDNY → EDTY
**Friedrichshafen Tower → Schwäbisch Hall Information**
NfL Sprechfunk 2024 | VFR | Tagflug

> **Legende:**
> 🎙 = Pilot spricht (du)
> 📻 = ATC/Plugin antwortet (erwartet)
> ✅ = Plugin soll reagieren
> ⚠️ = Testpunkt / worauf achten
> `[X]` = variabel / situationsabhängig

---

## PHASE 1 — Abflug EDNY (bereits getestet, zur Vollständigkeit)

**Frequenz: Friedrichshafen Tower 120.080**

🎙 `Friedrichshafen Tower, Delta Echo Romeo Kilo Lima, DA20, am Vorfeld, VFR nach Schwäbisch Hall, INFORMATION [X], bitte Startfreigabe.`

📻 `Delta Echo Romeo Kilo Lima, Friedrichshafen Tower, Piste [X], Wind [X], QNH [X], Start frei.`

🎙 `Start frei, Piste [X], QNH [X], Delta Echo Romeo Kilo Lima.`

*(Abflug, aufsteigen auf Reiseflughöhe)*

---

## PHASE 2A — Frequenzwechsel nach Abflug

**Trigger: Tower gibt Frequenzwechsel**

📻 `Delta Echo Romeo Kilo Lima, Frequenzwechsel genehmigt, auf Wiederhören.`

🎙 `Verlasse Frequenz, Delta Echo Romeo Kilo Lima.`

⚠️ **Testpunkt:** Plugin muss `LEAVING_FREQUENCY` erkennen und in `EN_ROUTE` wechseln. Kein weiterer ATC-Dialog bis RMZ.

---

## PHASE 2B — Stille EN_ROUTE

**Ca. 20 Minuten Überflug, kein ATC-Kontakt.**

⚠️ **Testpunkt:** Plugin bleibt still. Keine unaufgeforderten Meldungen. State = `EN_ROUTE`.

*(Optional: Langen Information auf 128.950 — aber kein Plugin-Dialog erwartet, Scope Phase 2)*

---

## PHASE 2C — RMZ-Einflug EDTY

**Frequenz: Schwäbisch Hall Information 129.230**
**Einflug RMZ ca. 5 NM vor Platz**

🎙 `Schwäbisch Hall Information, DA20 Delta Echo Romeo Kilo Lima, [Position z.B. nördlich Crailsheim], VFR, [Höhe] Fuß, werde in RMZ einfliegen zur Landung in Schwäbisch Hall.`

> NfL §7.4 *a): `(LFZ-Muster) (Position) (Flugregeln) (Ziffern) FUSS, WERDE IN RMZ EINFLIEGEN [...] ZUR LANDUNG [IN (Flugplatz)]`

📻 `Echo Romeo Kilo Lima, Schwäbisch Hall Information, [Verkehrsinfo oder] Landebahn [X], QNH [X], melden Sie [Position in Platzrunde].`

⚠️ **Testpunkt:** Plugin erkennt `RMZ_ENTER`, antwortet mit INFO-Phraseologie (keine Freigabe!), Rufname „Schwäbisch Hall Information" — NICHT „Tower".

⚠️ **Testpunkt:** `FrequencyType::INFO` korrekt erkannt für EDTY 129.230.

---

## PHASE 2D — Platzrunde / Anflug

**NfL §1.4.13 — Einflug Platzrunde**

🎙 `Schwäbisch Hall Information, DA20 Echo Romeo Kilo Lima, [Position in Platzrunde], [Höhe] Fuß, zur Landung.`

📻 `Echo Romeo Kilo Lima, Schwäbisch Hall Information, [Verkehrsinfo], melden Sie Endanflug.`

⚠️ **Testpunkt:** Antwort ist beratend, keine Landefreigabe (AFIS-Charakter).

🎙 `Melde Endanflug, Echo Romeo Kilo Lima.`

📻 `Echo Romeo Kilo Lima, verstanden.` *(oder Verkehrsinfo)*

---

## PHASE 2E — RMZ verlassen (falls Durchflug ohne Landung)

> Nur relevant wenn kein Landen — hier zur Vollständigkeit dokumentiert.

🎙 `Verlasse RMZ [Position] [Höhe] Fuß, Echo Romeo Kilo Lima.`

> NfL §7.4 *b): `VERLASSE RMZ (Position) (Ziffern) FUSS`

⚠️ **Testpunkt:** Plugin erkennt `RMZ_LEAVE`, bestätigt, kehrt zu `EN_ROUTE` zurück.

---

## Gegenprobe — Radio-Platz

**Empfehlung: EDFS Schweinfurt Radio (nach separatem Testflug)**

⚠️ **Testpunkt:** Rufname muss „Schweinfurt Radio" sein — nicht „Tower", nicht „Information".
⚠️ **Testpunkt:** `FrequencyType::RADIO` korrekt klassifiziert.

---

## Zusammenfassung Testpunkte

| # | Was testen | Erwartetes Verhalten |
|---|---|---|
| T1 | LEAVING_FREQUENCY nach Abflug | State → EN_ROUTE, Plugin still |
| T2 | EN_ROUTE Stille | Kein unaufgeforderter Dialog |
| T3 | RMZ_ENTER Erstkontakt EDTY | INFO-Flow, kein Tower-Rufname |
| T4 | FrequencyType EDTY 129.230 | `INFO`, nicht `TOWER` |
| T5 | AFIS-Antwort ohne Freigabe | Beratend, kein „Start/Landung frei" |
| T6 | RMZ_LEAVE | State → EN_ROUTE |
| T7 | Gegenprobe RADIO-Platz | Rufname „[Platz] Radio" |
