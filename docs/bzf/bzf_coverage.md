# BZF-Coverage-Matrix — DE/BZF-Profil

> **Status:** Phase 1 abgeschlossen — Wortlaut-Spalte gegen die NfL Sprechfunk 2024 verifiziert.
> **Review:** Community-Review offen, BZF-II-Inhaber per GitHub-Issue gesucht (siehe README).
> **Letzte Änderung:** 2026-06-05 · Autor: thWelly + Claude Code

Diese Matrix erfasst BZF-II-relevante Pflichtelemente der deutschen VFR-Sprechfunkprozedur
und mappt jedes Element gegen den aktuellen Stand des `de`-Profils dieses Plugins
(`data/atc_profiles/de/{intent_rules,atc_templates,phraseology_hints,flight_rules,ui_strings}.json`
und `src/atc/de_phraseology.{hpp,cpp}`).

**Primärer Wortlaut-Anker:** [DFS NfL Sprechfunk 2024](dfs_nfl_sprechfunk_2024.txt) (NfL 2024-1-3266,
gültig ab 28.11.2024). Sekundär: [BNetzA-Prüfungsfragen 2024](bnetza_pruefungsfragen_2024.txt)
und [NfL Funk Teil B 2010](nfl_funk_teilb_2010.txt). Zitierter Wortlaut stammt unverändert aus
diesen Quellen.

**Symbol-Legende:**
- ✓ = im Profil abgedeckt (Wortlaut entspricht oder ist nahe am NfL-Standard)
- ◐ = teilweise abgedeckt (Funktion vorhanden, aber Wortlaut weicht ab oder Pipeline-Aktivierung unklar)
- ✗ = fehlt komplett

**Prio-Legende:** **K** = kritisch (Sicherheit / NfL-Pflicht), **M** = mittel, **N** = nice-to-have.
**Bucket-Legende:** **A** = Engine-Capability (Phase 2 Kern), **B** = Data-only-Gap (sofort shippable als Content-PR), **C** = Pipeline-Verify.

---

## Wortlaut-Klarstellungen vorab (NfL-belegt)

Mehrere im Sprachgebrauch verbreitete Phrasen sind **nicht** NfL-Wortlaut. Diese Liste ist die
Grundlage für die Korrekturen in Bucket B:

| Verbreitet / im Profil | NfL-konform | NfL-Anker |
|---|---|---|
| „Lesbarkeit" | **„Verständlichkeit"** / „Verständlichkeitsskala" | §17 c) |
| „Sie sind laut und deutlich" | **„Höre Sie X"** (X = 1–5) | §17 b); BNetzA Q103/104 |
| „Piste frei" | **„PISTE VERLASSEN" / „RUNWAY VACATED"** | ANLAGE 1.4.7 *z), 1.4.9 *f) |
| „STARTBEREIT" | **„[ABFLUG-] BEREIT" / „READY"** | ANLAGE 1.4.10 *e) |
| „Sagen Sie nochmals" | **„WIEDERHOLEN SIE"** | §12 c), §18 c) Nr. 4 |
| „Unbekannter Verkehr" als Prefix | Klassifikation **„UNBEKANNT"** steht NACH der Position | ANLAGE 2.1.8 a) 1) |
| „zur Landung" als Suffix immer | Pflicht **nur** im JOIN-Anruf (ANLAGE 1.4.13), NICHT in inner-Pattern-Meldungen | ANLAGE 1.4.13/14 |
| „QNH X Hektopascal" | „Hektopascal" ist im NfL **nicht** Pflicht-Suffix | §10 a) Nr. 1 ii) |
| „Alpha" (mit ph) | **„ALFA"** (mit f) | §6 |
| „zwei" als Ziffer | **„ZWO"** (einzig zulässig) | §11 |

---

## Quellen

| ID | Quelle | Pfad / URL |
|----|--------|-----------|
| Q1 | **DFS NfL Sprechfunk 2024** (primär) — NfL 2024-1-3266, gültig ab 28.11.2024 | [`docs/bzf/dfs_nfl_sprechfunk_2024.txt`](dfs_nfl_sprechfunk_2024.txt) — Original: [DFS PDF](https://www.dfs.de/homepage/de/medien/ifr-vfr-informationen/ifr-informationen/neues-sprechfunk-nfl-2024-1-3266/sprechfunk24.pdf) |
| Q2 | **BNetzA-Prüfungsfragen BZF-II/I 2024** — Antwort-Vorlagen | [`docs/bzf/bnetza_pruefungsfragen_2024.txt`](bnetza_pruefungsfragen_2024.txt) — Original: [BNetzA PDF](https://www.bundesnetzagentur.de/SharedDocs/Downloads/DE/Sachgebiete/Telekommunikation/Unternehmen_Institutionen/Frequenzen/Funkzeugnisse/Flugfunkzeugnisse/2024Pruefungsfragen_BZFII_BZFI_pdf.pdf) |
| Q3 | **NfL Funk Teil B 2010** — historische Erläuterungen | [`docs/bzf/nfl_funk_teilb_2010.txt`](nfl_funk_teilb_2010.txt) |

Sekundärquellen (für Beispielmaterial, nicht als primärer Anker):
VATSIM Germany PHR02-Modul, bzf-kurs.de, bzf-lehrgang.de, Wikipedia ICAO-Alphabet.

---

## 1. Erstanruf & Verbindungsaufnahme

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 1.1 | Zweistufiger Erstanruf: gerufene Stelle ZUERST, dann eigenes Rufzeichen, dann Wartephase auf Antwort | §14 b) Nr. 1–3: „Beim Herstellen der Verbindung haben Luftfahrzeuge ihren Anruf mit der Bezeichnung der gerufenen Funkstelle zu beginnen, gefolgt von der Bezeichnung der rufenden Funkstelle." | ✓ | `intent_rules.json:362` INITIAL_CALL_TOWER; Template `atc_templates.json:16` | — |
| 1.2 | Vollständiger Erstanruf (TOWER): Bezeichnung gerufene Stelle + Rufzeichen + Standort + national: Flughöhe (CTR) / angeflogene Piste (Anflug) | §16 c) Nr. 1–4 + nationale Ergänzung: „Flüge durch Kontrollzonen haben die Flughöhe zu melden. Anflüge haben die angeflogene Piste zu nennen." | ◐ | `intent_rules.json:337` INITIAL_CALL_GROUND; Template `atc_templates.json:11` | **A** / K — Pflichtinhalt-Check fehlt |
| 1.3 | Erstanruf GROUND/DELIVERY: gerufene Stelle (Rollkontrolle / Anlasskontrolle) + Rufzeichen + Standort + ATIS-Letter + Anliegen. **Konformitäts-Sets (NfL-verankert, `bzf_strict_mode`):** `required` = ∅ für den Erstkontakt (das Roll-Anliegen ERBITTE ROLLEN ist der separate `REQUEST_TAXI`-Sprechakt); `recommended` = Luftfahrzeugmuster / Standort / Absichten / ATIS — alle NfL-[optional] | **Primärquelle** NfL §1.4.7 *a): „[Luftfahrzeugtyp] [Standort] ERBITTE ROLLEN [Absichten]" — nur ERBITTE ROLLEN unbeklammert (= zwingend), Rest [optional]; ATIS-Quittierung §1.4.3 *a)/*b) ebenfalls optional. Die 5-Elemente-Liste ist BZF-Didaktik (Q1), **kein** NfL-Pflichtumfang | ✓ | `initial_call_conformance` + `data/atc_profiles/de/conformance.json`; `intent_rules.json:243` INITIAL_CALL_GROUND | Hint NfL-konform geprüft ✓ (`flight_rules.json` pilot_phraseology, H2-Test). **Erste primärquellen-verankerte `required`/`recommended`-Zeile** |
| 1.4 | Erstanruf an Anflugkontrolle | §34 b) (2): „Anflugkontrollstelle – APPROACH – ANFLUGKONTROLLE" | ✓ | `intent_rules.json:319` INITIAL_CALL_APPROACH; Template `atc_templates.json:61, 366, 427` | — |
| 1.5 | Inbound-JOIN-Anruf von außerhalb: [Luftfahrzeugtyp] + Position + Flughöhe + [INFORMATION (Letter)] + „ZUR LANDUNG" | ANLAGE 1.4.13 *a)/*e): „*a) [Luftfahrzeugtyp] (Position) (Flughöhe) ZUR LANDUNG" / „*e) (Luftfahrzeugtyp) (Position) (Flughöhe) INFORMATION (ATIS-Kennbuchstabe) ZUR LANDUNG" | ◐ | `intent_rules.json:350` INITIAL_CALL_INBOUND; Template `atc_templates.json:21, 432` | **A** / K — Pflichtelement-Check (Typ/Position/Höhe/ATIS) fehlt |
| 1.6 | ATIS-Information im Erstanruf nennen (wenn ATIS vorhanden) | ANLAGE 1.4.3 *a)/*b) + 1.4.13 *e) — NfL bietet zwei gleichwertige Formen (*a) ohne, *b) mit ATIS) → **optional**, nicht zwingend | ◐ | `recommended`-Element `atis_letter` in `conformance.json`; im Trainer (`bzf_strict_mode`) geprüft via `initial_call_conformance` | **A** / K → für GROUND-Erstanruf erledigt (strict-only, NfL-optional). Detektor: Anker „Information" + NATO-Buchstabe |

## 2. Funkprobe & Verständlichkeit

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 2.1 | Pilot-Aufforderung: gerufene Stelle + Rufzeichen + „RADIO CHECK" + Frequenz | §17 a) Nr. 1–4. Alternative aus §12 c): „HOW DO YOU READ / WIE VERSTEHEN SIE MICH" | ✓ | `intent_rules.json:63` RADIO_CHECK | — |
| 2.2 | Lotsen-Antwort: Rufzeichen Pilot + Rufzeichen Bodenstelle + Verständlichkeitsangabe (Skala 1–5). BNetzA-Vorlage: **„Höre Sie X"** | §17 b) Nr. 1–3; BNetzA Q103/104 | ✗ | `atc_templates.json:56`: `"Sie sind laut und deutlich."` | **B** / K — Audit-Lücke. Wortlaut auf „Höre Sie X" mit Verständlichkeitswert umstellen |
| 2.3 | Verständlichkeitsskala 1–5: 1=unverständlich, 2=zeitweise verständlich, 3=schwer verständlich, 4=verständlich, 5=sehr gut verständlich | §17 c) | ✗ | — | **B** / K — Tabelle als Konstante hinterlegen; ATC kann zufallsbasiert oder kanalbasiert Wert wählen |

## 3. Rollanmeldung & Rollfreigabe

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 3.1 | Pilot-Rollanmeldung: [Typ] + [Standort] + [Flugregeln] + [Ziel] + „ERBITTE ROLLEN" + [Absichten] | ANLAGE 1.4.7 *a)/*b)/*q): „*a) [Luftfahrzeugtyp] [Standort] ERBITTE ROLLEN [Absichten]" | ◐ | `intent_rules.json:238` REQUEST_TAXI Keywords: „rollen", „rollfreigabe", „erbitte rollen" | **A** / K — Audit-Lücke. „Rolle für Piste …" ist umgangssprachlich; NfL-Pflicht ist „ERBITTE ROLLEN" + Pflichtelemente |
| 3.2 | Lotsen-Rollfreigabe: ROLLEN SIE → ROLLHALT → PISTE → [ÜBER (Strecke)] → [HALTEN SIE VOR / ÜBERQUEREN SIE] | ANLAGE 1.4.7 c)/e): „c) ROLLEN SIE ZUM ROLLHALT [Nummer] [PISTE (Nummer)] …"; bei fehlendem ATIS zusätzlich PISTE/WIND/QNH/TEMPERATUR/SICHT (ANLAGE 1.4.6 d)) | ✓ | Template `atc_templates.json:11, 31, 83` | — |
| 3.3 | Post-Landing-Rolle zur Abstellposition: „ROLLEN SIE ZUM TERMINAL / VORFELD" + Abstellplatz | ANLAGE 1.4.7 *n)/o)/p)/j); ANLAGE 1.4.20 d) | ✓ | `intent_rules.json:206` REQUEST_TAXI_PARKING; Template `atc_templates.json:36` | — |
| 3.4 | Konditionelle Rollanweisungen (BEHIND / HINTER) — Pflicht-Wiederholung der Bedingung im Readback | §25 d) Pflichtreihenfolge: Rufzeichen → Bedingung → Freigabe → kurze Wiederholung | ◐ | `intent_rules.json:98` READBACK | **A** / K — Konformitäts-Check für Konditionale-Readback fehlt |

## 4. Start & Startfreigabe

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 4.1 | Pilot „[ABFLUG-] BEREIT" | ANLAGE 1.4.10 *e): „*e) [ABFLUG-] BEREIT" / „READY" | ◐ | `intent_rules.json:188` READY_FOR_DEPARTURE Keywords: „bereit", „abflugbereit", **„startbereit"** | **B** / M — „startbereit" akzeptieren ist tolerant; sollte für Strict-Mode entfernt werden (kein NfL-Wortlaut) |
| 4.2 | Lotsen-Startfreigabe: „PISTE (Nummer) START FREI [MELDEN SIE ABGEHOBEN]" | ANLAGE 1.4.11 a): „a) PISTE (Nummer) START FREI [MELDEN SIE ABGEHOBEN]" | ◐ | Template `atc_templates.json:46, 51, 127, 132`: `"Piste {runway}, Start frei, Wind {wind}, ..."` | **B** / N — Wind wird per Plugin-Konvention immer mitgegeben; NfL 1.4.11 a) listet Wind NICHT als Pflicht (kommt aus 1.4.6 d) bei fehlendem ATIS). Akzeptabel. |
| 4.3 | Bedingte Startfreigabe (BEHIND LANDING) — Pflicht-Wiederholung der Bedingung | §25 d) (siehe 3.4); ANLAGE 1.4.10 ‡i) | ✗ | — | **A** / M — Konditionelle Startfreigaben nicht modelliert |

## 5. Platzrunde — Pflichtmeldungen

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 5.1 | Gegenanflug-Meldung — „GEGENANFLUG" (downwind) | ANLAGE 1.4.14 *a) + 1.4.15 c) | ✓ | `intent_rules.json:258` REPORT_POSITION_DOWNWIND | — |
| 5.2 | Queranflug-Meldung — „QUERANFLUG" (base) | ANLAGE 1.4.15 c): „c) MELDEN SIE QUERANFLUG (oder ENDANFLUG, oder LANGEN ENDANFLUG)" | ✓ | `intent_rules.json:264` REPORT_POSITION_BASE | — |
| 5.3 | Endanflug-Meldung — „ENDANFLUG" (final) | ANLAGE 1.4.15 c) | ✓ | `intent_rules.json:270` REPORT_POSITION_FINAL | — |
| 5.4 | „LANGER ENDANFLUG" ab > 4 NM bzw. Geradeausanflug > 8 NM | ANLAGE 1.4.15 Anmerkung | ✗ | — | **B** / N — separater Intent / Template wäre nice-to-have |
| 5.5 | Pflicht-Suffix „ZUR LANDUNG" — gilt **nur** für 1.4.13 (JOIN-Anruf), NICHT für inner-Pattern-Meldungen | ANLAGE 1.4.13/14 | ◐ | siehe 1.5 | (kein Gap — Klarstellung der ursprünglichen Annahme) |
| 5.6 | Lotsen-Antwort „weiter Anflug" / SETZEN SIE ANFLUG FORT | ANLAGE 1.4.15 d): „d) SETZEN SIE ANFLUG FORT [BEREITEN SIE SICH AUF MÖGLICHEN FEHLANFLUG VOR]" | ✓ | Template `atc_templates.json:174, 179, 228, 233` | — |

## 6. Anflug & Landung

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 6.1 | Inbound-Anruf von außen | siehe 1.5 | ◐ | siehe 1.5 | **A** / K |
| 6.2 | Lotsen-Landefreigabe: „PISTE (Nummer), LANDUNG FREI" | ANLAGE 1.4.16 a) | ✓ | Template `atc_templates.json:184, 238, 280, 285`: `"... Landung frei, Wind {wind}."` | — |
| 6.3 | Lotsen-Touch-and-Go-Freigabe: „FREI ZUM AUFSETZEN UND DURCHSTARTEN" | ANLAGE 1.4.16 c) | ✗ | Template `atc_templates.json:152, 248, 290`: `"... Touch-and-Go frei, Wind {wind}."` | **B** / K — Audit-Lücke. „Touch-and-Go" ist englisch im DE-Profil; NfL-konform ist „FREI ZUM AUFSETZEN UND DURCHSTARTEN" |
| 6.4 | Pilot-Anfrage Touch-and-Go: „AUFSETZEN UND DURCHSTARTEN" | folgt 1.4.16 c) | ✗ | `intent_rules.json:198` REQUEST_TOUCH_AND_GO Keywords: „durchstartlandung", „durchstart und landung" | **B** / K — Audit-Lücke. Exakter Wortlaut „aufsetzen und durchstarten" fehlt in Keywords |
| 6.5 | Abschlusslandung: „MACHEN SIE ABSCHLUSSLANDUNG" / „MAKE FULL STOP" | ANLAGE 1.4.16 d) | ✗ | — | **B** / N |
| 6.6 | Go-Around — Lotse „STARTEN SIE DURCH", Pilot „STARTE DURCH" | ANLAGE 1.4.18 a)/*b) | ✓ | `intent_rules.json:64` GO_AROUND | — |

## 7. Piste verlassen / nach Landung

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 7.1 | Pilot-Meldung: „PISTE VERLASSEN" (NICHT „Piste frei") | ANLAGE 1.4.7 *z), 1.4.9 *f); Anmerkung 1.4.9: „… wenn sich das ganze Luftfahrzeug hinter dem betreffenden Rollhalt befindet." | ◐ | `intent_rules.json:221` RUNWAY_VACATED Keywords: „piste frei", **„piste verlassen"**, „piste geräumt" | **B** / N — „piste verlassen" ist drin; „piste frei" sollte für Strict-Mode entfernt werden (kein NfL-Wortlaut) |
| 7.2 | Lotsen-Aufforderung: „VERLASSEN SIE PISTE" + Richtung (ERSTE / ZWEITE / PASSENDE LINKS / RECHTS) | ANLAGE 1.4.20 e) | ✗ | — | **B** / M — Richtungs-Wortlaut fehlt im Template |
| 7.3 | Kontakt-Boden-Anweisung: „RUFEN SIE ROLLKONTROLLE (Frequenz)" | ANLAGE 1.4.20 a)/b) | ✓ | Template `atc_templates.json:66, 295, 322`: `"... kontaktieren Sie Rollkontrolle auf {ground_frequency}, auf Wiederhoeren."` | — |

## 8. Verkehrsmeldung (Traffic-Advisory)

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 8.1 | Lotsen-Wortlaut: „VERKEHR (Zahl) UHR (Entfernung) (Flugrichtung) [Klassifikation 1–11]" — Klassifikation steht **NACH** Position | ANLAGE 2.1.8 a); ANLAGE 1.1.7 f); ANLAGE 1.4.14 c); ANLAGE 1.4.19 h) | ◐ | Template `atc_templates.json:379`: `"{callsign}, Verkehr, {clock} Uhr, {distance} Meilen, {direction}, {altitude_info}, {type}."` | **B** / M — Format-Reihenfolge stimmt; Klassifikationen 1–11 sind nicht systematisch abgedeckt. „Gleiche Höhe" ist Klassifikation 5 (selbe Richtung) — verifizieren ob `{direction}` das korrekt abbildet |
| 8.2 | Klassifikation „UNBEKANNT" (steht NACH der Position, nicht als Prefix) | ANLAGE 2.1.8 a) 1) | ✗ | — | (Audit-Annahme korrigiert: „Unbekannter Verkehr"-Prefix existiert in NfL nicht — kein Gap, sondern Klarstellung) |
| 8.3 | Pilot-Antwort: „HALTE AUSSCHAU" / „VERKEHR IN SICHT" / „KEIN [SICHT-] KONTAKT [Gründe]" | ANLAGE 1.1.7 *c)/*d)/*e); ANLAGE 1.4.7 *u)/*v) | ✓ | `intent_rules.json:85, 90, 387` TRAFFIC_IN_SIGHT / TRAFFIC_NEGATIVE_CONTACT / TRAFFIC_LOOKING | — |
| 8.4 | Avoiding-Action: „DREHEN SIE SOFORT NACH … STEUERKURS … UM UNBEKANNTEM VERKEHR AUSZUWEICHEN" | ANLAGE 2.1.8 e)/f) | ✗ | — | **B** / N — selten in VFR-Praxis, aber NfL-Pflichtphrase |

## 9. Notfall — MAYDAY & PAN-PAN

> **Hinweis:** Dieser Block ist Dokumentation der NfL-Pflichten zur Vollständigkeit der Matrix.
> **MAYDAY/PAN-PAN sind explizit AUSSERHALB des M11-MVP-Scopes** — siehe Block „MAYDAY-Move"
> weiter unten. Realisierung als separate Milestone (Phase 4 „Notfall-Sequenz" im M11-Plan
> oder eigener Milestone M-X).

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 9.1 | MAYDAY (vorzugsweise 3x) am Beginn der ersten Notmeldung | §22 a) Nr. 2; §22 b) Nr. 1 | ✗ | — | _separate Milestone_ |
| 9.2 | MAYDAY-Pflichtinhalte (Reihenfolge A–E): A) gerufene Stelle, B) Rufzeichen, C) Art der Notlage, D) Absicht, E) Standort/Höhe/Kurs | §22 b) Nr. 1 ii) | ✗ | — | _separate Milestone_ |
| 9.3 | PAN PAN (vorzugsweise 3x) — französisch „panne" ausgesprochen | §22 c) Nr. 1 | ✗ | — | _separate Milestone_ |
| 9.4 | PAN PAN MEDICAL (Sanitätstransport) — „MAY-DEE-CAL" | §22 c) Nr. 4 ii) | ✗ | — | _separate Milestone_ |
| 9.5 | „NOTVERKEHR BEENDET" / „DISTRESS TRAFFIC ENDED" | §22 b) Nr. 5 iii) | ✗ | — | _separate Milestone_ |
| 9.6 | Notfrequenz 121,5 MHz | §19 a) Nr. 1; §22 a) Nr. 5 | ✗ | — | _separate Milestone_ |
| 9.7 | „SQUAWK MAYDAY [SIEBEN-SIEBEN-NULL-NULL]" | ANLAGE 2.3.9 | ✗ | — | _separate Milestone_ |
| 9.8 | Vorrangordnung der Meldungsarten: MAYDAY → PAN PAN → Peilfunk → Flugsicherheit → Wetter → Flugbetrieb | §3 a) | ✗ | — | _separate Milestone_ |

## 10. Frequenzwechsel & Abmeldung

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 10.1 | Lotsen-Aufforderung: „CONTACT (Stelle) (Frequenz)" / „RUFEN SIE (Stelle) (Frequenz)" — ohne Aufforderung muss Pilot vorab informieren | §15 a) + §12 c) „CONTACT/RUFEN SIE" | ✓ | Template `atc_templates.json:66, 295, 322, 477` | — |
| 10.2 | Lotsen-Bestätigung Frequenzwechsel: „VERSTANDEN, VERLASSEN DER FREQUENZ/KANAL GENEHMIGT" | ANLAGE 7.5 b) | ◐ | Template `atc_templates.json:206, 211, 477`: `"{callsign}, auf Wiederhoeren."` bzw. `"... Frequenzwechsel genehmigt, auf Wiederhoeren."` | **B** / M — „auf Wiederhoeren" ist OK als Gesprächsende (§18 b)), aber expliziter NfL-Wortlaut „Verlassen der Frequenz genehmigt" für Strict-Mode |
| 10.3 | Pilot-Abmeldung: „VERLASSE FREQUENZ/KANAL" | ANLAGE 7.5 *c); §18 b) Gesprächsbeendigung mit Rufzeichen | ✓ | `intent_rules.json:68` LEAVING_FREQUENCY | — |
| 10.4 | Pilot-Anfrage Frequenzwechsel: „ERBITTE FREQUENZWECHSEL" (kein normativer NfL-Wortlaut — folgt REQUEST-Schema) | §12 c) „REQUEST / ERBITTE" | ✓ | `intent_rules.json:304` REQUEST_FREQUENCY | — |
| 10.5 | Erstanruf NACH Frequenzwechsel — Flughöhe ist Pflicht (national: „NON RNAV" oder „FORMATION" zusätzlich falls zutreffend) | §16 a) + nationale Ergänzung | ◐ | `intent_rules.json:319` INITIAL_CALL_APPROACH | **A** / M — Flughöhen-Check fehlt bei post-handoff Erstanrufen |

## 11. Readback-Pflichten

> **NfL §25 b) Nr. 1** definiert eine explizite Pflicht-Readback-Liste. Diese ist die zentrale
> Datengrundlage für den **Pilot-Utterance-Conformance-Validator** (Bucket A).

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 11.1 | Pflicht-Liste: i) Streckenfreigaben; ii) Freigaben/Anweisungen für Aufrollen, Landen auf, Start von, Anhalten vor, Kreuzen von, Rollen auf, Zurückrollen auf Pisten; iii) **Betriebspiste**, **Höhenmessereinstellungen**, **SSR-Codes**, **neu zugeteilte Funkkanäle**, **Anweisungen zur Flughöhe**, **Kurs- und Geschwindigkeitsanweisungen**; iv) Übergangsflächen | §25 b) Nr. 1 i)–iv) | ◐ | `intent_rules.json:98` READBACK-Intent; Pflichtinhalts-Check im BZF-Strict-Mode über `bzf_compliance.cpp::diff_readback` (Piste/QNH/Frequenz/Squawk + Rufzeichen), noch nicht für alle Freigabetypen verdrahtet | **A** / K — Kernfunktion Bucket A |
| 11.2 | Lotsen-Hörpflicht: Lotse muss Unstimmigkeiten erkennen und berichtigen | §25 b) Nr. 3 | ✓ | `bzf_compliance.cpp::diff_readback` unterscheidet fehlend/falsch; `apply_bzf_strict_check` (`atc_state_machine.cpp`) berichtigt falschen Wert aktiv mit Sollwert (Issue #28) |  — |
| 11.3 | Konditionelle Freigaben: vollständige Wiederholung der Bedingung Pflicht | §25 b) Nr. 2 + §25 d) | ✗ | — | **A** / K — siehe 3.4, 4.3 |

## 12. Korrektur, Standard-Tokens & Transponder

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 12.1 | „BERICHTIGUNG" / „CORRECTION" + Wiederholung der letzten korrekten Sprechgruppe + richtiger Wortlaut | §18 c) Nr. 1; §12 c) Glossar | ✓ | `intent_rules.json:28` NEGATIVE_CORRECTION Keywords: „berichtige", „berichtigung", „das ist falsch", „ich meinte" | — |
| 12.2 | „VERSTANDEN" / „ROGER" — nur als reine Empfangsbestätigung, NICHT als Antwort auf Frage die READ BACK / POSITIV / NEGATIV erfordert | §12 c); NfL Teil B 2010 Anmerkung | ✓ | `intent_rules.json:98` READBACK Keywords | — |
| 12.3 | „WILCO" — will comply (bleibt englisch im DE-Funk) | §12 c) | ◐ | nicht als eigener Token modelliert, aber READBACK absorbiert | **B** / N |
| 12.4 | „NEGATIV" / „NEGATIVE" — „Nein" / „Erlaubnis nicht erteilt" / „Nicht in der Lage" | §12 c) | ✓ | `intent_rules.json:62` UNABLE | — |
| 12.5 | „NICHT MÖGLICH" / „UNABLE" — mit Begründung | §12 c); NfL Teil B 2010 | ✓ | `intent_rules.json:62` UNABLE | — |
| 12.6 | „WIEDERHOLEN SIE" / „SAY AGAIN" (NICHT „Sagen Sie nochmals" — umgangssprachlich); „WIEDERHOLEN SIE WÖRTLICH" / READ BACK bei unvollständigem Readback | §12 c); §18 c) Nr. 4; Glossar READ BACK (dfs 2024 :808); PF65 | ◐ | BZF-Strict-Korrektur nutzt NfL-Wortlaut „WIEDERHOLEN SIE WOERTLICH" (`atc_templates.json` `bzf_strict.read_back`/`negativ_lead`/`read_back_tail`, Issue #28); allgemeiner `fallbacks.say_again` weiterhin `"wiederholen Sie."` | **B** / N — Rest-Fallback-Wortlaut optional angleichen |
| 12.7 | „SQUAWK (Code)" — Transponder-Code-Anweisung | ANLAGE 2.3.3 a)/b) | ✗ | — | **B** / M — Squawk-Token im Template fehlt |
| 12.8 | „SQUAWK IDENT" | ANLAGE 2.3.7 a) | ✗ | — | **B** / M |
| 12.9 | „SQUAWK STANDBY" / „SQUAWK NORMAL" / „SQUAWK LOW" | ANLAGE 2.3.7/2.3.8; §12 c) STANDBY | ✗ | — | **B** / N |
| 12.10 | „BESTÄTIGE SQUAWK (Code)" — Pilot-Antwort auf „BESTÄTIGEN SIE SQUAWK" | ANLAGE 2.3.6 | ✗ | — | **B** / N |
| 12.11 | „POSITIV" / „AFFIRM" — „Ja" | NfL Teil B 2010 | ✓ | implizit in READBACK-Pfad | — |

## 13. Callsign-Aussprache & ICAO-Tafel

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 13.1 | Typ-a-Callsign (Registrierung): erstes Zeichen + letzte ≥ 2 Zeichen für Verkürzung | §13 a) Nr. 1; §13 b) Nr. 1 | ✓ | `de_phraseology.cpp:121` `expand_d_callsigns()`; `de_phraseology.cpp:744` `expand_callsign_phonetic()` | — |
| 13.2 | Callsign-Verkürzung: Bodenstelle muss ZUERST verkürzen; bei Freigaben **immer vollständig** | §14 c) Nr. 1+2 | ✗ | — | **A** / M — kein Tracking ob Lotse verkürzt hat; Strict-Mode könnte hier Konformität prüfen |
| 13.3 | N-prefix US-Callsign — NfL hat **keine** Sonderregel; Typ-a-Schema gilt; ziffernweise Expansion durch BZF-Konvention | §13 a) Beispiel-Tabelle | ✓ | `de_phraseology.cpp:744` `expand_callsign_phonetic()`; aktiv im Pipeline-Flow via `settings.cpp:232` `pilot_callsign()` (DE-Profil-Switch); Catch2-Tests `tests/test_de_phraseology.cpp` Sektion „Callsign phonetic expansion" | — (Bucket C abgeschlossen — Audit-Annahme #5 widerlegt) |
| 13.4 | ICAO-Buchstabieralphabet (ALFA, BRAVO, CHARLIE, …, ZULU) | §6 — vollständige 26-Buchstaben-Tabelle mit Aussprache-Hilfe (z.B. „NO WEMM BA" für November) | ✓ | `de_phraseology.cpp:39` `kNatoLetters` | — |
| 13.5 | Sonderformen: „ALFA" (mit f), „JULIETT" (Doppel-T), „X-RAY" (Bindestrich) | §6 | ✓ | `de_phraseology.cpp:39` | — |

## 14. Ziffernweise Aussprache

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 14.1 | „ZWO" statt „ZWEI" für Ziffer 2 (zwingend) | §11; §10 a) Nr. 1 | ✓ | `de_phraseology.cpp:32` `kDigitWords` | — |
| 14.2 | Frequenz: ziffernweise + „KOMMA" (NICHT „Punkt"); bei 8,33-kHz alle 6 Ziffern | §10 a) Nr. 5+6 | ✓ | `de_phraseology.cpp:289` `expand_frequencies()` | — |
| 14.3 | QNH: „QNH" + ziffernweise (Ausnahme: 1000 hPa → „QNH EIN TAUSEND"). „HEKTOPASCAL" ist NICHT Pflicht-Suffix | §10 a) Nr. 1 ii) | ◐ | `de_phraseology.cpp` Pass 3 fügt „Hektopascal" an | **B** / N — strenge NfL-Lesung: Suffix entfernen; Konvention vieler Lehrwerke fügt es aber an. Streitbar — beim Community-Review klären |
| 14.4 | Höhe: ganze Hunderter/Tausender mit „HUNDERT"/„TAUSEND"; gemischt ziffernweise | §10 a) Nr. 2 | ✓ | `de_phraseology.cpp:97` `thousand_form()` | — |
| 14.5 | Flugflächen: FL XX0 → „FLUGFLÄCHE … HUNDERT"; sonst ziffernweise | §10 a) Nr. 1 i) | ✗ | — | **B** / N — FL kommt im VFR-Pattern selten vor; nice-to-have |
| 14.6 | Information-Letter: „INFORMATION (ICAO-Letter)" | ANLAGE 1.4.3 *b); ANLAGE 1.4.13 *e) | ✓ | `de_phraseology.cpp:455` `swap_information_alpha()` | — |

## 15. Sonstige Pflichtelemente

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 15.1 | Erstanruf-Reihenfolge: gerufene Stelle ZUERST | §14 b) Nr. 1 | ✓ | implizit in den Templates | — |
| 15.2 | „Sind Sie in Verfügung der freigegebenen Strecke?" — **in NfL 2024, NfL Teil B 2010 und BNetzA 2024 nicht behandelt** | nicht behandelt | n/a | — | (Audit-Lücke #6 korrigiert: Wortlaut existiert in keiner der Quellen — sehr wahrscheinlich Paraphrase/IFR-Verwechslung. Aus K-Liste entfernen) |
| 15.3 | Profanity / unangemessene Sprache → ATC-Reaktion | (Realismus-Strafe, nicht NfL) | ✓ | `intent_rules.json:9` INAPPROPRIATE_LANGUAGE | — |

## 16. Nationale Ergänzungen — RMZ & ATS-Suffixe

| # | Pflichtelement | NfL-Wortlaut (Anker) | Im DE-Profil? | Wo (file:line) | Bucket / Prio |
|---|---|---|---|---|---|
| 16.1 | RMZ-Einflug-Meldung: „(LFZ-Muster) (Position) (Flugregeln) (Ziffern) FUSS, WERDE IN RMZ EINFLIEGEN / WERDE RMZ DURCHFLIEGEN (Strecke) [ZUR LANDUNG] [IN (Flugplatz)]" | ANLAGE 7.4 *a) | ◐ | `RMZ_ENTER`: `intent_rules.json` + `atc_templates.json` (uncontrolled/INFO) + `handle_info_flow()` (advisory, kein Clearance, next_state IDLE). ATC-Antwort NfL-silent → Info-Stil extrapoliert | **B** / M — VFR-relevant; nur INFO/AFIS modelliert |
| 16.2 | RMZ-Verlassen-Meldung: „VERLASSE RMZ (Position) (Ziffern) FUSS" | ANLAGE 7.4 *b) | ◐ | `RMZ_LEAVE`: `intent_rules.json` + `atc_templates.json` (uncontrolled/INFO, next_state EN_ROUTE) + `handle_info_flow()`. Transit-Pilot → EN_ROUTE | **B** / M |
| 16.3 | ATS-Dienststellen-Suffixe — deutsche Bezeichnungen: TURM, ROLLKONTROLLE, ANLASSKONTROLLE, ANFLUGKONTROLLE, RADAR, INFORMATION, RADIO (UNICOM mit Flugleiter), SEGELFLUG, RÜCKHOLER | §34 b); §35 | ◐ | INFORMATION/RADIO jetzt modelliert: apt.dat-Code-1054-Namenssuffix wird zu `FrequencyType::INFO`/`RADIO` klassifiziert, eigener beratender `handle_info_flow()` (keine Freigaben), Rufname „<Platz> Information"/„<Platz> Radio" via `{station}`. TURM/ROLLKONTROLLE/ANFLUGKONTROLLE weiterhin englisch (Stilfrage, NfL erlaubt beide) | **B** / N — Tower-Suffix offen; Info/Radio abgedeckt |

---

## MAYDAY-Move — Begründung der Scope-Entscheidung

§9 (MAYDAY/PAN-PAN) und die zugehörigen NfL-Pflichten (§22, §3 a)) sind **explizit aus dem M11-MVP
ausgenommen**. Begründung:

1. **Cross-Profile-Engine-Feature, kein DE-spezifischer Gap.** MAYDAY-Recognition + Tower-Response
   würde alle Profile (EU, US, DE) gleichermaßen brauchen — der Realisierungsaufwand gehört nicht
   in den Strict-Mode der DE-Phraseologie.
2. **Flow-Level-Feature.** MAYDAY ändert den gesamten Session-Flow (Funkstille, Priorität,
   Spezial-Frequency 121.5, Notverkehr-Ende-Phrase) — das ist eine eigene Architekturentscheidung,
   nicht ein Wortlaut-Patch.
3. **M11-Plan-File listet es in Phase 4 („Notfall-Sequenz")** — Phase 2 (Strict-Mode-MVP) ist
   für Konformitäts-Checks und Wortlaut-Korrekturen reserviert.

**Dokumentations-Zweck der §9-Zeilen in dieser Matrix:** Vollständige NfL-Coverage-Erfassung für
die zukünftige Notfall-Milestone. Die §9-Zeilen zählen **nicht** in den K-Count der Bucket-Summary.

---

## Zusammenfassung — drei Buckets für Phase 2

### Bucket A — Engine-Capability: Pilot-Utterance-Conformance-Validator

**Eine Engine-Feature, die alle „kein Konformitäts-Check"-Lücken einlöst.** Adressiert:

- 1.2 Erstanruf TOWER — Pflichtinhalt-Check (Stelle / Rufzeichen / Standort / Flughöhe / Piste)
- 1.5 / 6.1 Inbound-JOIN — Pflichtelement-Check (Typ / Position / Höhe / [ATIS] / „ZUR LANDUNG")
- 1.6 ATIS-Letter im Erstanruf
- 3.1 Rollanmeldung — „ERBITTE ROLLEN" + Pflichtelemente
- 3.4 / 4.3 Konditionelle Freigaben — Wiederholung der Bedingung im Readback
- 5.1–5.3 Position-in-Pattern — Strict-Mode kann Pflicht-Suffix-Konformität prüfen
- 5.4 **Phasen-Plausibilität der Positionsmeldung** — `REPORT_POSITION_FINAL` hat
  `allowed_phases` inkl. `PATTERN` (`flight_rules.json:62`); ein im Gegenanflug fälschlich
  gefunktes „melde Endanflug" löst heute die Landefreigabe aus. Für den Simulator-Mode
  vorbestehend & akzeptabel (bare „Endanflug" verhält sich genauso); im Strict-/Trainer-Mode
  soll der Endanflug nur aus einer plausiblen Queranflug/Gegenanflug-Sequenz akzeptiert
  werden. Aufgedeckt beim Fix der „melde Endanflug"→READBACK-Fehlklassifikation
  (User-Log EDNY 2026-06-21), bewusst aus jenem Fix herausgehalten (Scope-Disziplin)
- 10.5 Erstanruf nach Frequenzwechsel — Flughöhe Pflicht
- 11.1 Readback-Pflicht-Liste (NfL §25 b) Nr. 1) — die zentrale Datengrundlage
- 11.3 Konditionelle Freigaben — vollständige Bedingungs-Wiederholung
- 13.2 Callsign-Verkürzung — Bodenstelle muss zuerst initiieren

**Realisierung:** Neuer SDK-freier TU `src/atc/bzf_compliance.{hpp,cpp}`, Erweiterung von
`intent_rules.json` um `"required_elements": [...]` pro Intent (NfL §25 b) Nr. 1 als
Datengrundlage), korrektive Tower-Responses statt toleranter Intent-Resolve.

### Bucket B — Data-only-Gaps (Phase-1 ✅ abgeschlossen 2026-06-05)

Reine Wortlaut-/Template-/Keyword-Korrekturen — kein neuer Code nötig, verbessert auch den
Simulation-Mode. Phase-1-Korrekturen sind committed; Tests grün (235 Catch2 / 42 Scenarios).

**Phase-1 — abgeschlossen (6 NfL-Korrekturen):**

| Eintrag | Aktuell → NfL-Korrektur | Status |
|---|---|---|
| 2.2 Funkprobe-Antwort | „Sie sind laut und deutlich." → **„Hoere Sie fuenf."** | ✅ `atc_templates.json:57` |
| 6.3 Touch-and-Go Tower-Templates (3×) | „Touch-and-Go frei" → **„frei zum Aufsetzen und Durchstarten"** | ✅ `atc_templates.json:153, 249, 291` |
| 6.4 Touch-and-Go Pilot-Keywords | + **„aufsetzen und durchstarten"** | ✅ `intent_rules.json:200` (REQUEST_TOUCH_AND_GO `any`) + `intent_rules.json:288` (REQUEST_LANDING `none`-Guard) |
| 10.2 Verlassen-Genehmigung (2×) | `"auf Wiederhoeren."` → **„Verlassen der Frequenz genehmigt, auf Wiederhoeren."** | ✅ `atc_templates.json:212, 478` |
| 12.6 Fallback-Phrasen „sagen Sie nochmals" (3×) | → **„wiederholen Sie"** (§18 c) Nr. 4) | ✅ `atc_templates.json:5, 6, 7` |

**Bucket-B-Phase-2 — bewusst zurückgestellt:**

| Eintrag | Begründung der Zurückstellung |
|---|---|
| 4.1 „startbereit" Keyword entfernen | Sim-Mode soll tolerant bleiben → gehört in Bucket A (Strict-Mode-Filter) |
| 7.1 „piste frei" Keyword entfernen | analog 4.1 → Bucket A |
| 2.3 Verständlichkeitsskala 1–5 | Für Strict-Mode mit variabler Stufe brauchbar; Funkprobe-Antwort ist jetzt konstant „fuenf" → reicht für Sim-Mode. Skala in Bucket A. |
| 6.5 „MACHEN SIE ABSCHLUSSLANDUNG" | Tower-Phrase; aktueller Flow gibt sie nicht expliziert. Nice-to-have. |
| 7.2 Pisten-Verlassen mit Richtung | Neues Verhalten („ERSTE/ZWEITE/PASSENDE LINKS/RECHTS"); braucht neue Variable + Engine-Logik → eher Bucket A. |
| 12.7–12.10 SQUAWK-Tokens | Neue Intents (REQUEST_SQUAWK/SQUAWK_ASSIGNMENT) + Readback-Validierung → eigener Mini-PR |
| 14.3 QNH „Hektopascal"-Suffix | Strittig (NfL listet ihn nicht, aber verbreitet); beim Community-Review klären |
| 16.1/16.2 RMZ-Phrasen | ✅ Phase 2C umgesetzt — `RMZ_ENTER`/`RMZ_LEAVE` Intents + INFO-Templates + `handle_info_flow`; advisory (NfL §7.4/§35), nur INFO/AFIS-Plätze |

### Bucket C — N-Callsign-Pipeline-Verify ✅ abgeschlossen 2026-06-05

- 13.3 `de_phraseology::expand_callsign_phonetic()` ist **aktiv** im Pipeline-Flow.
  `settings::pilot_callsign()` (`src/persistence/settings.cpp:232`) ruft die Funktion
  bei `atc_profile() == "DE"` für jedes Callsign auf — D-, HB-, N-prefix
  gleichermaßen. Die Funktion expandiert character-by-character: NATO-Tafel für Buchstaben,
  BZF-Ziffern für Digits, Dashes/Spaces werden übersprungen.
- **Audit-Annahme #5 widerlegt:** N-Callsigns werden ziffernweise expandiert
  (`"N123AB"` → `"November eins zwo drei Alfa Bravo"`).
- **7 neue Catch2-Tests** in `tests/test_de_phraseology.cpp` Sektion
  „Callsign phonetic expansion (NfL §6 + §13)" sichern das Verhalten gegen Regression
  (D-prefix, N-prefix, HB-prefix, lowercase, empty, pure digits, embedded dashes).
- `make test` grün: 235 test cases / 2830 assertions / 0 failures.

---

## Community-Review

> **Aufruf an die Community:** Diese Matrix ist publiziert, damit BZF-II/I-Inhaber sie
> zeilenweise verifizieren können — ohne den Code zu kennen müssen.
>
> **Beitrag per GitHub-Issue:** Pro Matrix-Zeile ein Issue („Row 1.2: …") mit Korrektur,
> NfL-§-Verweis, ggf. Wortlaut-Vorschlag. Schwelle bewusst niedrig gehalten.

| Feld | Wert |
|------|------|
| Wortlaut-Spalte gegen NfL Sprechfunk 2024 verifiziert? | **Ja** (Re-Anchor 2026-06-05) |
| Anzahl Pflichtelemente | 60 in 16 Sektionen |
| Klarstellungen gegenüber M11-Audit-Annahmen | 3 (siehe 5.5, 8.2, 15.2) |
| Community-Review-Phase | offen — Issue-Tracker |

---

## Akzeptanzkriterien Phase 1 (überarbeitet)

- [x] Matrix in `docs/bzf/bzf_coverage.md` existiert
- [x] ≥ 30 Pflichtelemente erfasst — aktuell **60 Einträge** in 16 Sektionen
- [x] Wortlaut-Spalte gegen NfL Sprechfunk 2024 verifiziert (Re-Anchor abgeschlossen)
- [x] Audit-Lücken aus M11-Plan-File geklärt (mit NfL-Beleg: real oder Paraphrase)
- [x] Bucket-Split (Engine-Capability A / Data-only B / Pipeline-Verify C) für Phase-2-Planung
- [x] MAYDAY ausserhalb MVP dokumentiert
- [x] Community-Review-Block ersetzt Holder-Hard-Gate
- [x] README verlinkt diese Matrix (siehe README §„Known Limitations")

## Akzeptanzkriterien Phase 2 (Strict-Mode-MVP) — Vorbedingung

- [x] Wortlaut-Spalte gegen NfL Sprechfunk 2024 verifiziert (= diese Datei)
- [x] Bucket C abgeschlossen (Audit-Annahme #5 widerlegt; 7 Catch2-Tests grün)
- [x] Bucket B Phase-1 umgesetzt (6 NfL-Wortlaut-Korrekturen in DE-Profil; alle Tests grün)
- [x] Bucket A umgesetzt:
  - [x] A1: `settings::bzf_strict_mode()` Accessor + JSON-Default (`data/settings.json`)
  - [x] A2: `src/atc/bzf_compliance.{hpp,cpp}` (SDK-frei, im `xp_atc_engine` OBJECT lib)
  - [x] A3: `bzf_strict.*` Block in `data/atc_profiles/de/atc_templates.json` + `atc_templates::lookup_bzf_strict()`
  - [x] A4: `apply_bzf_strict_check()` in `atc_state_machine::process()` + `last_clearance_text_` tracking
  - [x] A5: UI-Toggle in Settings-Tab (DE-only sichtbar, mit Tooltip)
  - [x] A6: `tests/test_bzf_compliance.cpp` (18 Catch2-Tests grün)
- [x] Universal binary baut (arm64 + x86_64)
- [x] Audit-Logging-Backend-Adapter-Rule bleibt grün
