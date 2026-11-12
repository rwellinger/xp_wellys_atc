# ICAO-Coverage-Matrix — EN/ICAO-VFR-Profil

> **Status:** Phase 1 — Wortlaut-Spalte gegen die ICAO-Primärquellen verifiziert
> (Doc 4444 16. Ed. Kap. 12, Annex 10 Vol II 7. Ed. §5.2, SERA (EU) 923/2012).
> **Review:** Community-Review offen; Korrekturen von Inhabern eines ICAO-VFR-
> Sprechfunkzeugnisses (BZF I / AZF / EFZ) per GitHub-Issue willkommen.
> **Kontext:** Fundament für das EN-Profil-Bundle (#38) und das
> `en_phraseology`-Modul (#41). Das `en/`-Bundle existiert noch nicht — die
> Spalte „Im EN-Profil?" ist daher überwiegend „geplant". Dies ist die
> Authoring-Grundlage, kein Ist-Stand des Codes.

Diese Matrix mappt jeden `PilotIntent` (`src/atc/intent_parser.hpp`) und jede
ATC-Response auf die **englische ICAO-VFR-Standardphrase** samt Quellenstelle.
Sie ist das EN-Pendant zu [`../bzf/bzf_coverage.md`](../bzf/bzf_coverage.md).

## Grundsatz: EN ist keine Übersetzung des NfL

Englischer VFR-Funk folgt einer **eigenständigen** Phraseologie. Die
massgeblichen Phrasen-Templates liegen in **ICAO Doc 4444 (PANS-ATM) Kapitel 12**
— *nicht* in Annex 10 Vol II (das die allgemeinen Verfahrenswörter, das
Buchstabier­alphabet, die Zahlen, die Verständlichkeitsskala und die
Readback-*Delegation* trägt). Doc 9432 (*Manual of Radiotelephony*) ist eine
Trainings-Wiedergabe genau dieser beiden Quellen.

## Referenz-Entscheidung (DACH-neutral)

- **Basis:** ICAO Doc 4444 Kap. 12 (Phrasen) + Annex 10 Vol II §5.2
  (Verfahren) + **SERA** (bindende EU-Rechtsebene, DE/AT; CH via Übernahme).
- **CAP 413** nur als **Illustration**. **UK-Spezifika ausgeschlossen** —
  `FREECALL`, Basic/Traffic/Deconfliction Service, „London Information",
  AFISO-„at your discretion". Wo eine Phrase nur UK-spezifisch existiert, ist
  das in der Zeile markiert.

## Symbol-Legende

- ✓ = ICAO-standardisiert (Doc 4444 / Annex 10 / SERA) — verbindlicher Anker
- ◐ = ICAO regelt nur einen Teil (z. B. Inhalt, nicht Wortlaut)
- ⚠ = **keine** ICAO/SERA-Standardphrase — nationale/AFIS-Konvention
  (im Plugin als Konvention behandeln, nicht als Pflicht)

**Prio:** **K** = kritisch (Sicherheit / Readback-Pflicht), **M** = mittel,
**N** = nice-to-have.

---

## Quellen

| ID | Quelle | Bezug |
|----|--------|-------|
| Q1 | **ICAO Doc 4444 PANS-ATM**, 16. Ed. (2016), **Kap. 12 Phraseologies** — Primärquelle für alle Bewegungs-/Aerodrome-Phrasen | ICAO (kostenpflichtig); zusammengefasst auf SKYbrary. Mirror u. a. bazl.admin.ch `4444_cons_en.pdf` |
| Q2 | **ICAO Annex 10 Vol II**, 7. Ed. (2016, Amdt 91), **§5.2** — Verfahrenswörter, Alphabet, Zahlen, Verständlichkeit, Readback-Delegation, Rufzeichen | ICAO (kostenpflichtig) |
| Q3 | **EASA SERA** — Durchführungsverordnung (EU) Nr. 923/2012, insb. **SERA.8015(e)** (Readback), SERA.14035/14050/14055/14085 (Zahlen/Rufzeichen/RTF), SERA.3225 (Aerodrome-Umgebung) | [EUR-Lex 32012R0923](https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32012R0923); wortgleicher Retained-Law-Mirror legislation.gov.uk |
| Q4 | **UK CAA CAP 413**, Ed. 17 (2008) — praxisnahe VFR-Beispiele, **nur Illustration** (UK-Spezifika ausgeschlossen) | [CAA publicapps](https://publicapps.caa.co.uk/) (frei) |
| Q5 | **EUROCONTROL Manual for AFIS** §2.4 — AFIS = Information, keine Freigabe | EUROCONTROL/SKYbrary (frei) |

> **Wichtige Struktur-Erkenntnis:** ICAOs Phrasen-*Templates* stehen in **Doc
> 4444 Kap. 12**, die Verfahrens-/Wort-Definitionen in **Annex 10 Vol II §5.2**.
> Der Readback-Pflichtumfang ist von Annex 10 an Doc 4444 §4.5.7 delegiert; die
> wortgleiche bindende EU-Fassung ist **SERA.8015(e)**.

---

## 1. Erstanruf & Verbindungsaufnahme

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Im EN-Profil? | Prio |
|---|---|---|---|---|---|
| 1.1 | Zweistufiger Anruf: gerufene Stelle ZUERST, dann eigenes Rufzeichen | Anruf `[station called] [station calling]`; Antwort `[station called] [answering station]` | Q2 §5.2.1.7.3.2.1/.3 · SERA.14055(b)(1) | geplant (#38) `INITIAL_CALL_*` | K |
| 1.2 | Wartepflicht auf Antwort (mit Ausnahme) | Anruf + Antwort abwarten, ausser wenn sicher ist, dass die Gegenstelle empfängt | Q2 §5.2.1.7.3.2.5 | geplant (#38) | M |
| 1.3 | **Pflichtinhalt Erstanruf** | ICAO/SERA verlangen **nur** den zweistelligen Rufzeichen-Austausch + volle Rufzeichen. **Typ / Position / Höhe / Absichten / ATIS sind NICHT ICAO-pflichtig** — sie sind national/lokal vorgeschrieben (die CAP-413-„content order" ist die UK-Operationalisierung). | Q2 §5.2.1.7.3.2.1 · SERA.14055(b) | ◐ — bestätigt den bestehenden `initial_call_conformance`-Befund (Pflicht ≠ Didaktik) | K |
| 1.4 | GROUND-Erstanruf (illustrativ) | „[Aerodrome] Ground, [callsign], [location], **request taxi** for VFR flight to [dest]" | Q4 Kap.4 §1.5 (Illustration) | geplant (#38) `INITIAL_CALL_GROUND` | — |
| 1.5 | TOWER-Join (illustrativ) | „[Aerodrome] Tower, [callsign], **request join**" → ATC „pass your message" | Q4 Kap.4 §1.8.2 (Illustration) | geplant (#38) `INITIAL_CALL_TOWER` | — |
| 1.6 | APPROACH/INBOUND (illustrativ) | „[Aerodrome] Approach, [callsign], request join" → Typ, inbound-from, VFR, Level, QNH, Estimate, „information [X]" | Q4 Kap.6 §1.4.1 (Illustration) | geplant (#38) `INITIAL_CALL_INBOUND` | — |

## 2. Funkprobe & Verständlichkeit

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 2.1 | `RADIO_CHECK` (Pilot) | `[station called] [aircraft ID]` **RADIO CHECK** `[frequency]` | Q2 §5.2.1.8.1 | — |
| 2.2 | Antwort (ATC) | `[aircraft ID] [station] READABILITY [1–5]` | Q2 §5.2.1.8.2 | K |
| 2.3 | Verständlichkeitsskala (verbatim) | 1 **Unreadable**; 2 **Readable now and then**; 3 **Readable but with difficulty**; 4 **Readable**; 5 **Perfectly readable** | Q2 §5.2.1.8.4 | K |
| 2.4 | Alt.: „HOW DO YOU READ" | „What is the readability of my transmission?" | Q2 §5.2.1.5.8 | N |

> DE-Kontrast: NfL „Höre Sie fünf" ↔ ICAO „**Readable 5 / Perfectly readable**".

## 3. Rollanmeldung, Rollfreigabe & Warten

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 3.1 | `REQUEST_TAXI` (Pilot) | „[type] [wake] [location] **REQUEST TAXI** [intentions]" bzw. „…(flight rules) TO (dest) REQUEST TAXI" | Q1 §12.3.4.7(a)/(b) | K |
| 3.2 | Rollfreigabe zum Rollhalt (ATC) | „**TAXI TO HOLDING POINT [number] [RUNWAY (nr)]** [HOLD SHORT OF RUNWAY (nr) / CROSS RUNWAY (nr)] [TIME]" | Q1 §12.3.4.7(c) | K |
| 3.3 | Rollfreigabe via Route | „TAXI TO HOLDING POINT … **VIA (route)** …" | Q1 §12.3.4.7(e) | M |
| 3.4 | Halten generisch (ATC) | „**HOLD POSITION**" / „HOLD (distance) FROM (position)" | Q1 §12.3.4.8(b)/(c) | K |
| 3.5 | **HOLD SHORT** (verlangt explizite Quittung, nicht ROGER/WILCO) | „**HOLD SHORT OF (position)**"; Pilot-Quittung „**HOLDING**" / „**HOLDING SHORT**" | Q1 §12.3.4.8(d)/(e)/(f) | K |
| 3.6 | Konditionelle Freigabe (Reihenfolge-Regel) | Identifikation → Bedingung → Freigabe → **kurze Wiederholung der Bedingung**; z. B. „…, BEHIND DC9 ON SHORT FINAL, **LINE UP BEHIND**"; Pilot „(condition) **LINING UP** (reiteration)" | Q1 §12.2.7 · §12.3.4.10(i)/(j) | K |

> DE-Kontrast: „ROLLEN SIE ZUM ROLLHALT" ↔ „**TAXI TO HOLDING POINT**";
> „HALTEN SIE VOR" ↔ „**HOLD SHORT OF**".

## 4. Aufreihen & Startfreigabe

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 4.1 | Piste aufreihen, auf Start warten (ATC) | „**LINE UP [AND WAIT]**" / „LINE UP RUNWAY (nr)" | Q1 §12.3.4.10(f)/(g) | K |
| 4.2 | Startbereitschaft (ATC-Frage / Pilot) | „**REPORT WHEN READY** [FOR DEPARTURE]" / „ARE YOU READY…" → Pilot „**READY**" | Q1 §12.3.4.10(b)–(e) | — |
| 4.3 | `READY_FOR_DEPARTURE` → Startfreigabe (ATC) | „**RUNWAY (nr) CLEARED FOR TAKE-OFF** [REPORT AIRBORNE]"; mit Verkehr „(traffic) RUNWAY (nr) CLEARED FOR TAKE-OFF" | Q1 §12.3.4.11(a)/(b) | K |
| 4.4 | Start abbrechen (ATC) | „HOLD POSITION, **CANCEL TAKE-OFF I SAY AGAIN CANCEL TAKE-OFF** (reasons)" | Q1 §12.3.4.11(e) | K |
| 4.5 | **„TAKE-OFF"-Wort-Regel** | Das Wort **TAKE-OFF** darf **nur** bei der Startfreigabe oder ihrer Aufhebung benutzt werden; sonst **DEPARTURE / AIRBORNE** | Q1 §7.9.3.3 | K |

> Konsequenz für `en_phraseology`/Templates: Positions-/Bereitschaftsmeldungen
> nutzen „departure", nie „take-off" — anders als die deutsche „Start frei".

## 5. Platzrunde & Positionsmeldungen

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 5.1 | Einflug-/Join-Anruf (Pilot) | „[type] (position) (level) **FOR LANDING**" / „…**INFORMATION (ATIS id) FOR LANDING**" | Q1 §12.3.4.13(a)/(d) | K |
| 5.2 | Join-Anweisung (ATC) | „**JOIN** [(circuit direction)] (position) (runway) [SURFACE] WIND … QNH (nr) [TRAFFIC …]" | Q1 §12.3.4.13(b) | K |
| 5.3 | Geradeausanflug (ATC) | „**MAKE STRAIGHT-IN APPROACH** RUNWAY (nr) …" | Q1 §12.3.4.13(c) | M |
| 5.4 | `REPORT_POSITION_DOWNWIND/BASE/FINAL` (Pilot) | Positionswort im Circuit: „**DOWNWIND**" / „**BASE**" / „**FINAL**" / „**LONG FINAL**" | Q1 §12.3.4.14(a) | — |
| 5.5 | Meldeaufforderung (ATC) | „**REPORT BASE** (or FINAL, or LONG FINAL)" | Q1 §12.3.4.15(c) | — |
| 5.6 | Sequenzierung (ATC) | „**NUMBER … FOLLOW** (aircraft type and position)" | Q1 §12.3.4.14(b) | M |
| 5.7 | Anflug formen (ATC) | „**MAKE SHORT APPROACH**" / „MAKE LONG APPROACH (or **EXTEND DOWNWIND**)" | Q1 §12.3.4.15(a)/(b) | N |
| 5.8 | FINAL/LONG-FINAL-Definition | FINAL bei 7 km (4 NM); LONG FINAL bei Eindrehen jenseits 7 km bzw. Geradeaus­anflug bei 15 km (8 NM) | Q1 §12.3.4.15 Note | Referenz |

> DE-Kontrast: „Nummer eins, melden Sie Endanflug" ↔ „**NUMBER ONE, REPORT
> FINAL**"; „Gegenanflug/Queranflug/Endanflug" ↔ „**DOWNWIND / BASE / FINAL**".

## 6. Landung & Aufsetzen-Durchstarten

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 6.1 | `REQUEST_LANDING` (Pilot) | Kein separates „request landing"-Template — der Einflug-Anruf „(position)(level) **FOR LANDING**" (5.1) erfüllt die Rolle | Q1 §12.3.4.13(a) | — |
| 6.2 | Landefreigabe (ATC) | „**RUNWAY (nr) CLEARED TO LAND**"; mit Verkehr „(traffic) RUNWAY (nr) CLEARED TO LAND" | Q1 §12.3.4.16(a)/(b) | K |
| 6.3 | `REQUEST_TOUCH_AND_GO` (ATC-Freigabe) | „**CLEARED TOUCH AND GO**" | Q1 §12.3.4.16(c) | K |
| 6.4 | Vollstopp (ATC) | „**MAKE FULL STOP**" | Q1 §12.3.4.16(d) | N |
| 6.5 | Piste belegt / noch nicht frei (ATC) | „**CONTINUE APPROACH** [PREPARE FOR POSSIBLE GO AROUND]" | Q1 §12.3.4.15(d) | K |

> DE-Kontrast: „Landung frei" ↔ „**CLEARED TO LAND**"; „frei zum Aufsetzen und
> Durchstarten" ↔ „**CLEARED TOUCH AND GO**"; „weiter Anflug, Verkehr auf der
> Piste" ↔ „**CONTINUE APPROACH**".

## 7. Durchstarten (Go-Around)

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 7.1 | `GO_AROUND` — ATC weist an | „**GO AROUND**" | Q1 §12.3.4.18(a) | K |
| 7.2 | Pilot meldet | „**GOING AROUND**" | Q1 §12.3.4.18(b) | K |

> Keine Lücke — ICAO kodifiziert beide Richtungen.

## 8. Piste verlassen & Übergabe

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 8.1 | `RUNWAY_VACATED` (Pilot) | „**RUNWAY VACATED**" — gemeldet, wenn das ganze Luftfahrzeug jenseits des Runway-Holding-Position ist | Q1 §12.3.4.9(e) + Note | K |
| 8.2 | Kreuzen + melden (ATC) | „**CROSS RUNWAY (nr)** [REPORT VACATED]" | Q1 §12.3.4.9(b) | M |
| 8.3 | Post-Landing-Übergabe (ATC) | „**CONTACT GROUND (freq)**" / „**WHEN VACATED CONTACT GROUND (freq)**" | Q1 §12.3.4.20(a)/(b) | K |

## 9. Frequenzwechsel

| # | Intent / Element | EN-Standardphrase (Anker) | ICAO | Prio |
|---|---|---|---|---|
| 9.1 | Übergabe (ATC) | „**CONTACT (unit) (freq) [NOW]**" | Q1 §12.3.1.4(a) | K |
| 9.2 | Konditionale Übergabe | „AT/OVER (time/place) [or WHEN PASSING/LEAVING/REACHING (level)] CONTACT (unit) (freq)" | Q1 §12.3.1.4(b) | M |
| 9.3 | `REQUEST_FREQUENCY` (Pilot) | „**REQUEST CHANGE TO (freq)**" → ATC „**FREQUENCY CHANGE APPROVED**" | Q1 §12.3.1.4(e)/(f) | — |
| 9.4 | Nur mithören (ATC) | „**MONITOR (unit) (freq)**" → Pilot „**MONITORING (freq)**" | Q1 §12.3.1.4(g)/(h) | N |
| 9.5 | `LEAVING_FREQUENCY` (Pilot) | ⚠ **Keine feste ICAO-Phrase.** Annex 10 §5.2.2.6.2: der Pilot „shall transmit such information as may be prescribed by the appropriate Authority" — Wortlaut lokal offen | Q2 §5.2.2.6.1/.2 | ⚠ |
| 9.6 | Readback des Frequenzwechsels **pflichtig** | z. B. „[unit] 129.125 [callsign]" | SERA.8015(e)(1)(iii) · Q1 §4.5.7 | K |

> ⚠ **`FREECALL` ist NICHT ICAO-Standard** (UK-Spezifikum, laut CAP-413-eigener
> Differenztabelle) → im EN-Profil **nicht** verwenden. Pilot-initiierter
> Wechsel = „**REQUEST CHANGE TO (freq)**".

## 10. Readback-Pflichtumfang (ICAO / SERA)

Annex 10 Vol II zählt die Liste **nicht selbst** auf — es delegiert an Doc 4444
§4.5.7 (Q2 §5.2.1.9.2.2). Die wortgleiche bindende EU-Fassung ist **SERA.8015(e)**.

| # | Element | Wortlaut-Anker | Quelle | Prio |
|---|---|---|---|---|
| 10.1 | **Immer voll zurückzulesen** | (i) ATC-Streckenfreigaben; (ii) Freigaben/Anweisungen zu **enter / land on / take off from / hold short of / cross / taxi / backtrack** auf *jeder* Piste; (iii) **runway-in-use, altimeter settings (QNH), SSR codes, newly assigned frequencies, level, heading, speed**; (iv) **transition levels** | SERA.8015(e)(1) · Q1 §4.5.7 | K |
| 10.2 | Übrige Freigaben (inkl. konditionale + Taxi) | „shall be read back **or acknowledged** in a manner to clearly indicate that they have been understood and will be complied with" | SERA.8015(e)(2) | K |
| 10.3 | Controller-Pflicht | „shall listen to the read-back … and shall take immediate action to correct any discrepancies" | SERA.8015(e)(3) | K |

> DE-Kontrast: NfL §25 b) Nr. 1 (Piste/QNH/Frequenz/Squawk/Rufzeichen) ↔ die
> breitere ICAO/SERA-Liste oben. **Entscheidung für ein EN-Konformitäts-Pendant
> zum `bzf_strict_mode` → Issue #42** (die ICAO-Liste ist der natürliche Anker).

## 11. Zahlen, Alphabet & Rufzeichen

| # | Element | Wortlaut-Anker | Quelle |
|---|---|---|---|
| 11.1 | Zahlenaussprache | 0 **ZE-RO**, 1 **WUN**, 2 **TOO**, 3 **TREE**, 4 **FOW-er**, 5 **FIFE**, 6 **SIX**, 7 **SEV-en**, 8 **AIT**, 9 **NIN-er**, Decimal **DAY-SEE-MAL**, Hundred **HUN-dred**, Thousand **TOU-SAND** | Q2 §5.2.1.4.3.1 |
| 11.2 | Ziffernweise-Regel | Zahlen werden ziffernweise gesprochen (Ausnahmen §5.2.1.4.1.2–.1.6) | Q2 §5.2.1.4.1.1 · SERA.14035 |
| 11.3 | Buchstabieralphabet | A **Alfa** … J **Juliett** (Doppel-t) … Z **Zulu** | Q2 Fig. 5-1 (§5.2.1.3) |
| 11.4 | Volles Rufzeichen zum Aufbau | „Full radiotelephony call signs shall always be used when establishing communication" | Q2 §5.2.1.7.3.2.1 · SERA.14055(b)(1) |
| 11.5 | **Rufzeichen-Abkürzung** | Nur nach zufriedenstellend aufgebauter Verbindung **und** wenn keine Verwechslung droht; Luftfahrzeug darf abgekürzt **erst, nachdem die Bodenstelle es so angesprochen hat** | Q2 §5.2.1.7.3.3.1 · SERA.14055(c)(1) |
| 11.6 | Abgekürzte Form (Registrierung) | erstes Zeichen + mindestens die letzten zwei Zeichen des Rufzeichens | SERA.14050 |

> Für `en_phraseology` (#41): ICAO-NATO (**Juliett**, einfach-t ist BZF —
> ICAO nutzt Doppel-t) + ICAO-Ziffern (**tree/fower/fife/niner**) statt der
> BZF-Ziffern (`zwo`). Die Rufzeichen-Abkürzungs-Regel (11.5) ist EN-eigen und
> hat kein direktes NfL-Pendant.

## 12. Standard-Wörter & -Wendungen (Annex 10 Vol II §5.2.1.5.8, verbatim)

| Wort | ICAO-Bedeutung |
|---|---|
| **AFFIRM** | „Yes." |
| **NEGATIVE** | „'No' / 'Permission not granted' / 'That is not correct' / 'Not capable'." |
| **WILCO** | „(will comply) I understand your message and will comply with it." |
| **ROGER** | „I have received all of your last transmission." (Nicht als Antwort auf eine Frage, die READ BACK oder AFFIRM/NEGATIVE verlangt.) |
| **STANDBY** | „Wait and I will call you." (Keine Genehmigung/Ablehnung.) |
| **SAY AGAIN** | „Repeat all, or the following part, of your last transmission." |
| **I SAY AGAIN** | „I repeat for clarity or emphasis." |
| **CORRECTION** | „An error has been made … The correct version is …" |
| **READ BACK** | „Repeat all, or the specified part, of this message back to me exactly as received." |
| **WORDS TWICE** | „Communication is difficult. Please send every word … twice." |

Korrektur-/Wiederhol-Verfahren: `CORRECTION` (Q2 §5.2.1.9.4.1); `CORRECTION,
I SAY AGAIN` (ganze Nachricht, §5.2.1.9.4.2); `SAY AGAIN ALL BEFORE/AFTER …`
(§5.2.1.9.4.5); **`NEGATIVE I SAY AGAIN`** zum Zurückweisen eines falschen
Readbacks (§5.2.1.9.4.7).

> DE-Kontrast: `WIEDERHOLEN SIE` ↔ **SAY AGAIN**; `BESTÄTIGEN SIE` ↔
> **ACKNOWLEDGE/CONFIRM**; `NEGATIV, ich wiederhole` ↔ **NEGATIVE I SAY AGAIN**.

## 13. Unkontrollierter Flugplatz — Selbstansage / Blindmeldung

| # | Element | Wortlaut / Regel | Quelle |
|---|---|---|---|
| 13.1 | Pflichten in Flugplatz-Umgebung | Verkehr beobachten/meiden, Platzrunde einhalten, Linkskurven, gegen den Wind starten/landen — **keine** Funk-Broadcast-Pflicht in der Regel selbst | SERA.3225(a)–(d) |
| 13.2 | `SELF_ANNOUNCE` (Format) | ⚠ „[Aerodrome] **Traffic**, [callsign], [position], [intentions]" | national/AFIS-Konvention (SKYbrary; UK/CA/AU/FAA AIM 4-1-9) |
| 13.3 | AFIS = Information, keine Freigabe | „The AFIS unit is not an air traffic control unit … AFISOs shall only pass information and warnings." | Q5 §2.4 |
| 13.4 | Blindmeldung (eng gefasst) | „**TRANSMITTING BLIND** …" — nur bei Kommunikations-/Empfängerausfall, **nicht** als Routine-Selbstansage | SERA.14085 |

> ⚠ **Keine ICAO/SERA-Standard-Selbstansage.** Wie im DE-Profil die NfL-„5
> Pflichtelemente" als Didaktik behandelt werden, ist die „[Aerodrome]
> Traffic, …"-Ansage als **Konvention** zu modellieren, nicht als Pflicht.

---

## Lücken / nicht aus ICAO-Primärquelle belegbar

Aus der Quellrecherche explizit markiert — für das EN-Bundle (#38) / #42 relevant:

1. **ATIS-Quittung** („information X received") — **keine** kodifizierte Doc-4444-
   Phrase; nur Konvention (CAP 413). Volltextsuche bestätigte die Abwesenheit.
   Doc 4444 §12.3.4.13(d) kodifiziert aber das **Mitführen** des ATIS-Buchstabens
   im Einflug-Anruf („… INFORMATION (ATIS id) FOR LANDING").
2. **Readback-Liste (10.x)** — exakter **Doc 4444 §4.5.7**-Primärwortlaut nicht aus
   einem ICAO-PDF gezogen; Annex 10 bestätigt Doc 4444 als Autorität, **SERA.8015(e)**
   ist die wortgetreue bindende Umsetzung → beide zitiert.
3. **Selbstansage (13.2)** — keine ICAO/SERA-Standardphrase; nur Konvention.
4. **`LEAVING_FREQUENCY` / `FREECALL` (9.5)** — FREECALL UK-spezifisch
   (ausgeschlossen); ICAO hat keine feste „leaving frequency"-Phrase
   (Annex 10 §5.2.2.6.2 überlässt es der lokalen Behörde).
5. **Doc 9432** (Manual of Radiotelephony) — nicht separat beschafft; Inhalt ist
   eine Trainings-Wiedergabe von Doc 4444 Kap. 12 + Annex 10 Vol II (den
   zugrunde liegenden Autoritäten).

**Leitlinie fürs EN-Profil:** Doc 4444 Kap. 12 = Phrasen-Rückgrat; Annex 10 Vol II
§5.2 = Verfahrenswörter/Zahlen/Readback-Delegation; SERA.8015(e)/14xxx = bindende
EU-Ebene. Punkte 1 und 3 haben **keine** ICAO-Phrase und sind als Konvention zu
modellieren — analog zur Behandlung der NfL-„5 Pflichtelemente" als Didaktik im
DE-Profil.
