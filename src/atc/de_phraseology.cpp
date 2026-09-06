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

#include "atc/de_phraseology.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace de_phraseology {

namespace {

// BZF ziffern: "2" is "zwo" (mandatory, distinguishes from "drei"),
// "5" is "fuenf" (ASCII, no umlaut — matches the M2 templates).
constexpr std::array<const char *, 10> kDigitWords = {
    "null",  "eins",  "zwo",    "drei", "vier",
    "fuenf", "sechs", "sieben", "acht", "neun"};

// BZF / ICAO NATO phonetic alphabet. Only used for D-prefix callsign
// expansion; the M3 scope does not include a wholesale NATO swap of
// freely-occurring letter words.
constexpr std::array<const char *, 26> kNatoLetters = {
    "Alfa",   "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
    "Hotel",  "India",   "Juliett", "Kilo",   "Lima",   "Mike",    "November",
    "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
    "Victor", "Whiskey", "X-Ray",   "Yankee", "Zulu"};

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
inline bool is_alpha(char c) { return is_upper(c) || is_lower(c); }
inline bool is_word_char(char c) { return is_alpha(c) || is_digit(c); }

// Word-boundary check: position i is at a word boundary if it is the
// start of the string or the preceding char is not a word char.
inline bool boundary_before(const std::string &s, std::size_t i) {
  return i == 0 || !is_word_char(s[i - 1]);
}

// Word-boundary check after position i (exclusive): true if i is past
// end-of-string or s[i] is not a word char.
inline bool boundary_after(const std::string &s, std::size_t i) {
  return i >= s.size() || !is_word_char(s[i]);
}

// Scan a contiguous digit run starting at pos. Returns the end position
// (exclusive). The number itself is s.substr(pos, end-pos).
std::size_t scan_digits(const std::string &s, std::size_t pos) {
  std::size_t end = pos;
  while (end < s.size() && is_digit(s[end]))
    ++end;
  return end;
}

// Render raw digits as ziffernweise: "25" -> "zwo fuenf".
std::string ziffernweise(const std::string &digits) {
  std::string out;
  bool first = true;
  for (char c : digits) {
    if (!is_digit(c))
      continue;
    if (!first)
      out += ' ';
    out += kDigitWords[c - '0'];
    first = false;
  }
  return out;
}

// Render integer N as "tausend"/"hundert" form when clean. Returns
// empty string for unsupported shapes (N >= 10000, mixed digits like
// 3050). Examples:
//   1000 -> "eins tausend"
//   2000 -> "zwo tausend"
//   3500 -> "drei tausend fuenfhundert"
//   1100 -> "eins tausend einhundert"
//   3050 -> "" (fall back to caller)
std::string thousand_form(int n) {
  if (n < 1000 || n > 9999)
    return {};
  int thousands = n / 1000;
  int remainder = n % 1000;
  std::string out = kDigitWords[thousands];
  out += " tausend";
  if (remainder == 0)
    return out;
  if (remainder % 100 != 0)
    return {}; // mixed digits — caller falls back
  int hundreds = remainder / 100;
  out += ' ';
  out += kDigitWords[hundreds];
  out += "hundert";
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 1: D-XXXX callsigns
// "D-EXYZ" -> "Delta Echo X-Ray Yankee Zulu"
// Triggers on "D-" at word boundary, followed by 2..5 uppercase letters
// terminated by a word boundary.
// ────────────────────────────────────────────────────────────────────
std::string expand_d_callsigns(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 32);
  std::size_t i = 0;
  while (i < s.size()) {
    bool match = false;
    if (i + 2 < s.size() && s[i] == 'D' && s[i + 1] == '-' &&
        boundary_before(s, i)) {
      std::size_t letter_start = i + 2;
      std::size_t letter_end = letter_start;
      while (letter_end < s.size() && is_upper(s[letter_end]))
        ++letter_end;
      std::size_t n_letters = letter_end - letter_start;
      if (n_letters >= 2 && n_letters <= 5 && boundary_after(s, letter_end)) {
        // Render: "Delta " + nato(letters[0]) + " " + nato(letters[1]) + ...
        out += "Delta";
        for (std::size_t k = letter_start; k < letter_end; ++k) {
          out += ' ';
          out += kNatoLetters[s[k] - 'A'];
        }
        i = letter_end;
        match = true;
      }
    }
    if (!match) {
      out += s[i];
      ++i;
    }
  }
  return out;
}

// Helper: scan past a keyword (literal substring) starting at position i,
// requiring a word boundary before. Returns position right after the
// keyword if matched, or std::string::npos if not.
std::size_t match_keyword(const std::string &s, std::size_t i, const char *kw) {
  std::size_t klen = 0;
  while (kw[klen])
    ++klen;
  if (i + klen > s.size())
    return std::string::npos;
  if (!boundary_before(s, i))
    return std::string::npos;
  for (std::size_t k = 0; k < klen; ++k) {
    if (s[i + k] != kw[k])
      return std::string::npos;
  }
  return i + klen;
}

// ────────────────────────────────────────────────────────────────────
// Pass 2: "Piste <NN>[LRC]" with optional German suffix
// "Piste 25"        -> "Piste zwo fuenf"
// "Piste 07L"       -> "Piste null sieben links"
// "Piste 25 links"  -> "Piste zwo fuenf links" (no double-translate)
// ────────────────────────────────────────────────────────────────────
std::string expand_runways(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t after_kw = match_keyword(s, i, "Piste ");
    if (after_kw == std::string::npos) {
      out += s[i];
      ++i;
      continue;
    }
    std::size_t digit_end = scan_digits(s, after_kw);
    std::size_t n_digits = digit_end - after_kw;
    if (n_digits < 1 || n_digits > 2) {
      // No usable digit run — copy "Piste " literally and continue past it
      // so we don't loop forever on the same position.
      out.append(s, i, after_kw - i);
      i = after_kw;
      continue;
    }
    out += "Piste ";
    out += ziffernweise(s.substr(after_kw, n_digits));
    std::size_t cursor = digit_end;
    // Optional immediate L/R/C suffix
    if (cursor < s.size()) {
      char c = s[cursor];
      if ((c == 'L' || c == 'R' || c == 'C') && boundary_after(s, cursor + 1)) {
        if (c == 'L')
          out += " links";
        else if (c == 'R')
          out += " rechts";
        else
          out += " mitte";
        cursor += 1;
      }
    }
    i = cursor;
  }
  return out;
}

// Generic helper: expand digits following a keyword. After the digits,
// optionally append a unit string if not already present in the next
// few characters. `min_digits`/`max_digits` bound the digit run length.
// If `pad_to` > 0, the digits are zero-padded on the left to that width
// before being spelled (e.g. "Steuerkurs 50" -> "null fuenf null").
std::string expand_keyword_digits(const std::string &s, const char *keyword,
                                  int min_digits, int max_digits, int pad_to,
                                  const char *unit_to_append) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t kw_len = 0;
  while (keyword[kw_len])
    ++kw_len;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t after_kw = match_keyword(s, i, keyword);
    if (after_kw == std::string::npos) {
      out += s[i];
      ++i;
      continue;
    }
    std::size_t digit_end = scan_digits(s, after_kw);
    int n_digits = static_cast<int>(digit_end - after_kw);
    if (n_digits < min_digits || n_digits > max_digits) {
      out.append(s, i, after_kw - i);
      i = after_kw;
      continue;
    }
    std::string digits = s.substr(after_kw, n_digits);
    if (pad_to > n_digits)
      digits.insert(static_cast<std::size_t>(0),
                    static_cast<std::size_t>(pad_to - n_digits), '0');
    out.append(keyword, kw_len);
    out += ziffernweise(digits);
    if (unit_to_append && *unit_to_append) {
      // Append unit only if it does not already follow (idempotency for
      // unit phrases like "QNH 1013 Hektopascal" — already present, do
      // not double-append).
      std::size_t unit_len = 0;
      while (unit_to_append[unit_len])
        ++unit_len;
      bool already = false;
      // Skip one leading space when checking; the unit is appended with
      // a leading space.
      if (digit_end + 1 + unit_len <= s.size() && s[digit_end] == ' ') {
        already = true;
        for (std::size_t k = 0; k < unit_len; ++k) {
          if (s[digit_end + 1 + k] != unit_to_append[k]) {
            already = false;
            break;
          }
        }
        if (already && !boundary_after(s, digit_end + 1 + unit_len))
          already = false;
      }
      if (!already) {
        out += ' ';
        out += unit_to_append;
      }
    }
    i = digit_end;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 5: Frequencies "NNN.NNN" -> "<digits> Komma <digits>"
// No keyword anchor — the decimal point alone is the signal. Scans for
// a digit run, dot, digit run with sane widths (2..3 . 1..3).
// ────────────────────────────────────────────────────────────────────
std::string expand_frequencies(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t a_end = scan_digits(s, i);
      std::size_t a_len = a_end - i;
      if (a_len >= 2 && a_len <= 3 && a_end < s.size() && s[a_end] == '.') {
        std::size_t b_start = a_end + 1;
        std::size_t b_end = scan_digits(s, b_start);
        std::size_t b_len = b_end - b_start;
        if (b_len >= 1 && b_len <= 3 && boundary_after(s, b_end)) {
          out += ziffernweise(s.substr(i, a_len));
          out += " Komma ";
          out += ziffernweise(s.substr(b_start, b_len));
          i = b_end;
          continue;
        }
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 6: Wind "<NNN> Grad <N..NNN> Knoten" -> ziffernweise both
// numbers; "Grad"/"Knoten" stay.
// ────────────────────────────────────────────────────────────────────
std::string expand_wind(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t a_end = scan_digits(s, i);
      std::size_t a_len = a_end - i;
      if (a_len >= 1 && a_len <= 3 && a_end + 5 < s.size() &&
          s.compare(a_end, 5, " Grad") == 0 &&
          (a_end + 5 == s.size() || !is_word_char(s[a_end + 5]))) {
        // Direction matches; now look for " <digits> Knoten"
        std::size_t after_grad = a_end + 5;
        // skip any whitespace
        std::size_t b_start = after_grad;
        while (b_start < s.size() && s[b_start] == ' ')
          ++b_start;
        if (b_start < s.size() && is_digit(s[b_start])) {
          std::size_t b_end = scan_digits(s, b_start);
          std::size_t b_len = b_end - b_start;
          if (b_len >= 1 && b_len <= 3 && b_end + 7 <= s.size() &&
              s.compare(b_end, 7, " Knoten") == 0 &&
              (b_end + 7 == s.size() || !is_word_char(s[b_end + 7]))) {
            out += ziffernweise(s.substr(i, a_len));
            out += " Grad ";
            out += ziffernweise(s.substr(b_start, b_len));
            out += " Knoten";
            i = b_end + 7;
            continue;
          }
        }
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 7: Altitudes "<digits> Fuss" or "<digits> Meter"
// Tausend/hundert form when clean (1000, 1500, 2000, 3500, ...);
// otherwise ziffernweise.
// ────────────────────────────────────────────────────────────────────
std::string expand_altitudes(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t d_end = scan_digits(s, i);
      std::size_t d_len = d_end - i;
      const char *unit = nullptr;
      std::size_t unit_len = 0;
      if (d_end + 5 <= s.size() && s.compare(d_end, 5, " Fuss") == 0 &&
          (d_end + 5 == s.size() || !is_word_char(s[d_end + 5]))) {
        unit = "Fuss";
        unit_len = 4;
      } else if (d_end + 6 <= s.size() && s.compare(d_end, 6, " Meter") == 0 &&
                 (d_end + 6 == s.size() || !is_word_char(s[d_end + 6]))) {
        unit = "Meter";
        unit_len = 5;
      }
      if (unit && d_len >= 1 && d_len <= 5) {
        std::string digits = s.substr(i, d_len);
        int n = 0;
        for (char c : digits)
          n = n * 10 + (c - '0');
        std::string tf = thousand_form(n);
        if (!tf.empty()) {
          out += tf;
        } else {
          out += ziffernweise(digits);
        }
        out += ' ';
        out += unit;
        i = d_end + 1 + unit_len;
        continue;
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 8: Clock position "<N|NN> Uhr" (1..12)
// ────────────────────────────────────────────────────────────────────
std::string expand_clock(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  std::size_t i = 0;
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t d_end = scan_digits(s, i);
      std::size_t d_len = d_end - i;
      if (d_len >= 1 && d_len <= 2 && d_end + 4 <= s.size() &&
          s.compare(d_end, 4, " Uhr") == 0 &&
          (d_end + 4 == s.size() || !is_word_char(s[d_end + 4]))) {
        std::string digits = s.substr(i, d_len);
        int n = 0;
        for (char c : digits)
          n = n * 10 + (c - '0');
        if (n >= 1 && n <= 12) {
          out += ziffernweise(digits);
          out += " Uhr";
          i = d_end + 4;
          continue;
        }
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 9: Sequence number "Nummer <digit>"
// Only 1-digit values are realistic for a landing-sequence position;
// 2-digit values are theoretically possible but the M2 templates
// already render "Nummer eins" as a literal word for the common
// case — we only normalize when the variable was filled with a digit.
// ────────────────────────────────────────────────────────────────────
std::string expand_sequence(const std::string &s) {
  return expand_keyword_digits(s, "Nummer ", 1, 2, 0, nullptr);
}

// ────────────────────────────────────────────────────────────────────
// Pass: restore UTF-8 umlauts on a curated word list. The DE templates
// and the rest of this normalizer emit ASCII stand-ins ("fuenf",
// "hoeren", "ueber") to keep XPLMDebugString / ImGui safe. Cloud TTS
// (Mistral Voxtral, OpenAI tts-1) however reads "hoeren" as
// "h-o-e-r-e-n" because the digraphs are not pronunciation tokens —
// sounds like "ein Chinese der Deutsch spricht und die Silben
// verschluckt" (user feedback). Substituting "hören" yields the
// proper /ˈhøːʁən/ phoneme.
//
// Substitution is word-boundary aware (no inside-word matches) and
// case-sensitive — both upper- and lower-case forms must be listed
// explicitly. Idempotent: a second pass finds the UTF-8 forms in the
// output and matches nothing (the source column is pure ASCII).
//
// This pass runs ONLY in the TTS pipeline (via normalize_for_speech).
// The original ASCII text is what the transcript / Log.txt show, so
// [[feedback_xplane_log_ascii]] stays intact.
// ────────────────────────────────────────────────────────────────────
std::string restore_umlaute(const std::string &s) {
  // Curated map: every entry must be a complete ASCII word that appears
  // in DE templates or in normalize_for_speech output. Longer compounds
  // before their roots so a longest-match-first walk picks them up
  // before the shorter prefix steals the boundary.
  struct Entry {
    const char *ascii;
    const char *utf8;
  };
  static const std::array<Entry, 22> kMap = {{
      // longest first
      {"Hoehenmessereinstellung", "Höhenmessereinstellung"},
      {"Platzrundenhoehe", "Platzrundenhöhe"},
      {"Wiederhoeren", "Wiederhören"},
      {"fuenfhundert", "fünfhundert"},
      {"vollstaendige", "vollständige"},
      {"zurueckliest", "zurückliest"},
      {"bestaetigen", "bestätigen"},
      {"Bestaetigen", "Bestätigen"},
      {"vollstaendig", "vollständig"},
      {"Vollstaendig", "Vollständig"},
      {"zurueck", "zurück"},
      {"Zurueck", "Zurück"},
      {"hoeren", "hören"},
      {"Hoeren", "Hören"},
      {"Hoehen", "Höhen"},
      {"Hoehe", "Höhe"},
      {"fuenf", "fünf"},
      {"Fuenf", "Fünf"},
      {"ueber", "über"},
      {"Ueber", "Über"},
      {"Fuss", "Fuß"},
      {"fuer", "für"},
  }};

  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    bool matched = false;
    for (const auto &e : kMap) {
      std::size_t klen = 0;
      while (e.ascii[klen])
        ++klen;
      if (i + klen > s.size())
        continue;
      if (!boundary_before(s, i))
        continue;
      bool eq = true;
      for (std::size_t k = 0; k < klen; ++k) {
        if (s[i + k] != e.ascii[k]) {
          eq = false;
          break;
        }
      }
      if (!eq)
        continue;
      if (!boundary_after(s, i + klen))
        continue;
      out += e.utf8;
      i += klen;
      matched = true;
      break;
    }
    if (!matched) {
      out += s[i];
      ++i;
    }
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Pass 10: "Alpha" -> "Alfa" at word boundary, only when preceded by
// "Information " (the only place Alpha legitimately appears in DE
// templates today is as the ATIS information letter).
// ────────────────────────────────────────────────────────────────────
std::string swap_information_alpha(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  const std::string anchor = "Information Alpha";
  std::size_t i = 0;
  while (i < s.size()) {
    if (i + anchor.size() <= s.size() &&
        s.compare(i, anchor.size(), anchor) == 0 && boundary_before(s, i) &&
        boundary_after(s, i + anchor.size())) {
      out += "Information Alfa";
      i += anchor.size();
      continue;
    }
    out += s[i];
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// Reverse normalizer helpers (M7)
// ────────────────────────────────────────────────────────────────────

// Lookup spoken digit word -> 0..9; -1 if not a digit word. Accepts
// case-insensitive "Zwo", "ZWO", "zwo" and the BZF synonym "zwei".
// Also accepts the UTF-8 umlaut form "fünf" so Debug-Texteingabe input
// like "Piste zwo fünf" parses identically to "Piste zwo fuenf".
int spoken_digit_value(const std::string &lc_word) {
  // Order matches kDigitWords (0..9). "zwei" is BZF-tolerant synonym for "zwo".
  static const std::array<const char *, 10> words = {
      "null",  "eins",  "zwo",    "drei", "vier",
      "fuenf", "sechs", "sieben", "acht", "neun"};
  for (int i = 0; i < 10; ++i)
    if (lc_word == words[static_cast<std::size_t>(i)])
      return i;
  if (lc_word == "zwei")
    return 2;
  if (lc_word == "fünf")
    return 5;
  return -1;
}

// Lowercase one ASCII word.
std::string to_lower_word(const std::string &w) {
  std::string out;
  out.reserve(w.size());
  for (char c : w) {
    if (c >= 'A' && c <= 'Z')
      out += static_cast<char>(c + ('a' - 'A'));
    else
      out += c;
  }
  return out;
}

// Tokenize on whitespace. Returns (token, original_start, original_end)
// triples. Punctuation is kept as part of the token; the caller strips
// trailing punctuation when comparing.
struct Token {
  std::string raw;      // original casing + punctuation
  std::string lc_word;  // lowercased, trailing punctuation stripped
  std::string trailing; // punctuation after the word (",", ".", etc.)
  std::size_t start;    // index in original string
  std::size_t end;      // one past last char in original string
};

std::vector<Token> tokenize(const std::string &s) {
  std::vector<Token> out;
  std::size_t i = 0;
  while (i < s.size()) {
    // skip whitespace
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
      ++i;
    if (i >= s.size())
      break;
    std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n')
      ++i;
    Token t;
    t.raw = s.substr(start, i - start);
    t.start = start;
    t.end = i;
    // split trailing punctuation off the word body for the lc_word
    std::size_t body_end = t.raw.size();
    while (body_end > 0) {
      char c = t.raw[body_end - 1];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9'))
        break;
      --body_end;
    }
    std::string body = t.raw.substr(0, body_end);
    t.trailing = t.raw.substr(body_end);
    t.lc_word = to_lower_word(body);
    out.push_back(std::move(t));
  }
  return out;
}

// True if any character of the token body is a raw digit. Used to
// preserve idempotency: already-numeric tokens are passed through.
bool token_body_has_digit(const Token &t) {
  for (char c : t.lc_word)
    if (c >= '0' && c <= '9')
      return true;
  return false;
}

// Greedy run collector: from position `i`, walk forward while tokens are
// pure spoken digit words. Returns (digits, run_length). digits is the
// concatenated number; run_length is how many tokens were consumed.
// Cap: collect at most max_digits. The run is ONLY recognized if it
// contains >= min_digits tokens.
struct DigitRun {
  std::string digits;
  std::size_t length; // number of consumed tokens
};

DigitRun collect_digit_run(const std::vector<Token> &tok, std::size_t i,
                           std::size_t min_digits, std::size_t max_digits) {
  DigitRun r{"", 0};
  std::size_t j = i;
  while (j < tok.size() && r.digits.size() < max_digits) {
    int v = spoken_digit_value(tok[j].lc_word);
    if (v < 0)
      break;
    r.digits += static_cast<char>('0' + v);
    r.length += 1;
    // Stop a run on a token that has trailing punctuation other than
    // none -- "eins null, eins drei" is two runs, not one.
    if (!tok[j].trailing.empty()) {
      ++j;
      break;
    }
    ++j;
  }
  if (r.digits.size() < min_digits) {
    r.digits.clear();
    r.length = 0;
  }
  return r;
}

// Append a digit string to the output as a single token, carrying the
// trailing punctuation of the last consumed token.
void emit_digits(std::string &out, const std::string &digits,
                 const std::string &trailing) {
  if (!out.empty() && out.back() != ' ')
    out += ' ';
  out += digits;
  out += trailing;
}

// Append a frequency "AA.BBB" with optional trailing punctuation.
void emit_frequency(std::string &out, const std::string &left,
                    const std::string &right, const std::string &trailing) {
  if (!out.empty() && out.back() != ' ')
    out += ' ';
  out += left;
  out += '.';
  out += right;
  out += trailing;
}

// Is the previous token a known anchor keyword that permits a
// single-digit digit run? "Piste 7", "QNH 1013" works either way, but
// "Piste 7" needs single-digit acceptance. Free-floating digit runs
// stay min=2 to avoid false positives on numeric words like "eins"
// used as a numerus ("die eins Achse").
bool prev_token_is_run_anchor(const std::vector<Token> &tok, std::size_t i) {
  if (i == 0)
    return false;
  const std::string &prev = tok[i - 1].lc_word;
  static const std::array<const char *, 12> anchors = {
      "piste", "qnh", "steuerkurs", "runway", "frequenz", "wind",
      "auf",   "in",  "ueber",      "nummer", "verkehr",  "information"};
  for (auto a : anchors)
    if (prev == a)
      return true;
  return false;
}

// Case-insensitive token-body match.
bool tok_eq(const Token &t, const char *kw) {
  std::size_t k = 0;
  while (kw[k]) {
    if (k >= t.lc_word.size())
      return false;
    char c = kw[k];
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
    if (t.lc_word[k] != c)
      return false;
    ++k;
  }
  return k == t.lc_word.size();
}

std::string parse_spoken_number_impl(const std::string &text) {
  if (text.empty())
    return text;
  auto tok = tokenize(text);
  if (tok.empty())
    return text;

  std::string out;
  out.reserve(text.size());

  std::size_t i = 0;
  while (i < tok.size()) {
    // Skip tokens that already contain raw digits -- preserves
    // idempotency: parse_spoken_number(parse_spoken_number(s)) == once(s).
    if (token_body_has_digit(tok[i])) {
      if (!out.empty() && out.back() != ' ')
        out += ' ';
      out += tok[i].raw;
      ++i;
      continue;
    }

    // ── Frequency pattern: <2..3 digits> "komma" <2..3 digits> ────────
    // The frequency probe always requires 2..3 digits on the left
    // (single-digit MHz prefixes are not aviation-real). General digit
    // runs allow min=1 after a known anchor keyword (Piste 7) or min=2
    // free-floating to avoid grabbing single Numerus words.
    DigitRun freq_probe = collect_digit_run(tok, i, 2, 3);
    if (freq_probe.length > 0) {
      std::size_t after_left = i + freq_probe.length;
      bool prev_had_trailing = !tok[after_left - 1].trailing.empty();
      if (!prev_had_trailing && after_left < tok.size() &&
          tok_eq(tok[after_left], "komma") &&
          tok[after_left].trailing.empty()) {
        DigitRun right = collect_digit_run(tok, after_left + 1, 2, 3);
        if (right.length > 0) {
          std::size_t end = after_left + 1 + right.length;
          emit_frequency(out, freq_probe.digits, right.digits,
                         tok[end - 1].trailing);
          i = end;
          continue;
        }
      }
    }

    // ── Generic digit run ─────────────────────────────────────────────
    std::size_t min_digits = prev_token_is_run_anchor(tok, i) ? 1 : 2;
    DigitRun run = collect_digit_run(tok, i, min_digits, 4);
    if (run.length > 0) {
      std::size_t after = i + run.length;
      emit_digits(out, run.digits, tok[after - 1].trailing);
      i = after;
      continue;
    }

    // ── Single digit word fallthrough: keep as word ───────────────────
    // Single isolated digit words (e.g. "eins" as Numerus) are NOT
    // rewritten -- collect_digit_run with min=2 already returned empty.
    if (!out.empty() && out.back() != ' ')
      out += ' ';
    out += tok[i].raw;
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// De-shout pass (issue #62): lowercase ALL-CAPS phraseology words before
// they reach the synthesizer. BZF/NfL writes ATC phraseology in uppercase
// ("WIEDERHOLEN SIE WOERTLICH", "NEGATIV", "ROLLEN"); a language-agnostic
// moderation "shouting/aggression" heuristic (Mistral's TTS guardrail)
// reads sustained uppercase as hostile and 403-blocks the request.
// Lowercasing the SPOKEN form is pronunciation-neutral and defuses the
// trigger — the transcript / Log.txt still show the original uppercase,
// because this runs only in the TTS pipeline (via normalize_for_speech).
//
// UTF-8 aware for the three German uppercase umlauts: it runs AFTER
// restore_umlaute, and while that pass only emits Title/lowercase umlaut
// forms today (so all-caps tokens reaching here are still ASCII), the
// uppercase-umlaut handling keeps this pass correct if that ever changes.
// Acronyms in kKeepUpper stay uppercase so the TTS still articulates them
// (they are spoken as initialisms / precede ziffernweise digits). A word
// qualifies as "shouting" only when it has >= 2 letters, at least one
// uppercase letter, and NO lowercase letter — so Title-case words
// (Charlie, Hektopascal) and number-words (eins null) are untouched.
// Idempotent.
// ────────────────────────────────────────────────────────────────────
std::string soften_caps_for_speech(const std::string &s) {
  static const std::array<const char *, 11> kKeepUpper = {
      {"QNH", "VFR", "IFR", "ATIS", "ILS", "UTC", "RMZ", "TMZ", "CTR", "TMA",
       "CTA"}};

  // Second UTF-8 byte after a 0xC3 lead byte, for the German umlauts.
  auto is_upper_umlaut = [](unsigned char b) {
    return b == 0x96 || b == 0x84 || b == 0x9C; // Oe Ae Ue
  };
  auto is_lower_umlaut = [](unsigned char b) {
    return b == 0xB6 || b == 0xA4 || b == 0xBC || b == 0x9F; // oe ae ue ss
  };
  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };

  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    if (is_space(s[i])) {
      out += s[i++];
      continue;
    }
    // One token = a maximal run of non-whitespace bytes.
    const std::size_t start = i;
    while (i < n && !is_space(s[i]))
      ++i;
    const std::string tok = s.substr(start, i - start);

    bool has_upper = false, has_lower = false;
    int letters = 0;
    for (std::size_t k = 0; k < tok.size();) {
      const unsigned char c = static_cast<unsigned char>(tok[k]);
      if (c == 0xC3 && k + 1 < tok.size()) {
        const unsigned char d = static_cast<unsigned char>(tok[k + 1]);
        if (is_upper_umlaut(d)) {
          has_upper = true;
          ++letters;
        } else if (is_lower_umlaut(d)) {
          has_lower = true;
          ++letters;
        }
        k += 2;
        continue;
      }
      if (is_upper(tok[k])) {
        has_upper = true;
        ++letters;
      } else if (is_lower(tok[k])) {
        has_lower = true;
        ++letters;
      }
      ++k;
    }

    const bool all_caps = has_upper && !has_lower && letters >= 2;
    bool keep = false;
    if (all_caps) {
      // Compare the token's ASCII letter/digit core against the acronym
      // allowlist (acronyms are pure ASCII — no umlauts).
      std::string core;
      for (char ch : tok)
        if (is_upper(ch) || is_digit(ch))
          core += ch;
      for (const char *k2 : kKeepUpper)
        if (core == k2) {
          keep = true;
          break;
        }
    }

    if (!all_caps || keep) {
      out += tok;
      continue;
    }
    for (std::size_t k = 0; k < tok.size();) {
      const unsigned char c = static_cast<unsigned char>(tok[k]);
      if (c == 0xC3 && k + 1 < tok.size() &&
          is_upper_umlaut(static_cast<unsigned char>(tok[k + 1]))) {
        out += static_cast<char>(0xC3);
        out += static_cast<char>(static_cast<unsigned char>(tok[k + 1]) + 0x20);
        k += 2;
        continue;
      }
      out += is_upper(tok[k]) ? static_cast<char>(tok[k] + ('a' - 'A')) : tok[k];
      ++k;
    }
  }
  return out;
}

} // namespace

std::string normalize_for_speech(const std::string &text) {
  if (text.empty())
    return text;
  std::string s = text;
  // Order: callsigns first (their letters must not be mistaken for
  // anything else); frequencies before any digit pattern that could
  // partially match the integer half; wind before generic altitude
  // since both feature multi-digit numbers but only wind has the
  // "Grad ... Knoten" anchor pair.
  s = expand_d_callsigns(s);
  s = expand_runways(s);
  s = expand_keyword_digits(s, "QNH ", 3, 4, 0, "Hektopascal");
  s = expand_keyword_digits(s, "Steuerkurs ", 1, 3, 3, nullptr);
  s = expand_frequencies(s);
  s = expand_wind(s);
  s = expand_altitudes(s);
  s = expand_clock(s);
  s = expand_sequence(s);
  s = swap_information_alpha(s);
  // ASCII -> UTF-8 umlaut restoration on a curated word list. TTS-only —
  // the original ASCII output above is what callers see when they invoke
  // this directly; the umlaut pass produces the string that actually goes
  // to backends::tts::synthesize_async.
  s = restore_umlaute(s);
  // Last pass: de-shout ALL-CAPS phraseology so a moderation "shouting"
  // heuristic cannot 403-block the spoken form (issue #62).
  s = soften_caps_for_speech(s);
  return s;
}

std::string parse_spoken_number(const std::string &text) {
  return parse_spoken_number_impl(text);
}

std::string expand_callsign_phonetic(const std::string &raw) {
  std::string out;
  out.reserve(raw.size() * 6);
  for (char c : raw) {
    const char *word = nullptr;
    if (is_upper(c))
      word = kNatoLetters[c - 'A'];
    else if (is_lower(c))
      word = kNatoLetters[c - 'a'];
    else if (is_digit(c))
      word = kDigitWords[c - '0'];
    if (!word)
      continue; // skip dashes, spaces, punctuation
    if (!out.empty())
      out += ' ';
    out += word;
  }
  return out;
}

// Reverse lookup for a single spoken NATO letter word -> uppercase letter.
// Returns '\0' when the token is not a recognized NATO letter word. Tolerant
// of dash stripping (x-ray) and common variants (alpha, juliet).
static char nato_word_to_letter(const std::string &w) {
  std::string key;
  for (char c : w)
    if (c != '-')
      key += c;
  for (std::size_t i = 0; i < kNatoLetters.size(); ++i) {
    std::string lw;
    for (const char *p = kNatoLetters[i]; *p; ++p) {
      char c = *p;
      if (c == '-')
        continue;
      lw += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    if (key == lw)
      return static_cast<char>('A' + i);
  }
  if (key == "alpha")
    return 'A';
  if (key == "juliet")
    return 'J';
  return '\0';
}

std::string parse_spoken_icao(const std::string &words_lower) {
  std::string icao;
  std::size_t i = 0;
  const std::size_t n = words_lower.size();
  bool started = false;
  while (i < n) {
    while (i < n && !is_alpha(words_lower[i]))
      ++i; // skip separators (spaces, commas)
    const std::size_t start = i;
    while (i < n && (is_alpha(words_lower[i]) || words_lower[i] == '-'))
      ++i;
    if (start == i)
      break;
    const char letter =
        nato_word_to_letter(words_lower.substr(start, i - start));
    if (letter) {
      icao += letter;
      started = true;
    } else {
      if (started)
        break;    // run ended at first non-NATO word
      return {};  // leading word is not a NATO letter -> miss
    }
    if (icao.size() > 4)
      return {}; // too long to be an ICAO code
  }
  if (icao.size() < 3 || icao.size() > 4)
    return {};
  return icao;
}

} // namespace de_phraseology
