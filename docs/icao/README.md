# Fachliche Grundlagen — ICAO / EN-VFR Phraseologie & Quellen

Dieses Verzeichnis bündelt die **massgeblichen fachlichen Referenzen** für das
**englische ICAO-VFR-Profil** (`"EN"`) von Welly's ATC — das Pendant zu
[`docs/bzf/`](../bzf/README.md) für das deutsche NfL/BZF-Profil. Es ist die
**Single Source of Truth** für die englische VFR-Sprechfunk-Phraseologie: Jede
Wortlaut-Frage, jede Phraseologie-Regel und jede Response im `en`-Profil wird
gegen die hier gelisteten Primärdokumente belegt — **nicht** aus dem Gedächtnis,
**nicht** als Übersetzung des NfL-Wortlauts.

> **Grundsatz (siehe Epic #35):** Englischer VFR-Funk ist **keine Übersetzung**
> des deutschen Profils. Er folgt einer eigenständigen Phraseologie nach ICAO
> Annex 10 Vol II / ICAO Doc 9432 bzw. EASA SERA / CAP 413 — andere
> Standardwortlaute (`line up and wait` ≠ „reihen Sie sich ein"), andere
> Zahlen-/Rufzeichen-Aussprache, andere Readback-Regeln.

## Referenz-Entscheidung: ICAO/EASA als Basis, DACH-neutral

- **Basis-Wortlaut:** ICAO (Annex 10 Vol II, Doc 9432) und EASA **SERA** — die in
  DE/AT/CH gleichermassen gültige Rechts-/Verfahrensbasis. Kein länderspezifischer
  Wortlaut.
- **CAP 413** wird **nur als Beispiel-Illustration** herangezogen (frei verfügbar,
  praxisnahe VFR-Beispiele). **UK-Spezifika werden ausdrücklich ausgeschlossen** —
  etwa der UK-FIS-Wortlaut („Basic Service" / „Traffic Service" /
  „Deconfliction Service") oder UK-eigene Verfahren. Diese haben in DACH keine
  Entsprechung.
- **Kein IFR, kein FR/IT** (siehe Epic #35 Nicht-Ziele).

## Dokumente / Referenzen

Anders als bei den frei publizierbaren deutschen NfL-Dokumenten sind die
massgeblichen ICAO-Werke **urheberrechtlich geschützt** (ICAO) bzw. umfangreiche
Amtspublikationen (CAA). Sie werden daher **nicht** als Volltext in dieses Repo
eingecheckt, sondern **per Referenz zitiert** (Dokument + Kapitel/Abschnitt +
Bezugs-URL). Kurze Wortlaut-Zitate in [`icao_coverage.md`](icao_coverage.md)
dienen ausschliesslich der originalgetreuen Nachbildung.

| Referenz | Dokument | Rolle | Bezug |
|---|---|---|---|
| **Annex 10 Vol II** | ICAO Annex 10 to the Convention on International Civil Aviation — *Aeronautical Telecommunications, Vol II (Communication Procedures)* | **Basis-Standard & Wortlaut-Anker.** Definiert die Standard-Wörter/Phrasen, Zahlen-/Buchstabieraussprache und die Readback-Pflichten. | ICAO (kostenpflichtig); zusammengefasst auf [SKYbrary](https://skybrary.aero/) |
| **Doc 9432** | ICAO *Manual of Radiotelephony* (Doc 9432) | **Kernquelle für Beispiel-Phrasen.** Kapitel zu Aerodrome Control, VFR-Verfahren, Positionsmeldungen. | ICAO (kostenpflichtig) |
| **SERA** | Durchführungsverordnung (EU) Nr. 923/2012 (*Standardised European Rules of the Air*), inkl. SERA.14 (Kommunikation) | **EU-VFR-Rechtsbasis** für DE/AT (und CH via bilaterale Übernahme). | [EUR-Lex 32012R0923](https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32012R0923) |
| **CAP 413** | UK CAA *Radiotelephony Manual* (CAP 413) | **Praxis-Beispiele (nur Illustration).** UK-Spezifika ausgeschlossen. | [CAA publicapps](https://publicapps.caa.co.uk/) (frei) |
| **EGAST / EASA VFR-Guide** | EASA/EGAST *„Radiotelephony guide for VFR pilots"* | Kompakte VFR-Zusammenfassung. | EASA (frei) |

> Die konkret abgerufenen URLs und die pro Phrase verwendeten Abschnittsnummern
> stehen in der Quellenliste am Ende von [`icao_coverage.md`](icao_coverage.md).

## Abgeleitete Arbeitsunterlage

| Datei | Inhalt |
|---|---|
| [`icao_coverage.md`](icao_coverage.md) | **ICAO-Coverage-Matrix.** Mappt jeden `PilotIntent` und jede ATC-Response auf die englische Standardphrase (ICAO/CAP 413) samt Quellenstelle. Arbeitsgrundlage für das `en`-Profil-Bundle (#38) und das `en_phraseology`-Modul (#41). Legende analog `bzf_coverage.md`. |

## Verwendung im Projekt

- **Verbindlichkeit:** Bei jeder Änderung an EN-Phraseologie, EN-Intent-Regeln,
  EN-Templates oder EN-Konformität ist der Wortlaut gegen die Referenzen oben zu
  belegen (primär ICAO Annex 10 / Doc 9432, Rechtsbasis SERA; CAP 413 nur als
  Beispiel). Betrifft künftig `data/atc_profiles/en/*.json` und
  `src/atc/en_phraseology.*` (#41).
- **Kein 1:1-BZF-Pendant:** Der deutsche `bzf_strict_mode` / `conformance` /
  `initial_call_conformance`-Layer ist ein **BZF-Prüfungs-Konstrukt** ohne direkte
  EN-Entsprechung; das Pendant wäre ICAO-Phraseologie-Konformität (Entscheidung
  offen in #42).
- **Readback:** Der EN-Readback-Pflichtumfang (ICAO Annex 10 / SERA) weicht vom
  NfL §25 ab — die massgebliche ICAO-Readback-Liste steht in `icao_coverage.md`.

> **Rechtlicher Hinweis:** Diese Sammlung dient ausschliesslich der
> originalgetreuen Nachbildung in der Flugsimulation. Sie ersetzt **keine**
> offizielle Ausgabe der Dokumente und ist **kein** Prüfungs- oder
> Ausbildungsersatz. Massgeblich sind stets die amtlichen Originale von ICAO,
> EASA und der jeweiligen nationalen Luftfahrtbehörde in ihrer gültigen Fassung.
