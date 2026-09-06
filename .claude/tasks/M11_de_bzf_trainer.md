# M11 — DE/BZF Trainer Mode

## Context

Das DE/BZF-Profil ist heute eine **realistische Simulation** mit echten BZF-Begriffen ("Start frei", "Piste", "Gegenanflug", "Information Alfa") und einer dedizierten C++ Normalisierungs-Pipeline (`src/atc/de_phraseology.cpp` — ziffernweise Aussprache, BZF-NATO-Tafel, Frequenz-Wortlaut). Authentizitäts-Score laut Audit: 7.5/10. Es ist **kein** Trainer für die BZF-Praxis-Prüfung.

Der Markt für deutschsprachiges BZF-Funktraining auf dem Mac ist unterversorgt: SayIntentions, BeyondATC, PilotEdge sind alle englisch-erst oder US-only. Hier liegt der eigentliche USP — aber nur, wenn das Produkt mehr ist als ein tolerantes LLM, das BZF-Slang akzeptiert.

**Ziel von M11**: Aus der Simulation einen Trainer machen, der **pre-exam User (BZF-Vorbereitung)** UND **post-exam User (Skills auffrischen)** abdeckt, ohne den Simulation-Modus zu verlieren. Mechanismus: Strict-Mode-Toggle.

## Strategische Entscheidungen

Festgelegt mit User am 2026-06-05:

- **Audience**: Beide Gruppen — mit `bzf_strict_mode` Toggle in Settings. Default `false` (Simulation, immersiv). Strict an: Pflicht-Konformitäts-Check, Tower reagiert wie Prüfer.
- **Scope**: Iterativ — erst Coverage-Matrix + Strict-Mode-MVP, dann Debrief, dann Scenario-Presets als separate Arcs.
- **Marketing-Guardrail** (Berater-Hinweis): Plugin darf **NICHT** als "BZF-Prüfungsvorbereitung" beworben werden bis die Coverage-Matrix steht und unabhängig von einem BZF-Inhaber abgenommen ist. Bei 150 Usern ist die Credibility das Produkt.

## Bestehende Bausteine (zum Wiederverwenden)

- `src/atc/de_phraseology.{hpp,cpp}` — `normalize_for_speech()`, `parse_spoken_number()`, `expand_callsign_phonetic()`. Die ziffernweise Aussprache ist BZF-konform; aber `parse_spoken_number()` arbeitet auf dem Pilot-Transkript und sollte für Strict-Mode-Konformitäts-Checks erweitert/wiederverwendet werden.
- `data/atc_profiles/de/intent_rules.json` — bereits 100+ BZF-Keywords. Strict-Mode kann auf "MUSS"-Keywords vs "OPTIONAL"-Keywords erweitern.
- `data/atc_profiles/de/phraseology_hints.json` — Hint-Matrix. Strict-Mode-Korrekturen können daraus generiert werden.
- `src/atc/engine.cpp::build_unclear_response()` mit dem 3-stufigen Eskalation-Pfad (say_again → garbled → use_standard_phraseology) — Vorlage für die Strict-Mode-Korrekturen.
- `_INVALID`-Fallback-Block in `atc_templates.json` (neu, aus dem Refactor) — kann pro Profil um Strict-Mode-Korrektur-Strings erweitert werden.

## Plan (Phasen)

### Phase 1 — BZF-Coverage-Matrix (read-only, kein Code)

Bevor irgendwas am Code passiert, wird in einem Markdown-Dokument (z.B. `docs/bzf_coverage.md`) folgendes erfasst:

- Spalten: `BZF-Spec Pflichtelement` | `Phrase EN-Ref (ICAO)` | `Aktuell drin im DE-Profil?` | `Wo (file:line)` | `Lücke`
- Quellen: BZF-Sprechfunk-Verfahrensanweisung (BFU), DFS AIP VFR, skyguide VAC, Austro Control AIM
- Liste der **bekannten Lücken** aus dem Audit:
  1. Notfall-Phrasen (MAYDAY/PAN-PAN) fehlen komplett — weder Pilot-Intent noch Tower-Response
  2. TRAFFIC-Dialog ist mit EU identisch — keine DE-typische Verkehrsmeldung `"ein Verkehr in zwo Uhr, drei Meilen, gleiche Hoehe"`
  3. Pilot-Rollanmeldung `"Ich rolle fuer Piste X ueber Rollbahn Alpha"` fehlt als Intent-Variante
  4. Funkpruefung-Response nur `"Sie sind laut und deutlich"` statt BZF-Standard `"Lesbarkeit fuenf"` / `"ich hoere Sie fuenf"`
  5. N-prefix US-Callsigns werden nicht ziffernweise expandiert (BZF-Tafel)
  6. `"Sind Sie Verfuegung der freigegebenen Strecke?"` — BZF-Pflichtelement bei Erstkontakt fehlt
- Pro Lücke: Prioritäts-Score (kritisch / mittel / nice-to-have)

**Akzeptanz**: Coverage-Matrix ist in `docs/bzf_coverage.md` und wird einmalig von einem BZF-Inhaber gegengelesen, bevor Phase 2 startet.

### Phase 2 — Strict-Mode MVP (Code, ohne Debrief)

Mechanismus, der den Tower zum Prüfer macht:

- **Setting**: `bzf_strict_mode` (bool, default `false`). UI-Toggle nur sichtbar wenn `atc_profile == "DE"`.
- **Konformitäts-Check pro Pilot-Transkript** (neu in `engine.cpp` oder in einem neuen `src/atc/bzf_compliance.{hpp,cpp}`):
  - Pflichtelemente pro Intent erkennen (Callsign vorhanden / Runway genannt / QNH zurückgelesen / Frequenz korrekt phrasiert)
  - Datenquelle: Erweiterung von `intent_rules.json` um `"required_elements": ["callsign", "runway", "qnh"]` pro Intent
- **Korrektive Tower-Antworten** statt toleranter Intent-Resolve: bei fehlendem Pflichtelement nicht silent absorbieren, sondern z.B. `"{callsign}, wiederholen Sie die vollstaendige Freigabe mit QNH"`.
- **Strict-spezifische Fallback-Strings** in `data/atc_profiles/de/atc_templates.json` unter dem `fallbacks`-Block (neuer Sub-Block `bzf_strict.*`).
- **LM-Pfad bleibt aus** im Strict-Mode für Whisper-Fixes (sonst korrigiert das LM die Pilot-Fehler weg — genau das Gegenteil davon, was Trainer-Modus soll). Nur Whisper-Mishearings korrigieren, nicht Pilot-Phrasing.
- **Lücken aus Phase 1 schließen** (zumindest die kritischen): Notfall-Intents, DE-Traffic-Dialog, Funkpruefung-Response, N-Callsign-Expand.

**Akzeptanz**:
- Plugin lädt mit Strict-Mode an, Tower spielt Prüfer
- 3-5 Testflüge mit bewussten Phraseologie-Fehlern → Tower reagiert korrektiv, nicht tolerant
- Catch2-Tests für Konformitäts-Check (Pflichtelement vorhanden / fehlt → erwartete Tower-Antwort)
- Audit-Logging-Test grün (Backend-Adapter-Rule)

### Phase 3 — Debrief (Post-Flight Phraseology Report)

Erst angreifen wenn Phase 2 in der Praxis steht und ein BZF-Inhaber das Strict-Mode-Verhalten als realitätsnah einstuft.

- Strict-Mode sammelt während des Flugs eine Compliance-Liste: `(timestamp, intent, missing_elements, corrective_response)`
- Am Flug-Ende (z.B. Engines off / X-Plane Pause): Modal mit `"Phraseologie-Auswertung"` — Liste der Fehler, Statistik (X von Y Anweisungen vollständig phrasiert).
- Export als Plain-Text-Log in `<X-Plane>/Output/preferences/xp_wellys_devfr_atc/debrief_<YYYY-MM-DD>.txt`.

### Phase 4 — Scenario-Presets (BZF-Praxis-Prüfungsstruktur)

Separater Arc. Vermutlich eigener Milestone `M12_bzf_scenarios.md`. Aufgaben analog zur realen BZF-Praxis:
- Kontrollzonen-Abflug (z.B. EDDS Stuttgart)
- Transit durch belebte CTR
- Anflug Info-Platz (uncontrolled mit Pflichtmeldungen)
- Wechsel zur Verkehrsfrequenz + Selbstanmeldung
- Notfall-Sequenz (MAYDAY/PAN)

Voraussetzung: Phase 1-3 abgeschlossen, Coverage-Matrix von BZF-Inhaber abgenommen.

## Tooltip / Marketing-Sprache

Solange Phase 1 nicht abgeschlossen ist:
- **DARF**: "DACH-Phraseologie mit ziffernweiser Aussprache" / "realistische BZF-Simulation"
- **DARF NICHT**: "BZF-Prüfungsvorbereitung" / "BZF-Trainer" / "Prüfungstauglich"

Tooltip wurde im Vor-Refactor-Commit bereits entsprechend angepasst.

## Verifikation (übergeordnet, pro Phase einzeln im Detail)

- Coverage-Matrix existiert in `docs/bzf_coverage.md` mit ≥30 Pflichtelementen
- `bzf_strict_mode`-Toggle im UI sichtbar (DE-Profil), default false
- Strict-Mode aktiv: 3 Test-Szenarien mit absichtlichen Phraseologie-Fehlern produzieren korrektive Tower-Antworten
- Catch2-Suite grün, neue Tests für Konformitäts-Check
- BZF-Inhaber-Review der Coverage-Matrix dokumentiert (Name/Datum/abgenommen ja/nein)

## Out of Scope für M11

- M10 (Cross-Country) — wird nach M11 neu beurteilt
- Lokalisierung über Deutsch hinaus (FR, IT, NL)
- Voice-Activity-Detection-Verbesserungen
- Whisper-Prompt-Bias für Deutsch (separater Track, siehe Plan-File des vorherigen Refactors Abschnitt 10)
