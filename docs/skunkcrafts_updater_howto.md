# SkunkCrafts Updater — Integrations-HOWTO

Wie man ein X-Plane-Addon für den **SkunkCrafts Updater** updatefähig macht.
Diese Anleitung dokumentiert jeden Schritt, der in `xp_wellys_vfr_atc`
gemacht wurde, so dass sie sich 1:1 auf weitere Projekte übertragen lässt.
**Projekt-spezifische Stellen sind mit 🔧 markiert** — das sind die einzigen
Dinge, die du pro Projekt anpassen musst.

---

## 1. Wie der Updater funktioniert (Kurzfassung)

- Der SkunkCrafts Updater ist seit v3.0 ein **eigenständiges Desktop-Tool**
  (Go/Fyne, Win/macOS/Linux), kein In-Sim-Plugin mehr.
- Er **scannt** das X-Plane-Verzeichnis nach `skunkcrafts_updater.cfg`-Dateien.
  Jedes Addon, das so eine Datei mitbringt, wird automatisch entdeckt.
- Für jedes entdeckte Addon liest er die `module`-URL, holt von dort die
  **Kontrolldateien**, vergleicht `version` + CRC32 + Größe und lädt
  **differenziell** nur geänderte Dateien nach.
- Das Protokoll ist **offen und rein dateibasiert** (`key|value`,
  Pipe-getrennt). Keine Registrierung bei SkunkCrafts nötig — du hostest
  einfach selbst (z. B. eine `release`-Branch auf GitHub).

### Zwei Seiten

| Seite | Liegt wo | Inhalt |
|---|---|---|
| **Client** | im Plugin-Ordner des Nutzers | nur `skunkcrafts_updater.cfg` (zeigt per `module`-URL aufs Repo) |
| **Server** | deine `release`-Branch / HTTPS-Host | kompletter Plugin-Baum **+** Kontrolldateien |

### Kontrolldateien (Server-Seite)

| Datei | Format | Zweck |
|---|---|---|
| `skunkcrafts_updater.cfg` | `key\|value` | Version + Modul-URL (wird auch mit-ausgeliefert) |
| `skunkcrafts_updater_whitelist.txt` | `pfad\|crc32` | CRC32 (unsigned 32-Bit **dezimal**) je verwalteter Datei |
| `skunkcrafts_updater_sizeslist.txt` | `pfad\|bytes` | Dateigröße je Datei |
| `skunkcrafts_updater_oncelist.txt` | `pfad` | nur laden wenn **fehlend**, nie überschreiben — Datei gehört **nicht** zusätzlich in whitelist/sizeslist |
| `skunkcrafts_updater_blacklist.txt` | `pfad` | lokal **LÖSCHEN** (Vorsicht!) |
| `skunkcrafts_updater_ignorelist.txt` | `pfad` | beim Update-Check ignorieren |

> **Goldene Regel:** Dateien, die **nicht** in der whitelist stehen, lässt
> der Updater unangetastet. Große, separat heruntergeladene Daten (Modelle,
> User-Daten) gehören **weder in die whitelist noch in die blacklist** —
> einfach gar nicht tracken. Die blacklist löscht!

> **Kehrseite derselben Regel — Umbenennungen räumen sich nicht selbst auf.**
> Genau weil nicht-gewhitelistete Dateien unangetastet bleiben, überlebt eine
> Datei, die wir früher unter einem anderen Namen ausgeliefert haben, jedes
> Update. v0.6.0 lieferte `xp_wellys_devfr_atc/mac_x64/xp_wellys_devfr_atc.xpl`,
> v0.6.1 benannte Ordner **und** Binary um. Der Updater verwaltet das
> Verzeichnis, in dem die `.cfg` liegt — der Ordnername ist ihm egal —, also
> zog er brav die neue `.xpl` in den alten Ordner und liess die alte liegen.
> X-Plane lädt **jede** `.xpl` im Arch-Ordner: zwei Instanzen, gleiche
> Signature, gleiche Command-Namen. Sichtbares Symptom: die Updater-UI meldet
> die neue Version, das Plugin-Fenster die alte.
>
> Deshalb ist `LEGACY_PATHS` in `generate.py` die **einzige** Quelle der
> blacklist: exakte Pfade, nie Globs, nie etwas, das einem Nutzer gehören
> könnte. Der Generator bricht hart ab, wenn ein blacklisteter Pfad zugleich
> ausgeliefert wird, in der oncelist steht oder unter `IGNORE_GLOBS` fällt.
> Einträge bleiben dauerhaft stehen — das Löschen einer Datei, die der Client
> nie hatte, ist ein No-op, und es gibt keinen Zeitpunkt, ab dem man
> Alt-Installationen ausschliessen kann.
>
> **Oncelist-Sonderfall:** Eine user-editierbare Config (`settings.json`)
> steht **ausschliesslich** in der oncelist — **nie** zusätzlich in
> whitelist/sizeslist. Der whitelist-CRC32 (bzw. der sizeslist-Eintrag) ist
> der Diff-Trigger des Updaters: sobald der Nutzer die Datei ändert, driftet
> die Prüfsumme und ein Client, der die whitelist über die oncelist stellt,
> flaggt sie als out-of-sync und überschreibt sie (siehe Issue #27). Nur in
> der oncelist bleibt „laden wenn fehlend" erhalten, ohne je einen
> Überschreib-Grund zu liefern.

### cfg-Felder

| Feld | Bedeutung |
|---|---|
| `zone` | `custom` = `module` ist die volle URL |
| `module` | Basis-URL des Remote-Repos mit den Update-Dateien |
| `name` | Anzeigename in der Updater-UI |
| `version` | aktuell installierte Version |
| `disabled` | `true`/`false` — Eintrag aktiv? |
| `locked` | `true`/`false` — Updates gesperrt (Wartungsmodus) |
| `liveries` | `true`/`false` — enthält Liveries? |

---

## 2. Dateien, die pro Projekt angelegt werden

```
tools/skunkcrafts/
├── skunkcrafts_updater.cfg.template   # cfg mit @VERSION@-Platzhalter
└── generate.py                        # erzeugt whitelist/sizeslist/oncelist + cfg
```

Plus Integration in `Makefile` (lokaler Build) und
`.github/workflows/*.yml` (Release).

---

## 3. Schritt 1 — `skunkcrafts_updater.cfg.template`

`tools/skunkcrafts/skunkcrafts_updater.cfg.template`:

```
zone|custom
liveries|false
module|https://raw.githubusercontent.com/<USER>/<REPO>/refs/heads/release/
version|@VERSION@
disabled|false
name|<Anzeigename>
locked|false
```

🔧 **Anpassen pro Projekt:**
- `module`-URL → `<USER>/<REPO>` deines GitHub-Repos. `refs/heads/release/`
  ist die Branch, auf die der Release-Workflow den Baum pusht.
- `name` → Anzeigename im Updater.

> `@VERSION@` wird vom Generator durch die echte Version ersetzt.

---

## 4. Schritt 2 — `generate.py`

`tools/skunkcrafts/generate.py` — läuft über einen **installierten Plugin-Baum**
und schreibt die Kontrolldateien hinein. Portabel (nur Python-Stdlib: `zlib`,
`os`, `fnmatch`):

```python
#!/usr/bin/env python3
"""SkunkCrafts Updater control-file generator."""
import argparse, fnmatch, os, zlib
from pathlib import Path

# 🔧 Pfade (relativ zum Plugin-Root), die der Updater NICHT verwalten soll.
IGNORE_GLOBS = [
    "Resources/models/*",        # gross, separat geladen -> nie tracken/loeschen
    "data/flightlog/*",          # User-Runtime-Daten
    ".DS_Store", "**/.DS_Store",
    "skunkcrafts_updater_*.txt",
    "skunkcrafts_updater.cfg",
]
# 🔧 Nur laden wenn fehlend, nie ueberschreiben (User-editierbare Configs).
ONCE_GLOBS = [
    "data/settings.json",
]

def crc32(fp: Path) -> int:
    c = 0
    with fp.open("rb") as f:
        while chunk := f.read(65536):
            c = zlib.crc32(chunk, c)
    return c & 0xFFFFFFFF

def matches(rel, globs): return any(fnmatch.fnmatch(rel, g) for g in globs)

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True)
    ap.add_argument("--version", required=True)
    a = ap.parse_args()
    tree = Path(a.tree).resolve()
    template = (Path(__file__).parent / "skunkcrafts_updater.cfg.template").read_text()

    whitelist, sizes, once = [], [], []
    for dp, _d, files in os.walk(tree):
        for name in files:
            ap_ = Path(dp) / name
            rel = ap_.relative_to(tree).as_posix()
            if matches(rel, IGNORE_GLOBS):
                continue
            # ONCE_GLOBS -> oncelist ONLY, never whitelist/sizeslist. Ein
            # CRC32/Size-Eintrag dort ist der Diff-Trigger; eine user-editierte
            # Datei wuerde sonst als out-of-sync geflaggt + ueberschrieben.
            if matches(rel, ONCE_GLOBS):
                once.append(rel)
                continue
            whitelist.append(f"{rel}|{crc32(ap_)}")
            sizes.append(f"{rel}|{ap_.stat().st_size}")
    whitelist.sort(); sizes.sort(); once.sort()
    (tree / "skunkcrafts_updater_whitelist.txt").write_text("\n".join(whitelist) + "\n")
    (tree / "skunkcrafts_updater_sizeslist.txt").write_text("\n".join(sizes) + "\n")
    (tree / "skunkcrafts_updater_oncelist.txt").write_text("\n".join(once) + "\n")
    (tree / "skunkcrafts_updater.cfg").write_text(template.replace("@VERSION@", a.version))
    print(f"tracked {len(whitelist)} files, {len(once)} once-only -> {tree}")

if __name__ == "__main__":
    main()
```

🔧 **Anpassen pro Projekt:** nur `IGNORE_GLOBS` + `ONCE_GLOBS`.

Frag dich für jedes Projekt:
1. **Was wird separat geladen / ist riesig?** → in `IGNORE_GLOBS` (Modelle,
   große Assets, die nicht über den Updater laufen sollen).
2. **Welche Dateien editiert der Nutzer?** → in `ONCE_GLOBS` (Settings, Keys,
   Profile), damit ein Update sie nicht überschreibt. Der Generator schreibt
   ONCE_GLOBS-Treffer bewusst **nur** in die oncelist und lässt sie aus
   whitelist/sizeslist — sonst würde der CRC/Size-Diff sie doch überschreiben.
3. **Was ist reine Runtime-Ausgabe?** (Logs, Caches) → in `IGNORE_GLOBS`.

---

## 5. Schritt 3 — Makefile-Target (lokaler Test/Release)

Variable oben bei den anderen Vars:

```make
SKUNK_DIR := build/skunkcrafts
```

`.PHONY`-Zeile um `skunkcrafts` ergänzen. Dann das Target:

```make
skunkcrafts:
	@if [ ! -d "$(PLUGIN_DIR)" ]; then \
	    echo "Plugin not installed at '$(PLUGIN_DIR)'. Run 'make install' first."; exit 1; \
	fi
	@VER="$(VERSION)"; \
	if [ -z "$$VER" ] && [ -f VERSION.txt ]; then VER="$$(cat VERSION.txt)"; fi; \
	if [ -z "$$VER" ]; then echo "No version. Set VERSION=x.y.z or VERSION.txt."; exit 1; fi; \
	echo "=== Staging SkunkCrafts release tree ($$VER) ==="; \
	rm -rf "$(SKUNK_DIR)"; mkdir -p "$(SKUNK_DIR)"; \
	rsync -a \
	    --exclude 'Resources/models/' \
	    --exclude 'data/flightlog/' \
	    --exclude '.DS_Store' \
	    --exclude 'skunkcrafts_updater*' \
	    "$(PLUGIN_DIR)/" "$(SKUNK_DIR)/"; \
	python3 tools/skunkcrafts/generate.py --tree "$(SKUNK_DIR)" --version "$$VER"; \
	echo "Staged release tree at $(SKUNK_DIR)/ (version $$VER)."
```

🔧 **Anpassen:** `--exclude`-Liste muss zu `IGNORE_GLOBS` passen (große
Verzeichnisse hier ausschließen, damit sie gar nicht erst kopiert werden).
`$(PLUGIN_DIR)` ist der installierte Plugin-Pfad deines Projekts.

**Lokal testen:**
```bash
make build && make install
make skunkcrafts                 # Version aus VERSION.txt
make skunkcrafts VERSION=0.5.0   # explizit
```
Ergebnis: ein sauberer, publizierbarer Baum in `build/skunkcrafts/`. Die
Live-Installation bleibt unangetastet.

---

## 6. Schritt 4 — GitHub-Actions-Release

Voraussetzung: ein Workflow, der bei Tag-Push (`v*`) bereits einen
**signierten, install-fertigen Baum** staged (hier: `dist/<plugin>`). Diesen
Baum wiederverwenden — **nicht** `make skunkcrafts` in CI aufrufen (das Target
staged aus dem lokalen `PLUGIN_DIR`, den es in CI nicht gibt).

### 6a. Kontrolldateien in den Staging-Baum generieren — VOR dem Zippen

Direkt vor dem `zip`-Schritt im Staging-Step:

```yaml
          # SkunkCrafts-Kontrolldateien in den Staging-Baum, VOR dem Zip,
          # damit auch die Release-ZIP die cfg traegt (manuelle Installs
          # werden updater-discoverable). Version aus dem Tag (v1.2.3->1.2.3).
          python3 tools/skunkcrafts/generate.py \
            --tree "$STAGE" --version "${GITHUB_REF_NAME#v}"

          cd dist && zip -r <plugin>.zip <plugin>/
```

### 6b. Neuer Step — Baum auf die `release`-Branch pushen

Nach dem „Create release"-Step:

```yaml
      - name: Publish SkunkCrafts update tree
        if: startsWith(github.ref, 'refs/tags/v')
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          set -euo pipefail
          STAGE=dist/<plugin>
          rm -rf "$STAGE/Resources/models"   # leeren/nicht-verwalteten Ordner droppen
          pushd "$STAGE"
          git init -q
          git config user.name  "github-actions[bot]"
          git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
          git add -A
          git commit -q -m "release ${GITHUB_REF_NAME}"
          git branch -M release
          git push --force \
            "https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git" release
          popd
```

🔧 **Anpassen:** `$STAGE`-Pfad (`dist/<plugin>`) und der `rm -rf`-Ausschluss
auf deine ignorierten Verzeichnisse.

**Warum so:**
- **Force-Push, Single-Commit:** `git init` + ein Commit + `--force` ersetzt
  die `release`-Branch komplett. So sammeln sich Binaries **nicht** über die
  History an — die Branch trägt immer nur den aktuellen Snapshot.
- **Generieren vor dem Zip:** Die GitHub-Release-ZIP enthält dann ebenfalls
  `skunkcrafts_updater.cfg` → auch manuell Installierte werden updatefähig.
- **`GITHUB_TOKEN`** reicht zum Pushen, wenn der Job `permissions: contents:
  write` hat.

### 6c. Job-Permissions

Im Workflow muss der Job:
```yaml
    permissions:
      contents: write
```
haben (für das Pushen der Branch **und** das Erstellen des GitHub-Release).

---

## 7. Einmalige Repo-Einstellungen

1. **Workflow-Permissions:** Repo → *Settings → Actions → General →
   Workflow permissions* → **„Read and write permissions"** aktivieren.
   Sonst kann `GITHUB_TOKEN` trotz `contents: write` im Workflow nicht
   pushen (falls die Org-Default auf read-only steht).
2. **`release`-Branch:** muss **nicht** manuell angelegt werden — der erste
   `git push --force` erstellt sie automatisch.

---

## 8. Verifikation

### Lokal (vor dem ersten echten Release)
```bash
make skunkcrafts
ls build/skunkcrafts/skunkcrafts_updater*        # 4 Kontrolldateien?
cat build/skunkcrafts/skunkcrafts_updater.cfg    # Version + module-URL korrekt?
grep -c "Resources/models" build/skunkcrafts/skunkcrafts_updater_whitelist.txt   # -> 0
cat build/skunkcrafts/skunkcrafts_updater_oncelist.txt   # nur User-Configs?
```

### Nach dem ersten Tag-Push
- Actions-Run grün? Step „Publish SkunkCrafts update tree" durchgelaufen?
- `release`-Branch existiert und enthält den Baum + Kontrolldateien?
- `module`-URL im Browser erreichbar, z. B.
  `https://raw.githubusercontent.com/<USER>/<REPO>/refs/heads/release/skunkcrafts_updater.cfg`
- Im SkunkCrafts-Updater-Client: Addon wird gelistet, Version stimmt.

---

## 9. Checkliste pro neuem Projekt

- [ ] `tools/skunkcrafts/skunkcrafts_updater.cfg.template` — 🔧 `module`-URL + `name`
- [ ] `tools/skunkcrafts/generate.py` — 🔧 `IGNORE_GLOBS` + `ONCE_GLOBS`
- [ ] `Makefile` — `SKUNK_DIR`-Var, `.PHONY`, `skunkcrafts`-Target (🔧 `--exclude`-Liste)
- [ ] Workflow: `generate.py`-Aufruf vor dem Zip (🔧 `$STAGE`-Pfad)
- [ ] Workflow: „Publish SkunkCrafts update tree"-Step (🔧 `$STAGE` + `rm -rf`)
- [ ] Job hat `permissions: contents: write`
- [ ] Repo-Settings: „Read and write permissions" aktiv
- [ ] Lokal `make skunkcrafts` getestet
- [ ] Erster Tag-Push verifiziert (release-Branch + raw-URL erreichbar)

---

## 10. Stolperfallen

| Problem | Ursache / Fix |
|---|---|
| Updater will 2 GB Modelle hosten | Modelle stehen in der whitelist → in `IGNORE_GLOBS` aufnehmen |
| Modelle werden beim Update gelöscht | Modelle stehen in der **blacklist** → NIE blacklisten, nur ignorieren |
| User-Settings nach Update weg | Datei steht in whitelist/sizeslist (ggf. zusätzlich zur oncelist) → CRC-Diff überschreibt sie. Fix: in `ONCE_GLOBS` und **nur** in die oncelist schreiben, aus whitelist/sizeslist raushalten (Issue #27) |
| Push schlägt fehl (403) | Repo-Workflow-Permissions auf read-only → auf „read and write" stellen |
| Addon erscheint nicht im Updater | `skunkcrafts_updater.cfg` fehlt im Plugin-Root, `disabled` ist `true`, oder `module`-URL nicht erreichbar |
| Falsche/keine Version angezeigt | `@VERSION@` nicht ersetzt → `--version` an `generate.py` prüfen |
| Repo wird mit jedem Release größer | Du committest statt force-push → `git init` + Single-Commit + `--force` nutzen |

---

## Quellen

Protokoll verifiziert an offenen Repos und Doku:
- [SkunkCrafts Updater Forum](https://forums.x-plane.org/forums/forum/406-skunkcrafts-updater/)
- [XoL Protokoll-Recherche](https://github.com/mmaechtel/XoL/blob/master/research/addons/skunkcrafts_updater.md)
- [hotbso/openSAM — cfg.template](https://github.com/hotbso/openSAM/blob/master/skunkcrafts_updater.cfg.template)
- [devleaks/followthegreens — cfg](https://github.com/devleaks/followthegreens/blob/main/skunkcrafts_updater.cfg)
- [Standalone Client v3.2d (2025-02-06)](https://forums.x-plane.org/forums/topic/292710-20250206-skunkcrafts-updater-standalone-client-v32d-available/)
