/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "atc/bzf_compliance.hpp"
#include "atc/atc_templates.hpp"
#include "atc/de_phraseology.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace bzf_compliance {

namespace {

std::string to_lower(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Tower-output side: matchers detect that the controller actually
// passed a readback-mandatory item. We match the raw-digit form
// because atc_state_machine::process() emits raw digits in the
// response text; ziffernweise normalisation runs later in
// atc_session::speak_response(). Patterns are deliberately permissive
// — the goal is to discover what the tower asked the pilot to confirm,
// not to validate the controller's own wording.

// NOTE: Whisper does German compound-word welding on the fly. We have
// observed "Rollhalt Piste" → "Rollhaltpiste", "Endanflug Piste" →
// "Endanflugpiste", and similar in actual transcripts. To stay
// useful as a BZF trainer (rather than punishing pronunciation that
// only Whisper would dislike) the runway matcher therefore drops the
// leading word boundary on "piste" — any token that ENDS in "piste"
// followed by a runway designator counts as a runway mention. The
// QNH / squawk / frequency tokens do not have a known compound-form
// in the German aviation register and keep the stricter boundary.

bool tower_has_qnh(const std::string &lc) {
  // "qnh 1013" / "qnh 998" — case-insensitive
  static const std::regex re(R"(\bqnh\s+\d{3,4}\b)");
  return std::regex_search(lc, re);
}

bool tower_has_runway(const std::string &lc) {
  // Match "piste 25" / "piste 07L" with NO leading boundary, so Whisper-
  // welded compounds like "rollhaltpiste 06" or "endanflugpiste 24"
  // are still recognised. Trailing boundary stays for "06" vs. "067".
  static const std::regex re(R"(piste\s+\d{1,2}[lrc]?\b)");
  if (std::regex_search(lc, re))
    return true;
  // Pilots frequently slip into English on the read-back ("runway 06"),
  // and Whisper transcribes it verbatim. Accept the English form too so a
  // correct read-back isn't flagged as a missing runway.
  static const std::regex re_en(R"(\brunway\s+\d{1,2}[lrc]?\b)");
  return std::regex_search(lc, re_en);
}

bool tower_has_frequency(const std::string &lc) {
  // Either decimal form (118.300) or hand-off phrase ("frequenz 118")
  static const std::regex re(R"(\b\d{3}\.\d{1,3}\b)");
  if (std::regex_search(lc, re))
    return true;
  static const std::regex re2(R"(\bfrequenz\s+\d)");
  return std::regex_search(lc, re2);
}

bool tower_has_squawk(const std::string &lc) {
  static const std::regex re(R"(\bsquawk\s+\d{4}\b)");
  return std::regex_search(lc, re);
}

// Pilot-input side: same regexes — the reverse-normalizer
// (de_phraseology::parse_spoken_number) has already converted
// "QNH eins null eins drei" back to "QNH 1013" by the time the
// transcript reaches the state machine.

bool pilot_has_qnh(const std::string &lc) { return tower_has_qnh(lc); }
bool pilot_has_runway(const std::string &lc) { return tower_has_runway(lc); }
bool pilot_has_frequency(const std::string &lc) {
  return tower_has_frequency(lc);
}
bool pilot_has_squawk(const std::string &lc) { return tower_has_squawk(lc); }

// Normalise known Whisper NATO-letter mistranscriptions before matching.
// Whisper biases the ATC vocabulary prompt (see atc_prompt_templates.json
// :: whisper_prompt) but the general-language prior still wins on
// acoustically-ambiguous NATO letters — most reliably:
//   "Victor"  → "Vector"   (V — User-confirmed 2026-06-05, HB-DSV test)
//   "Juliett" → "Juliet"   (J — single-T is the general-English variant)
//   "Whiskey" → "Wisky"    (W — vowel-elision in fast speech)
//   "X-Ray"   → "Xray"     (X — dash dropped)
// We normalise both sides bidirectionally to a canonical form so the
// match works regardless of which side has the variant. Operates on
// lowercase input; touches whole-word occurrences via space anchors.
void normalize_whisper_nato_variants(std::string &lc) {
  // (from, to) pairs — `from` is the seen variant, `to` is the canonical
  // NfL form. Bidirectional check below tries `from` AND its NfL form
  // so we don't care which side is the canonical one.
  static const std::pair<const char *, const char *> kVariants[] = {
      {"vector", "victor"},
      {"juliet ", "juliett "},
      {"juliet,", "juliett,"},
      {"wisky", "whiskey"},
      {"whiskee", "whiskey"},
      {"xray", "x-ray"},
      {"x ray", "x-ray"},
      // Whisper renders the NfL "Alpha" as the ICAO "Alfa" (f-spelling)
      // about as often as not, so a shortened "... Alfa Bravo" read-back
      // would otherwise miss the stored "... Alpha Bravo" callsign.
      {"alfa", "alpha"},
  };
  for (const auto &[from, to] : kVariants) {
    std::size_t from_len = std::char_traits<char>::length(from);
    std::size_t to_len = std::char_traits<char>::length(to);
    std::size_t pos = lc.find(from);
    while (pos != std::string::npos) {
      lc.replace(pos, from_len, to);
      pos = lc.find(from, pos + to_len);
    }
  }
  // Trailing-token Juliet without punctuation: handle the end-of-string
  // case the space-anchored entries above miss.
  if (lc.size() >= 7 && lc.compare(lc.size() - 7, 7, " juliet") == 0)
    lc.replace(lc.size() - 7, 7, " juliett");
}

// Map a single German spoken-digit word to its numeral char, or '\0' if
// the token is not a digit word. Mirrors de_phraseology::spoken_digit_value
// (file-local there) and intent_parser's kSpokenDigitsDe, kept local to the
// callsign matcher below. Tolerates the BZF "zwei" (vs the radio "zwo") and
// the UTF-8 umlaut form "fünf".
char digit_word_to_numeral(const std::string &tok) {
  static const std::pair<const char *, char> kDigits[] = {
      {"null", '0'},  {"eins", '1'},   {"zwo", '2'},  {"zwei", '2'},
      {"drei", '3'},  {"vier", '4'},   {"fuenf", '5'}, {"f\xc3\xbcnf", '5'},
      {"sechs", '6'}, {"sieben", '7'}, {"acht", '8'}, {"neun", '9'}};
  for (const auto &[word, digit] : kDigits) {
    if (tok == word)
      return digit;
  }
  return '\0';
}

// Canonical form for CALLSIGN comparison: lowercase, NATO-variant unified
// (Alfa/Alpha, Vector/Victor), then tokenised on every non-alphanumeric
// char so each token is either a NATO letter, a numeral, or a spoken-digit
// word — each digit word collapsed to its numeral — and finally welded
// (alphanumerics only). Unlike canon_stripped/parse_spoken_number, this
// unifies a LONE spoken digit: in a phonetic callsign a standalone "eins"
// is always the numeral 1, never a numerus, so the parse_spoken_number
// min-2-run guard does not apply here. Collapses all of "Lima eins",
// "Lima 1", "Charlie, Oscar, Lima" and "November eins zwo drei" /
// "November 123" onto one stream (issue #79 — Voxtral emits numerals +
// per-letter commas where Whisper emits number words).
std::string canon_callsign(const std::string &s) {
  std::string t = to_lower(s);
  normalize_whisper_nato_variants(t);
  std::string out;
  out.reserve(t.size());
  std::size_t i = 0;
  const std::size_t n = t.size();
  while (i < n) {
    if (!std::isalnum(static_cast<unsigned char>(t[i]))) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && std::isalnum(static_cast<unsigned char>(t[j])))
      ++j;
    const std::string tok = t.substr(i, j - i);
    if (char d = digit_word_to_numeral(tok))
      out += d;
    else
      out += tok;
    i = j;
  }
  return out;
}

// Substring match — the pilot's phonetic callsign is multi-word
// ("november eins zwo drei alfa bravo"), and STT may render only a
// subset. We accept any match of at least the LAST TWO TOKENS of the
// phonetic callsign — the BZF-Verkürzung rule from NfL §13 b)
// (Bodenstelle initiates, pilot may follow with first + last two
// characters). Both sides run through canon_callsign() so they survive
// three STT quirks that otherwise raise a false "callsign missing" flag
// (issue #79 — most visible on Mistral/Voxtral but backend-agnostic):
//   1. NATO-letter variants (Alfa/Alpha) — unified.
//   2. A trailing registration digit spoken as a numeral vs a number
//      word: Voxtral emits "Lima 1", the stored callsign carries
//      "Lima eins". canon_callsign() collapses both to "...lima1".
//   3. Commas inserted between phonetic letters ("Charlie, Oscar,
//      Lima") — welded out with all other whitespace/punctuation.
bool pilot_has_callsign(const std::string &pilot_lc,
                        const std::string &callsign_lc) {
  if (callsign_lc.empty())
    return true; // can't check, give the benefit of the doubt
  const std::string p = canon_callsign(pilot_lc);
  const std::string c = canon_callsign(callsign_lc);
  if (c.empty())
    return true;
  // Try the full (welded) callsign first.
  if (p.find(c) != std::string::npos)
    return true;
  // Fall back to the last two spoken tokens of the callsign
  // (BZF-Verkürzung). Split the raw callsign on whitespace BEFORE
  // welding so the token boundary survives, then canonicalise just that
  // tail fragment and look for it in the welded transcript.
  std::size_t sp1 = callsign_lc.rfind(' ');
  if (sp1 == std::string::npos)
    return false;
  std::size_t sp2 = callsign_lc.rfind(' ', sp1 - 1);
  std::string tail =
      callsign_lc.substr(sp2 == std::string::npos ? 0 : sp2 + 1);
  const std::string tail_c = canon_callsign(tail);
  return !tail_c.empty() && p.find(tail_c) != std::string::npos;
}

// Canonical char-stream for value matching: lowercase, spoken digit
// words reverse-normalised to raw digits, Whisper NATO variants unified,
// then every non-alphanumeric char dropped. Stripping whitespace +
// punctuation is what makes the match weld-proof: "Start frei" and
// "startfrei" both collapse to "startfrei", "118.300" to "118300".
std::string canon_stripped(const std::string &s) {
  std::string out = to_lower(s);
  out = de_phraseology::parse_spoken_number(out);
  out = to_lower(out); // parse_spoken_number may re-introduce casing
  normalize_whisper_nato_variants(out);
  std::string stripped;
  stripped.reserve(out.size());
  for (char c : out) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc))
      stripped += static_cast<char>(uc);
  }
  return stripped;
}

// Is the canonicalised clearance value present as a substring of the
// canonicalised pilot transcript? An empty value cannot be checked, so
// give the benefit of the doubt (mirrors pilot_has_callsign).
bool value_covered(const std::string &haystack_canon, const std::string &value) {
  const std::string v = canon_stripped(value);
  if (v.empty())
    return true;
  return haystack_canon.find(v) != std::string::npos;
}

// Spoken-number-normalised, lowercase text WITH whitespace preserved (unlike
// canon_stripped, which welds everything). Needed so the value-capturing
// regexes below can anchor on "qnh 1013" / "piste 06" tokens.
std::string normalize_spoken(const std::string &s) {
  std::string out = to_lower(s);
  out = de_phraseology::parse_spoken_number(out);
  out = to_lower(out); // parse_spoken_number may re-introduce casing
  return out;
}

// Capture the value the pilot actually stated for a given element type from
// the normalised transcript, or "" if no value of that type is present. The
// patterns mirror the tower_has_* detectors but with a capture group. Used
// only to tell Missing (no value stated) from Wrong (a value stated, but the
// wrong digits) — the correctness verdict itself is value_covered's job.
std::string stated_value(Element e, const std::string &norm) {
  std::smatch m;
  switch (e) {
  case Element::QNH: {
    static const std::regex re(R"(qnh\s+(\d{3,4}))");
    if (std::regex_search(norm, m, re))
      return m[1].str();
    return "";
  }
  case Element::Runway: {
    static const std::regex re(R"((?:piste|runway)\s+(\d{1,2}[lrc]?))");
    if (std::regex_search(norm, m, re))
      return m[1].str();
    return "";
  }
  case Element::Frequency: {
    static const std::regex re(R"((\d{3}\.\d{1,3}))");
    if (std::regex_search(norm, m, re))
      return m[1].str();
    static const std::regex re2(R"(frequenz\s+(\d+))");
    if (std::regex_search(norm, m, re2))
      return m[1].str();
    return "";
  }
  case Element::Squawk: {
    static const std::regex re(R"(squawk\s+(\d{4}))");
    if (std::regex_search(norm, m, re))
      return m[1].str();
    return "";
  }
  case Element::Callsign:
    // A callsign has no Wrong verdict — a partial one is Missing, handled by
    // pilot_has_callsign in diff_readback. Never captured here.
    return "";
  }
  return "";
}

// The soll (cleared) value stored in the structured clearance for an element.
std::string expected_value(const ClearanceComponents &comp, Element e) {
  switch (e) {
  case Element::Runway:
    return comp.runway;
  case Element::QNH:
    return comp.qnh;
  case Element::Frequency:
    return comp.frequency;
  case Element::Squawk:
    return comp.squawk;
  case Element::Callsign:
    return comp.callsign;
  }
  return "";
}

// German label for an element in a spoken correction ("QNH", "Piste", ...).
// Overridable via atc_templates.json :: bzf_strict for i18n / hot-reload.
std::string element_label(Element e) {
  switch (e) {
  case Element::Runway:
    return atc_templates::lookup_bzf_strict("label_runway", "Piste");
  case Element::QNH:
    return atc_templates::lookup_bzf_strict("label_qnh", "QNH");
  case Element::Frequency:
    return atc_templates::lookup_bzf_strict("label_frequency", "Frequenz");
  case Element::Squawk:
    return atc_templates::lookup_bzf_strict("label_squawk", "Squawk");
  case Element::Callsign:
    return "";
  }
  return "";
}

// Replace every {callsign} slot in a template with the given callsign.
std::string substitute_callsign(std::string tmpl, const std::string &callsign) {
  const std::string slot = "{callsign}";
  std::size_t pos = tmpl.find(slot);
  while (pos != std::string::npos) {
    tmpl.replace(pos, slot.size(), callsign);
    pos = tmpl.find(slot, pos + callsign.size());
  }
  return tmpl;
}

} // namespace

const char *element_name(Element e) {
  switch (e) {
  case Element::Callsign:
    return "callsign";
  case Element::Runway:
    return "runway";
  case Element::QNH:
    return "qnh";
  case Element::Frequency:
    return "frequency";
  case Element::Squawk:
    return "squawk";
  }
  return "unknown";
}

const char *missing_key(Element e) {
  switch (e) {
  case Element::Callsign:
    return "missing_callsign";
  case Element::Runway:
    return "missing_runway";
  case Element::QNH:
    return "missing_qnh";
  case Element::Frequency:
    return "missing_frequency";
  case Element::Squawk:
    return "missing_squawk";
  }
  return "missing_multi";
}

std::vector<Element> extract_required(const std::string &tower_response) {
  std::vector<Element> out;
  if (tower_response.empty())
    return out;
  const std::string lc = to_lower(tower_response);
  // Callsign is mandatory on every non-empty clearance per §14 c) Nr. 2.
  out.push_back(Element::Callsign);
  if (tower_has_runway(lc))
    out.push_back(Element::Runway);
  if (tower_has_qnh(lc))
    out.push_back(Element::QNH);
  if (tower_has_frequency(lc))
    out.push_back(Element::Frequency);
  if (tower_has_squawk(lc))
    out.push_back(Element::Squawk);
  return out;
}

std::vector<Element> check_pilot_readback(const std::string &pilot_transcript,
                                          const std::vector<Element> &required,
                                          const std::string &pilot_callsign) {
  std::vector<Element> missing;
  const std::string pilot_lc = to_lower(pilot_transcript);
  const std::string cs_lc = to_lower(pilot_callsign);
  for (Element e : required) {
    bool ok = true;
    switch (e) {
    case Element::Callsign:
      ok = pilot_has_callsign(pilot_lc, cs_lc);
      break;
    case Element::Runway:
      ok = pilot_has_runway(pilot_lc);
      break;
    case Element::QNH:
      ok = pilot_has_qnh(pilot_lc);
      break;
    case Element::Frequency:
      ok = pilot_has_frequency(pilot_lc);
      break;
    case Element::Squawk:
      ok = pilot_has_squawk(pilot_lc);
      break;
    }
    if (!ok)
      missing.push_back(e);
  }
  return missing;
}

std::vector<FieldDiff> diff_readback(const ClearanceComponents &comp,
                                     const std::string &pilot_transcript) {
  std::vector<FieldDiff> out;
  const std::string pilot_lc = to_lower(pilot_transcript);
  const std::string pilot_canon = canon_stripped(pilot_transcript);
  const std::string pilot_norm = normalize_spoken(pilot_transcript);
  for (Element e : comp.required) {
    FieldDiff d;
    d.element = e;
    if (e == Element::Callsign) {
      // Callsign has no Wrong verdict — either the (possibly BZF-shortened)
      // callsign is present or it is Missing.
      d.status = pilot_has_callsign(pilot_lc, to_lower(comp.callsign))
                     ? ReadbackStatus::Ok
                     : ReadbackStatus::Missing;
      out.push_back(d);
      continue;
    }
    d.expected = expected_value(comp, e);
    if (value_covered(pilot_canon, d.expected)) {
      d.status = ReadbackStatus::Ok; // covered — semantics unchanged
    } else {
      // Not covered: distinguish an omitted element from a wrong value.
      std::string stated = stated_value(e, pilot_norm);
      if (stated.empty()) {
        d.status = ReadbackStatus::Missing;
      } else {
        d.status = ReadbackStatus::Wrong;
        d.stated = stated;
      }
    }
    out.push_back(d);
  }
  return out;
}

std::vector<Element> missing_readback_elements(const ClearanceComponents &comp,
                                               const std::string &pilot_transcript) {
  // Thin wrapper over diff_readback: every element that is not Ok (Missing or
  // Wrong) is "not read back" for the completeness gate. Ok is decided by
  // value_covered exactly as before, so behaviour is unchanged.
  std::vector<Element> missing;
  for (const auto &d : diff_readback(comp, pilot_transcript))
    if (d.status != ReadbackStatus::Ok)
      missing.push_back(d.element);
  return missing;
}

bool readback_covers_core(const std::vector<Element> &required,
                          const std::vector<Element> &missing) {
  auto covered = [&](Element e) {
    return std::find(required.begin(), required.end(), e) != required.end() &&
           std::find(missing.begin(), missing.end(), e) == missing.end();
  };
  // Callsign is the anchor — a readback without it isn't a readback.
  if (!covered(Element::Callsign))
    return false;
  // If the clearance carried no fact element at all, a covered callsign
  // is the whole readback.
  bool has_fact_element =
      std::any_of(required.begin(), required.end(),
                  [](Element e) { return e != Element::Callsign; });
  if (!has_fact_element)
    return true;
  // Otherwise demand at least one fact element — callsign alone must not
  // trigger a (phantom) readback acceptance.
  return covered(Element::Runway) || covered(Element::QNH) ||
         covered(Element::Frequency) || covered(Element::Squawk);
}

std::string build_correction_response(const std::string &callsign,
                                      const std::vector<Element> &missing) {
  if (missing.empty())
    return "";
  // Single missing element -> element-specific template.
  // Two or more  -> generic "vollstaendig wiederholen" template.
  const char *tmpl_key =
      (missing.size() == 1) ? missing_key(missing.front()) : "missing_multi";
  // Default text matches the NfL §18 wording in case the JSON block is
  // absent (e.g. EU/US profile with strict mode flipped on by accident).
  std::string tmpl = atc_templates::lookup_bzf_strict(
      tmpl_key, std::string("{callsign}, wiederholen Sie die vollstaendige "
                            "Freigabe."));
  // Substitute {callsign} — keep it simple, no general-purpose render
  // (atc_templates::fill is overkill for one slot).
  const std::string slot = "{callsign}";
  std::size_t pos = tmpl.find(slot);
  while (pos != std::string::npos) {
    tmpl.replace(pos, slot.size(), callsign);
    pos = tmpl.find(slot, pos + callsign.size());
  }
  return tmpl;
}

std::string build_correction_response(const std::string &callsign,
                                      const ClearanceComponents &,
                                      const std::vector<FieldDiff> &diff) {
  const bool any_wrong = std::any_of(diff.begin(), diff.end(), [](const FieldDiff &d) {
    return d.status == ReadbackStatus::Wrong;
  });
  const bool any_bad = std::any_of(diff.begin(), diff.end(), [](const FieldDiff &d) {
    return d.status != ReadbackStatus::Ok;
  });
  if (!any_bad)
    return "";

  // Incomplete-only readback (no wrong values): NfL standard READ BACK
  // instruction — the tower does NOT re-state the value (Prüfungsfrage 65 /
  // glossary READ BACK = "WIEDERHOLEN SIE WOERTLICH").
  if (!any_wrong) {
    std::string tmpl = atc_templates::lookup_bzf_strict(
        "read_back", "{callsign}, WIEDERHOLEN SIE WOERTLICH.");
    return substitute_callsign(tmpl, callsign);
  }

  // At least one wrong value: the tower actively corrects the discrepancy
  // (NfL §25 b) Nr. 3) and names each correct soll value, then demands a
  // verbatim readback:
  //   "{callsign}, NEGATIV, QNH 1013, Piste 25, WIEDERHOLEN SIE WOERTLICH."
  const std::string lead = substitute_callsign(
      atc_templates::lookup_bzf_strict("negativ_lead", "{callsign}, NEGATIV,"),
      callsign);
  const std::string tail = atc_templates::lookup_bzf_strict(
      "read_back_tail", "WIEDERHOLEN SIE WOERTLICH.");
  std::string body;
  for (const FieldDiff &d : diff) {
    if (d.status != ReadbackStatus::Wrong)
      continue;
    if (!body.empty())
      body += ", ";
    body += element_label(d.element) + " " + d.expected;
  }
  return lead + " " + body + ", " + tail;
}

} // namespace bzf_compliance
