# Fachliche Grundlagen — BZF / NfL Verordnungen & Quellen

Dieses Verzeichnis bündelt die **massgeblichen fachlichen Referenzen**, auf
denen das DE/BZF-Profil von Welly's ATC beruht. Es ist die **Single Source
of Truth** für die deutsche VFR-Sprechfunk-Phraseologie: Jede Wortlaut-Frage,
jede Phraseologie-Regel und jede Konformitätsprüfung im Plugin wird gegen die
hier abgelegten Primärdokumente belegt — nicht aus dem Gedächtnis oder aus
Sekundärquellen.

Die `.txt`-Dateien sind als **Plaintext extrahierte amtliche Dokumente** (für
`grep`-/Zitat-Workflows), damit sich jeder im Plugin verwendete Wortlaut
direkt auf die Quellzeile zurückführen lässt.

## Dokumente

| Datei | Dokument | Rolle | Stand / Gültigkeit |
|---|---|---|---|
| [`dfs_nfl_sprechfunk_2024.txt`](dfs_nfl_sprechfunk_2024.txt) | **DFS — Bekanntmachung über die Sprechfunkverfahren** (NfL 2024-1-3266), herausgegeben vom Bundesaufsichtsamt für Flugsicherung auf Grund § 29 Abs. 1 Nr. 2 LuftVO | **Primärquelle & Wortlaut-Anker.** Massgeblich für die gesamte VFR-Phraseologie, die Verfahrens­abläufe und die Pflichtinhalte (z. B. Erstanruf, Readback nach §25). | hebt NfL 2023-1-2726 auf; gültig ab 28.11.2024 |
| [`bnetza_pruefungsfragen_2024.txt`](bnetza_pruefungsfragen_2024.txt) | **BNetzA — Prüfungsfragen im Prüfungsteil „Kenntnisse"** für das Beschränkt Gültige Sprechfunkzeugnis I und II (BZF I / BZF II) | **Prüfungs-/Examensbezug.** Quelle für die BZF-relevanten Pflichtelemente und die Abgrenzung „NfL-Pflicht" vs. „BZF-Didaktik" im Strict-Mode. | gültig ab 01.05.2024 |
| [`nfl_funk_teilb_2010.txt`](nfl_funk_teilb_2010.txt) | **NfL Teil B (Phrasen)** — Anlagen 7 und 8 der DFS-Bekanntmachung (NfL I 226/10) | **Redewendungs-Referenz.** Zweisprachige (DE/EN) Standard-Redewendungen (BESTÄTIGEN SIE, POSITIV, GENEHMIGT, …) als historischer, weiterhin gültiger Phrasen-Anker. | in Kraft seit 15.10.2010 |

## Abgeleitete Arbeitsunterlage

| Datei | Inhalt |
|---|---|
| [`bzf_coverage.md`](bzf_coverage.md) | **BZF-Coverage-Matrix.** Erfasst die BZF-II-relevanten Pflichtelemente der deutschen VFR-Sprechfunkprozedur und mappt jedes Element gegen den aktuellen Stand des `de`-Profils (`data/atc_profiles/de/*.json` + `src/atc/de_phraseology.{hpp,cpp}`). Legende: ✓ abgedeckt · ◐ teilweise · ✗ fehlt; Prio K/M/N; Bucket A/B/C. Jeder zitierte Wortlaut stammt unverändert aus den Primärquellen oben. |

## Verwendung im Projekt

- **Verbindlichkeit:** Bei jeder Änderung an Phraseologie, Intent-Regeln,
  Templates oder Konformitätsprüfungen ist der Wortlaut gegen
  `dfs_nfl_sprechfunk_2024.txt` zu belegen (Sekundär: BNetzA-Prüfungsfragen
  und NfL Teil B). Das gilt insbesondere für `bzf_strict_mode` und
  `src/atc/{bzf_compliance,initial_call_conformance,de_phraseology}.*`.
- **Wichtige Abgrenzung (NfL 2024 §1.4.7 / §1.4.3):** Im Erstanruf ist nur
  das Roll-Anliegen zwingend; Luftfahrzeugmuster, Standort, Absichten und
  ATIS-Buchstabe sind [optional]. Die „5 Pflichtelemente" sind BZF-Didaktik
  (aus den BNetzA-Prüfungsfragen), **kein** NfL-Pflichtumfang — die Quellen
  in diesem Verzeichnis sind die Grundlage für genau diese Unterscheidung.
- **Status & Mitarbeit:** Die Coverage-Matrix ist in Community-Review;
  Korrekturen von BZF-Inhabern sind ausdrücklich willkommen (siehe Hinweise
  in `bzf_coverage.md`).

> **Rechtlicher Hinweis:** Diese Sammlung dient ausschliesslich der
> originalgetreuen Nachbildung in der Flugsimulation. Sie ersetzt **keine**
> offizielle Ausgabe der Dokumente und ist **kein** Prüfungs- oder
> Ausbildungsersatz. Massgeblich sind stets die amtlichen Originale von DFS,
> BAF und Bundesnetzagentur in ihrer jeweils gültigen Fassung.
