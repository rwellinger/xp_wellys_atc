# Demo-Flug 2 — VFR-Überlandflug Friedrichshafen → Memmingen (EDNY → EDJA)

> **Was das ist:** Ein vollständiges, NfL-Sprechfunk-2024-konformes
> Funkprotokoll für einen **Überlandflug** von Friedrichshafen (EDNY) nach
> Memmingen (EDJA, ≈ 35 NM Nordosten). Start mit Zielangabe, Verlassen der
> EDNY-Frequenz, Reiseflug, Erstanruf und Einflug Memmingen, Landung.
>
> **Nur Kommunikation.** Wind, QNH, Piste, ATIS-Buchstabe und die
> EDJA-Frequenzen unten sind Beispiele — im Sim die echten Werte aus ATIS,
> Tower-Freigabe bzw. aktueller AIP VFR übernehmen.

---

## Plugin-Einstellungen für diesen Flug

| Einstellung | Wert |
|---|---|
| **VFR-Flugart** | **Überlandflug** |
| **Zielflugplatz** | `EDJA` |
| Pilot-Rufzeichen | `D-EMAB` → gesprochen *Delta Echo Mike Alfa Bravo* |
| Luftfahrzeug | DV20 (Diamond DA20 Katana) |
| BZF-Strict-Mode | beliebig |
| Backend | beliebig (Local / OpenAI / Mistral) |

Mit **Flugart = Überlandflug** und **Ziel = EDJA** rendert das Plugin das
Vorhaben-Element als `VFR nach EDJA` und die Startbereit-Floskel als
`… abflugbereit, Kurs nach EDJA` (Variable `{vfr_course_phrase}`). Wäre das
Ziel leer, fiele die Kursfloskel weg (kein „nach Plan"-Platzhalter).

---

## Eckdaten

**EDNY Friedrichshafen (Abflug — tower_only)**

| | |
|---|---|
| ATIS | 127.575 |
| Tower (Boden + Air) | **120.075** |
| Piste (Beispiel) | 24, linke Platzrunde |
| Ausflug-VRP Richtung EDJA | Oscar (NE) |

**EDJA Memmingen (Ankunft — kontrolliert, Class D)**

| | |
|---|---|
| ATIS | 127.150 *(AIP-Wert prüfen)* |
| Radar / Director (CTR-Einflug) | 119.475 *(AIP-Wert prüfen)* |
| Tower | 124.875 *(AIP-Wert prüfen)* |
| Piste (Beispiel) | 24 |

> ⚠️ **EDJA-Frequenzen und Meldepunkte vor dem Flug aus der aktuellen AIP
> VFR (aip.dfs.de/BasicVFR) bzw. dem ATIS übernehmen.** Das Plugin liest die
> tatsächlichen Frequenzen zur Laufzeit aus `apt.dat`; die Werte oben sind
> Beispiele. EDJA ist nicht in der gebündelten VRP-Datenbank — der Tower
> weist das Einflugverfahren zu.

**Optional en-route:** Fluginformationsdienst (z. B. *München Information*)
— Frequenz aus AIP/Karte. Für den reinen Funkablauf nicht zwingend.

Legende je Schritt — **Du:** PTT-Sprechtext · **Tower:** erwartete Antwort
· **Readback:** was du zurücklesen musst · **NfL:** Regelanker.

---

## Phase 1 — ATIS abhören (keine Aussendung)

Information **Alpha** von EDNY-ATIS (127.575) notieren, COM1 auf **120.075**.

---

## Phase 2 — Rollanmeldung am Tower (mit Zielangabe)

**Schritt 1 — Erstanruf + Rollanmeldung mit Ziel**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, DV20, am
  Vorfeld, VFR nach EDJA, Information Alpha, erbitte Rollen."
- **Tower:** „Delta Echo Mike Alfa Bravo, Friedrichshafen Turm, guten Tag,
  Information Alpha, rollen Sie zum Rollhalt Piste 24 ueber Alpha, QNH 1013."
- **Readback (NfL §25 b) Nr. 1):** Rufzeichen · Piste 24 · QNH 1013
- **Du:** „Rollen zum Rollhalt Piste 24 ueber Alpha, QNH 1013, Delta Echo
  Mike Alfa Bravo."
- **Tower:** *(still — geht nach `TAXI_CLEARED`)*
- **NfL:** §14; **ANLAGE 1.4.7 b)** — „(Flugregeln) NACH (Zielflugplatz)
  ERBITTE ROLLEN [Absichten]" → hier „VFR nach EDJA"; §25 b) Nr. 1.

> **Plugin-Hinweis:** „Friedrichshafen **Turm**" wird an tower_only-Plätzen
> automatisch von `INITIAL_CALL_TOWER` auf `INITIAL_CALL_GROUND` kollabiert.
> Das Ziel „nach EDJA" gehört nach NfL 1.4.7 b in die Roll-Anfrage
> (`REQUEST_TAXI`) — genau dort rendert das Plugin das `{intention}`-Element.

---

## Phase 3 — Startbereit & Startfreigabe (Überlandflug)

**Schritt 2 — Startbereit mit Kursangabe**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, Rollhalt
  Piste 24, abflugbereit, Kurs nach EDJA."
- **Tower:** „Delta Echo Mike Alfa Bravo, Friedrichshafen Tower, Piste 24,
  Start frei, Wind 240 Grad 8 Knoten, Kurs nach Plan freigegeben,
  Frequenzwechsel nach Start genehmigt."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Piste 24, Start frei, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.10 / 1.4.11; §25 b) Nr. 1 ii.

> **Überland ≠ Lokal:** Die Startfreigabe enthält „Kurs nach Plan
> freigegeben" und „Frequenzwechsel nach Start genehmigt" — das Plugin geht
> in `XC/DEPARTURE_CLEARED`. „Kurs nach Plan freigegeben" ist hier die
> **Controller**-Phraseologie (cleared on course as filed) und korrekt; der
> Platzhalter-Fix betraf nur die **Piloten**-Hints.

Jetzt **starten** und Richtung Nordosten (VRP Oscar) steigen.

---

## Phase 4 — Verlassen der Kontrollzone & Frequenz

**Schritt 3 — Ausflug melden / Frequenz verlassen**
- **Du:** „Friedrichshafen Turm, Delta Echo Mike Alfa Bravo, ueber Oscar,
  verlasse die Kontrollzone Richtung Memmingen, verlasse Frequenz."
- **Tower:** „Delta Echo Mike Alfa Bravo, Verlassen der Frequenz genehmigt,
  auf Wiederhoeren."
- **Readback:** keiner (selbstabschließend).
- **NfL:** ANLAGE 1.4.19; §15 (Verlassen einer Kontrollfrequenz). Plugin →
  `XC/EN_ROUTE`.

---

## Phase 5 — Reiseflug (en-route)

Kein verpflichtender ATC-Kontakt zwischen den Kontrollzonen (Plugin ist im
`EN_ROUTE` still). **Optional** Fluginformationsdienst:

**Schritt 4 (optional) — FIS-Anmeldung**
- **Du:** „München Information, Delta Echo Mike Alfa Bravo, DV20, VFR von
  Friedrichshafen nach Memmingen, FL/Höhe …, erbitte Fluginformationsdienst."
- **FIS:** „Delta Echo Mike Alfa Bravo, München Information, identifiziert,
  …" *(Wortlaut/Frequenz aus AIP)*
- **NfL:** §16 (Fluginformationsdienst), ANLAGE 1.4.19.

Vor Erreichen der EDJA-Kontrollzone FIS verlassen
(„… verlasse Frequenz, wechsle Memmingen") und COM1 auf die EDJA-Frequenz.

---

## Phase 6 — Erstanruf & Einflug Memmingen (EDJA)

ATIS von EDJA abhören (Beispiel: Information **Bravo**), dann CTR-Einflug
beim zuständigen Memmingen-Radar/Tower (Frequenz aus AIP).

**Schritt 5 — Erstanruf Memmingen, zur Landung**
- **Du:** „Memmingen Turm, Delta Echo Mike Alfa Bravo, DV20, suedwestlich
  des Platzes, 3500 Fuss, Information Bravo, zur Landung, erbitte
  Einflugverfahren."
- **Tower:** „Delta Echo Mike Alfa Bravo, Memmingen Tower, einfliegen in den
  Gegenanflug Piste 24, melden Sie Mitte Gegenanflug." *(Einflugverfahren
  wird vom Tower zugewiesen)*
- **Readback:** Rufzeichen · Piste 24 · zugewiesenes Einflugverfahren
- **Du:** „Einfliegen Gegenanflug Piste 24, melde Mitte Gegenanflug, Delta
  Echo Mike Alfa Bravo."
- **NfL:** §16.1.c (Erstanruf Flugplatzkontrolle: Stelle, Rufzeichen,
  Standort; Anflüge nennen die Piste); ANLAGE 1.4.13.

**Schritt 6 — Gegenanflug melden**
- **Du:** „Memmingen Turm, Delta Echo Mike Alfa Bravo, Gegenanflug Piste 24."
- **Tower:** „Delta Echo Mike Alfa Bravo, Nummer eins, Piste 24, weiter
  Anflug, melden Sie Endanflug."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Melde Endanflug, Piste 24, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.14.

---

## Phase 7 — Endanflug & Landung Memmingen

**Schritt 7 — Endanflug melden**
- **Du:** „Endanflug Piste 24, Delta Echo Mike Alfa Bravo."
- **Tower:** „Delta Echo Mike Alfa Bravo, Piste 24, Landung frei, Wind …
  Grad … Knoten."
- **Readback:** Rufzeichen · Piste 24
- **Du:** „Piste 24, Landung frei, Delta Echo Mike Alfa Bravo."
- **NfL:** ANLAGE 1.4.16; §25 b) Nr. 1.

**Schritt 8 — Piste verlassen**
- **Du:** „Delta Echo Mike Alfa Bravo, Piste verlassen."
- **Tower:** „Delta Echo Mike Alfa Bravo, verstanden, rollen Sie zum
  Vorfeld …" *(bei Platz mit Boden ggf. „kontaktieren Sie Rollkontrolle auf
  …")*
- **Readback:** Rufzeichen (+ ggf. Rollkontrolle-Frequenz)
- **NfL:** ANLAGE 1.4.20.

> EDJA hat — anders als EDNY — i. d. R. eine **separate Bodenkontrolle**.
> Falls der Tower nach der Landung auf Rollkontrolle verweist, wechselst du
> die Frequenz und liest sie zurück. Den genauen Wert gibt der Tower vor.

---

## Kurzreferenz — gesprochene Pilotenzeilen

1. Friedrichshafen Turm, D-EMAB, DV20, am Vorfeld, VFR nach EDJA, Information Alpha, erbitte Rollen.
2. Rollen zum Rollhalt Piste 24 ueber Alpha, QNH 1013, D-EMAB.
3. Friedrichshafen Turm, D-EMAB, Rollhalt Piste 24, abflugbereit, Kurs nach EDJA.
4. Piste 24, Start frei, D-EMAB.
5. Ueber Oscar, verlasse die Kontrollzone Richtung Memmingen, verlasse Frequenz.
6. (optional FIS) München Information, D-EMAB, VFR von Friedrichshafen nach Memmingen, erbitte Fluginformationsdienst.
7. Memmingen Turm, D-EMAB, DV20, suedwestlich des Platzes, 3500 Fuss, Information Bravo, zur Landung, erbitte Einflugverfahren.
8. Gegenanflug Piste 24. → Endanflug Piste 24.
9. Piste 24, Landung frei, D-EMAB.
10. Piste verlassen.

*(Rufzeichen stets phonetisch ausbuchstabiert: „Delta Echo Mike Alfa
Bravo".)*

---

## Vergleich der beiden Demo-Flüge (NfL 1.4.7 a vs. b)

| | Demo 1 — Rundflug | Demo 2 — Überlandflug |
|---|---|---|
| Flugart-Einstellung | Platzrunde | Überlandflug |
| Zielflugplatz | — | EDJA |
| Vorhaben in Rollanfrage | „VFR Rundflug" | „VFR nach EDJA" |
| Startfreigabe | ohne Kurs/Freq-Wechsel | „Kurs nach Plan freigegeben, Frequenzwechsel genehmigt" |
| Startbereit-Hint | „… abflugbereit" | „… abflugbereit, Kurs nach EDJA" |
| Frequenzen | nur EDNY Tower 120.075 | EDNY → (FIS) → EDJA |
| NfL-Anker Roll-Anfrage | ANLAGE 1.4.7 **a)** | ANLAGE 1.4.7 **b)** |
