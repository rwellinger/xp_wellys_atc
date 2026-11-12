# Demo-Flug 1 — VFR-Rundflug Friedrichshafen (EDNY, lokal)

> **Was das ist:** Ein vollständiges, NfL-Sprechfunk-2024-konformes
> Funkprotokoll für einen **lokalen Rundflug** ab/an Friedrichshafen
> (EDNY). Start, lokaler Ausflug im Nahbereich der Kontrollzone, Rückkehr
> in die Platzrunde, Landung. **Eine Frequenz, eine Funkstelle** — EDNY ist
> tower_only (Tower macht Boden + Air).
>
> **Nur Kommunikation.** Wind, QNH, Piste und ATIS-Buchstabe unten sind
> Beispiele; im Sim übernimmst du die echten Werte aus ATIS und
> Tower-Freigabe.

---

## Plugin-Einstellungen für diesen Flug

| Einstellung | Wert |
|---|---|
| **VFR-Flugart** | **Platzrunde** (kein Zielflugplatz) |
| Zielflugplatz | — (leer lassen) |
| Pilot-Rufzeichen | `D-EMAB` → gesprochen *Delta Echo Mike Alfa Bravo* |
| Luftfahrzeug | DV20 (Diamond DA20 Katana) |
| BZF-Strict-Mode | beliebig (aus = stille Akzeptanz, an = Pflicht-Readback wird geprüft) |
| Backend | beliebig (Local / OpenAI / Mistral) |

Mit **Flugart = Platzrunde** rendert das Plugin das Vorhaben-Element als
`VFR Platzrunde`. Für einen Ortsrundflug ist auch `VFR Rundflug` korrekt —
der Konformitäts-Check erkennt beide (`platzrunde` / `rundflug`). Es wird
**kein** Zielflugplatz und **keine** „Kurs nach …"-Floskel gesprochen.

---

## EDNY-Eckdaten

| | |
|---|---|
| ATIS | 127.575 |
| Tower (TWR/RDR, kombiniert Boden + Air) | **120.075** |
| Pisten | 06 / 24 — beide linke Platzrunde |
| Platzrundenhöhe | ca. 2500 ft MSL (≈ 1100 ft AGL) |
| VRPs | November (N), Oscar (NE), Sierra (S), Whiskey (W) |
| Beispiel-Wetter | Wind 240°/8 kt, QNH 1013, Information Alpha, Piste **24** aktiv |

COM1 die ganze Zeit auf **120.075**.

Legende je Schritt — **Du:** PTT-Sprechtext · **Tower:** erwartete Antwort
· **Readback:** was du zurücklesen musst · **NfL:** Regelanker.

---

## Phase 1 — ATIS abhören (keine Aussendung)

COM1 kurz auf **127.575** (oder das Plugin spielt ATIS automatisch, wenn du
in Reichweite bist), Information **Alpha** notieren, dann zurück auf
**120.075**.

> ATIS-Beispiel: *„Friedrichshafen Information Alpha, Piste 24 in Betrieb,
> Wind 240 Grad 8 Knoten, QNH 1013, …"*

---

## Phase 2 — Rollanmeldung am Tower (tower_only)

**Schritt 1 — Erstanruf + Rollanmeldung**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, DV20, am
  Vorfeld, VFR Rundflug, Information Alpha, erbitte Rollen."
- **Tower:** „Delta Echo Mike Alfa Bravo, Friedrichshafen Turm, guten Tag,
  Information Alpha, rollen Sie zum Rollhalt Piste 24 ueber Alpha, QNH 1013."
- **Readback (NfL §25 b) Nr. 1):** Rufzeichen · Piste 24 · QNH 1013
- **Du:** „Rollen zum Rollhalt Piste 24 ueber Alpha, QNH 1013, Delta Echo
  Mike Alfa Bravo."
- **Tower:** *(still — geht nach `TAXI_CLEARED`)*
- **NfL:** §14, ANLAGE 1.4.7 a) („ERBITTE ROLLEN [Absichten]" — ohne
  Zielflugplatz = Lokalflug); §25 b) Nr. 1.

> **Plugin-Hinweis:** „Friedrichshafen **Turm**" wird vom Parser als
> `INITIAL_CALL_TOWER` erkannt und an tower_only-Plätzen automatisch auf
> `INITIAL_CALL_GROUND` kollabiert (Log: `Tower-only collapse: …`), damit
> die Rollanmeldung wie an einem Platz mit Boden behandelt wird. Das
> Vorhaben „VFR Rundflug" steht in der Roll-Anfrage (`REQUEST_TAXI`), nicht
> im reinen Erstanruf — genau wie NfL 1.4.7 es verortet.

---

## Phase 3 — Startbereit & Startfreigabe (gleiche Frequenz)

Am Rollhalt Piste 24 — kein Frequenzwechsel (du bist schon am Tower).

**Schritt 2 — Startbereit (Platzrunde/Lokal)**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, Rollhalt
  Piste 24, abflugbereit."
- **Tower:** „Delta Echo Mike Alfa Bravo, Friedrichshafen Tower, Piste 24,
  Start frei, Wind 240 Grad 8 Knoten, melden Sie im Gegenanflug."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Piste 24, Start frei, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.10 / 1.4.11; §25 b) Nr. 1 ii.

> **Lokal = kein Kurs.** Weil es ein Rundflug ohne Zielflugplatz ist, kommt
> hier die schlichte Startfreigabe **ohne** „Kurs nach …" und **ohne**
> „Frequenzwechsel genehmigt" (das gibt es nur beim Überlandflug, siehe
> Demo-Flug 2). Du bleibst beim Tower.

Jetzt **starten**.

---

## Phase 4 — Lokaler Rundflug im Nahbereich (Verbleib in der CTR)

Du verlässt die Platzrunde für einen lokalen Bogen über dem Bodensee und
bleibst dabei in der Kontrollzone / im Nahbereich auf **120.075**.

**Schritt 3 — Ausflug-Absicht melden**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, verlasse die
  Platzrunde Richtung Whiskey, Verbleib im Nahbereich, VFR Rundflug."
- **Tower:** „Delta Echo Mike Alfa Bravo, verstanden, melden Sie zur
  Rückkehr." *(Wortlaut variabel)*
- **Readback:** Rufzeichen
- **Du:** „Melde zur Rückkehr, Delta Echo Mike Alfa Bravo."
- **NfL:** §16 (Positions-/Absichtsmeldungen), ANLAGE 1.4.19.

**Schritt 4 — Position über VRP (während des Rundflugs, nach Bedarf)**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, ueber Whiskey,
  2500 Fuss."
- **Tower:** „Delta Echo Mike Alfa Bravo, verstanden."
- **Readback:** keiner (reine Positionsmeldung).
- **NfL:** ANLAGE 1.4.19 a).

---

## Phase 5 — Rückkehr & Wiedereinflug in die Platzrunde

**Schritt 5 — Rückmeldung über VRP Sierra, zur Landung**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, ueber Sierra,
  2500 Fuss, zur Landung."
- **Tower:** „Delta Echo Mike Alfa Bravo, Friedrichshafen Tower, einfliegen
  in den Gegenanflug Piste 24, melden Sie Mitte Gegenanflug."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Einfliegen Gegenanflug Piste 24, melde Mitte Gegenanflug, Delta
  Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.13 (Einflug Platzrunde).

**Schritt 6 — Gegenanflug melden (abeam Pistenmitte)**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, Gegenanflug
  Piste 24."
- **Tower:** „Delta Echo Mike Alfa Bravo, Nummer eins, Piste 24, weiter
  Anflug, melden Sie Endanflug."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Melde Endanflug, Piste 24, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.14 (in der Platzrunde).

---

## Phase 6 — Endanflug & Landung

**Schritt 7 — Endanflug melden**
- **Du:** „Endanflug Piste 24, Delta Echo Mike Alfa Bravo."
- **Tower:** „Delta Echo Mike Alfa Bravo, Piste 24, Landung frei, Wind 240
  Grad 8 Knoten."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Piste 24, Landung frei, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.16 (Landefreigabe); §25 b) Nr. 1.

---

## Phase 7 — Piste verlassen & Frequenz verlassen (tower_only)

**Schritt 8 — Piste verlassen**
- **Du:** „Delta Echo Mike Alfa Bravo, Piste verlassen."
- **Tower:** „Delta Echo Mike Alfa Bravo, verstanden, rollen Sie zum
  GAT-Vorfeld ueber Alpha, auf Wiederhoeren."
- **Readback:** Rufzeichen (Roll-Ziel zurücklesen ist gute Praxis).
- **Du:** „Rollen GAT-Vorfeld ueber Alpha, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.20 (Kommunikation nach der Landung). Kein
  „kontaktieren Sie Rollkontrolle" — tower_only.

**Schritt 9 — Frequenz verlassen (am Abstellplatz)**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, verlasse
  Frequenz."
- **Tower:** „Delta Echo Mike Alfa Bravo, Verlassen der Frequenz genehmigt,
  auf Wiederhoeren."
- **Readback:** keiner.
- **NfL:** ANLAGE 1.4.20; selbstabschließend.

---

## Kurzreferenz — gesprochene Pilotenzeilen

1. Friedrichshafen Turm, D-EMAB, DV20, am Vorfeld, VFR Rundflug, Information Alpha, erbitte Rollen.
2. Rollen zum Rollhalt Piste 24 ueber Alpha, QNH 1013, D-EMAB.
3. Friedrichshafen Turm, D-EMAB, Rollhalt Piste 24, abflugbereit.
4. Piste 24, Start frei, D-EMAB.
5. Verlasse die Platzrunde Richtung Whiskey, Verbleib im Nahbereich, VFR Rundflug.
6. Ueber Sierra, 2500 Fuss, zur Landung.
7. Gegenanflug Piste 24. → Endanflug Piste 24.
8. Piste 24, Landung frei, D-EMAB.
9. Piste verlassen. → Verlasse Frequenz.

*(Gesprochen wird das Rufzeichen stets phonetisch ausbuchstabiert: „Delta
Echo Mike Alfa Bravo".)*
