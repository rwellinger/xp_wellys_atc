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

#include "atc/en_phraseology.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace en_phraseology {

namespace {

// ICAO spoken digits (Annex 10 Vol II §5.2.1.4.3.1, docs/icao §11.1):
// the distinctive ATC forms "tree/fife/niner" for 3/5/9. Kept lowercase —
// the callsign expander and the mid-sentence ziffernweise both draw from
// this single table (mirrors de_phraseology's kDigitWords).
//
// 4 is "four", NOT the ICAO respelling "fower" (issue #63): "fower" is a
// human pronunciation hint, not a real word, so TTS engines say it
// literally ("FOW-er") instead of "four". "tree/fife/niner" survive
// because they are real words / clean homophones; "fower" was the outlier.
// The reverse parser (spoken_digit_value) still accepts both "fower" and
// "four", so STT tolerance is unchanged.
constexpr std::array<const char *, 10> kDigitWords = {
    "zero", "one",   "two",   "tree", "four",
    "fife", "six",   "seven", "eight", "niner"};

// ICAO/NATO phonetic alphabet, official spellings (Annex 10 Vol II
// Fig 5-1, docs/icao §11.3): "Alfa" (with f), "Juliett" (double-t),
// "X-ray". Identical set to de_phraseology's table.
constexpr std::array<const char *, 26> kNatoLetters = {
    "Alfa",   "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
    "Hotel",  "India",   "Juliett", "Kilo",   "Lima",   "Mike",    "November",
    "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
    "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu"};

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
inline bool is_alpha(char c) { return is_upper(c) || is_lower(c); }
inline bool is_word_char(char c) { return is_alpha(c) || is_digit(c); }
inline char lower(char c) {
  return is_upper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool boundary_before(const std::string &s, std::size_t i) {
  return i == 0 || !is_word_char(s[i - 1]);
}
inline bool boundary_after(const std::string &s, std::size_t i) {
  return i >= s.size() || !is_word_char(s[i]);
}

std::size_t scan_digits(const std::string &s, std::size_t pos) {
  std::size_t end = pos;
  while (end < s.size() && is_digit(s[end]))
    ++end;
  return end;
}

// Render raw digits digit-by-digit: "25" -> "two fife".
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

// Render integer N as thousand/hundred form when clean, else empty:
//   1000 -> "one thousand"
//   3500 -> "tree thousand fife hundred"
//   1100 -> "one thousand one hundred"
//   3050 -> "" (caller falls back to ziffernweise)
std::string thousand_form(int n) {
  if (n < 1000 || n > 9999)
    return {};
  int thousands = n / 1000;
  int remainder = n % 1000;
  std::string out = kDigitWords[thousands];
  out += " thousand";
  if (remainder == 0)
    return out;
  if (remainder % 100 != 0)
    return {};
  int hundreds = remainder / 100;
  out += ' ';
  out += kDigitWords[hundreds];
  out += " hundred";
  return out;
}

// Case-insensitive keyword match at position i requiring a word boundary
// before. Returns the position right after the keyword, or npos. English
// templates may present an anchor either capitalised ("Runway 25 …") or
// lower-case ("… cleared to land runway 25"); matching case-insensitively
// and re-emitting the ORIGINAL substring keeps whatever the template used.
std::size_t match_keyword_ci(const std::string &s, std::size_t i,
                             const char *kw) {
  std::size_t klen = 0;
  while (kw[klen])
    ++klen;
  if (i + klen > s.size())
    return std::string::npos;
  if (!boundary_before(s, i))
    return std::string::npos;
  for (std::size_t k = 0; k < klen; ++k) {
    if (lower(s[i + k]) != lower(kw[k]))
      return std::string::npos;
  }
  return i + klen;
}

// ────────────────────────────────────────────────────────────────────
// "Runway <NN>[LRC]" with optional English suffix
// "Runway 25"       -> "Runway two fife"
// "Runway 07L"      -> "Runway zero seven left"
// "runway 25 left"  -> "runway two fife left" (no double-translate)
// ────────────────────────────────────────────────────────────────────
std::string expand_runways(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t after_kw = match_keyword_ci(s, i, "Runway ");
    if (after_kw == std::string::npos) {
      out += s[i];
      ++i;
      continue;
    }
    std::size_t digit_end = scan_digits(s, after_kw);
    std::size_t n_digits = digit_end - after_kw;
    if (n_digits < 1 || n_digits > 2) {
      out.append(s, i, after_kw - i);
      i = after_kw;
      continue;
    }
    out.append(s, i, after_kw - i); // original "Runway "/"runway "
    out += ziffernweise(s.substr(after_kw, n_digits));
    std::size_t cursor = digit_end;
    if (cursor < s.size()) {
      char c = s[cursor];
      if ((c == 'L' || c == 'R' || c == 'C') && boundary_after(s, cursor + 1)) {
        if (c == 'L')
          out += " left";
        else if (c == 'R')
          out += " right";
        else
          out += " center";
        cursor += 1;
      }
    }
    i = cursor;
  }
  return out;
}

// Generic helper: expand a digit run following a (case-insensitive)
// keyword. `pad_to` left-pads with zeros before spelling (e.g. heading
// "50" -> "zero fife zero").
std::string expand_keyword_digits(const std::string &s, const char *keyword,
                                  int min_digits, int max_digits, int pad_to) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t after_kw = match_keyword_ci(s, i, keyword);
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
    out.append(s, i, after_kw - i); // original keyword text (preserve case)
    out += ziffernweise(digits);
    i = digit_end;
  }
  return out;
}

// Frequencies "NNN.NNN" -> "<digits> decimal <digits>". The decimal point
// alone is the signal (no keyword anchor).
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
          out += " decimal ";
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

// Wind "<NNN> degrees <N..NNN> knots" -> digit-by-digit both numbers;
// "degrees"/"knots" stay.
std::string expand_wind(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  const std::string kDeg = " degrees";
  const std::string kKts = " knots";
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t a_end = scan_digits(s, i);
      std::size_t a_len = a_end - i;
      if (a_len >= 1 && a_len <= 3 && a_end + kDeg.size() <= s.size() &&
          s.compare(a_end, kDeg.size(), kDeg) == 0 &&
          boundary_after(s, a_end + kDeg.size())) {
        std::size_t after_deg = a_end + kDeg.size();
        std::size_t b_start = after_deg;
        while (b_start < s.size() && s[b_start] == ' ')
          ++b_start;
        if (b_start < s.size() && is_digit(s[b_start])) {
          std::size_t b_end = scan_digits(s, b_start);
          std::size_t b_len = b_end - b_start;
          if (b_len >= 1 && b_len <= 3 && b_end + kKts.size() <= s.size() &&
              s.compare(b_end, kKts.size(), kKts) == 0 &&
              boundary_after(s, b_end + kKts.size())) {
            out += ziffernweise(s.substr(i, a_len));
            out += " degrees ";
            out += ziffernweise(s.substr(b_start, b_len));
            out += " knots";
            i = b_end + kKts.size();
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

// Altitudes "<digits> feet" (or "meters"): thousand/hundred form when
// clean, otherwise digit-by-digit.
std::string expand_altitudes(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  std::size_t i = 0;
  const std::string kFeet = " feet";
  const std::string kMeters = " meters";
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t d_end = scan_digits(s, i);
      std::size_t d_len = d_end - i;
      const char *unit = nullptr;
      std::size_t unit_span = 0;
      if (d_end + kFeet.size() <= s.size() &&
          s.compare(d_end, kFeet.size(), kFeet) == 0 &&
          boundary_after(s, d_end + kFeet.size())) {
        unit = "feet";
        unit_span = kFeet.size();
      } else if (d_end + kMeters.size() <= s.size() &&
                 s.compare(d_end, kMeters.size(), kMeters) == 0 &&
                 boundary_after(s, d_end + kMeters.size())) {
        unit = "meters";
        unit_span = kMeters.size();
      }
      if (unit && d_len >= 1 && d_len <= 5) {
        std::string digits = s.substr(i, d_len);
        int n = 0;
        for (char c : digits)
          n = n * 10 + (c - '0');
        std::string tf = thousand_form(n);
        out += tf.empty() ? ziffernweise(digits) : tf;
        out += ' ';
        out += unit;
        i = d_end + unit_span;
        continue;
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// Clock position "<N|NN> o'clock" (1..12).
std::string expand_clock(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  std::size_t i = 0;
  const std::string kClock = " o'clock";
  while (i < s.size()) {
    if (is_digit(s[i]) && boundary_before(s, i)) {
      std::size_t d_end = scan_digits(s, i);
      std::size_t d_len = d_end - i;
      if (d_len >= 1 && d_len <= 2 && d_end + kClock.size() <= s.size() &&
          s.compare(d_end, kClock.size(), kClock) == 0 &&
          boundary_after(s, d_end + kClock.size())) {
        std::string digits = s.substr(i, d_len);
        int n = 0;
        for (char c : digits)
          n = n * 10 + (c - '0');
        if (n >= 1 && n <= 12) {
          out += ziffernweise(digits);
          out += " o'clock";
          i = d_end + kClock.size();
          continue;
        }
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// Sequence number "number <digit>".
std::string expand_sequence(const std::string &s) {
  return expand_keyword_digits(s, "number ", 1, 2, 0);
}

// ────────────────────────────────────────────────────────────────────
// Reverse normalizer helpers
// ────────────────────────────────────────────────────────────────────

// Spoken digit word -> 0..9, or -1. Accepts the ICAO forms
// (tree/fower/fife/niner) AND the plain English words a transcriber may
// emit (three/four/five/nine), plus "oh" for zero. Case-insensitive.
int spoken_digit_value(const std::string &lc_word) {
  static const std::array<const char *, 10> icao = {
      "zero", "one",   "two",   "tree", "fower",
      "fife", "six",   "seven", "eight", "niner"};
  for (int i = 0; i < 10; ++i)
    if (lc_word == icao[static_cast<std::size_t>(i)])
      return i;
  if (lc_word == "oh")
    return 0;
  if (lc_word == "three")
    return 3;
  if (lc_word == "four")
    return 4;
  if (lc_word == "five")
    return 5;
  if (lc_word == "nine")
    return 9;
  return -1;
}

std::string to_lower_word(const std::string &w) {
  std::string out;
  out.reserve(w.size());
  for (char c : w)
    out += lower(c);
  return out;
}

struct Token {
  std::string raw;
  std::string lc_word;
  std::string trailing;
  std::size_t start;
  std::size_t end;
};

std::vector<Token> tokenize(const std::string &s) {
  std::vector<Token> out;
  std::size_t i = 0;
  while (i < s.size()) {
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
    std::size_t body_end = t.raw.size();
    while (body_end > 0) {
      char c = t.raw[body_end - 1];
      if (is_word_char(c) || c == '\'')
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

bool token_body_has_digit(const Token &t) {
  for (char c : t.lc_word)
    if (is_digit(c))
      return true;
  return false;
}

struct DigitRun {
  std::string digits;
  std::size_t length;
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

void emit_digits(std::string &out, const std::string &digits,
                 const std::string &trailing) {
  if (!out.empty() && out.back() != ' ')
    out += ' ';
  out += digits;
  out += trailing;
}

void emit_frequency(std::string &out, const std::string &left,
                    const std::string &right, const std::string &trailing) {
  if (!out.empty() && out.back() != ' ')
    out += ' ';
  out += left;
  out += '.';
  out += right;
  out += trailing;
}

// A leading anchor keyword permits a single-digit run ("Runway 7");
// free-floating runs stay min=2 to avoid grabbing a lone "one" numerus.
bool prev_token_is_run_anchor(const std::vector<Token> &tok, std::size_t i) {
  if (i == 0)
    return false;
  const std::string &prev = tok[i - 1].lc_word;
  static const std::array<const char *, 12> anchors = {
      "runway",  "qnh",     "heading", "frequency", "wind",   "number",
      "contact", "climb",   "descend", "squawk",    "traffic", "information"};
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
    if (t.lc_word[k] != lower(kw[k]))
      return false;
    ++k;
  }
  return k == t.lc_word.size();
}

// Is this token the frequency decimal separator word? Accepts both the
// ICAO "decimal" and the colloquial "point".
bool tok_is_decimal(const Token &t) {
  return tok_eq(t, "decimal") || tok_eq(t, "point");
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
    if (token_body_has_digit(tok[i])) {
      if (!out.empty() && out.back() != ' ')
        out += ' ';
      out += tok[i].raw;
      ++i;
      continue;
    }

    // Frequency: <2..3 digits> "decimal"/"point" <1..3 digits>.
    DigitRun freq_probe = collect_digit_run(tok, i, 2, 3);
    if (freq_probe.length > 0) {
      std::size_t after_left = i + freq_probe.length;
      bool prev_had_trailing = !tok[after_left - 1].trailing.empty();
      if (!prev_had_trailing && after_left < tok.size() &&
          tok_is_decimal(tok[after_left]) &&
          tok[after_left].trailing.empty()) {
        DigitRun right = collect_digit_run(tok, after_left + 1, 1, 3);
        if (right.length > 0) {
          std::size_t end = after_left + 1 + right.length;
          emit_frequency(out, freq_probe.digits, right.digits,
                         tok[end - 1].trailing);
          i = end;
          continue;
        }
      }
    }

    std::size_t min_digits = prev_token_is_run_anchor(tok, i) ? 1 : 2;
    DigitRun run = collect_digit_run(tok, i, min_digits, 4);
    if (run.length > 0) {
      std::size_t after = i + run.length;
      emit_digits(out, run.digits, tok[after - 1].trailing);
      i = after;
      continue;
    }

    if (!out.empty() && out.back() != ' ')
      out += ' ';
    out += tok[i].raw;
    ++i;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────
// De-shout pass (issue #62): lowercase ALL-CAPS phraseology words before
// they reach the synthesizer. ICAO/BZF writes phraseology in uppercase
// ("SAY AGAIN", "NEGATIVE", "READBACK"); a language-agnostic moderation
// "shouting/aggression" heuristic (Mistral's TTS guardrail) reads
// sustained uppercase as hostile and 403-blocks the request. Lowercasing
// the SPOKEN form is pronunciation-neutral and defuses the trigger — the
// transcript / Log.txt still show the original uppercase, because this
// runs only in the TTS pipeline (via normalize_for_speech). Mirrors
// de_phraseology::soften_caps_for_speech (English carries no umlauts, so
// this ASCII variant suffices). Acronyms in kKeepUpper stay uppercase so
// the TTS still articulates them. A word qualifies as "shouting" only
// when it has >= 2 letters, at least one uppercase letter, and NO
// lowercase letter. Idempotent.
// ────────────────────────────────────────────────────────────────────
std::string soften_caps_for_speech(const std::string &s) {
  static const std::array<const char *, 11> kKeepUpper = {
      {"QNH", "VFR", "IFR", "ATIS", "ILS", "UTC", "RMZ", "TMZ", "CTR", "TMA",
       "CTA"}};

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
    const std::size_t start = i;
    while (i < n && !is_space(s[i]))
      ++i;
    const std::string tok = s.substr(start, i - start);

    bool has_upper = false, has_lower = false;
    int letters = 0;
    for (char ch : tok) {
      if (is_upper(ch)) {
        has_upper = true;
        ++letters;
      } else if (is_lower(ch)) {
        has_lower = true;
        ++letters;
      }
    }

    const bool all_caps = has_upper && !has_lower && letters >= 2;
    bool keep = false;
    if (all_caps) {
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
    for (char ch : tok)
      out += is_upper(ch) ? static_cast<char>(ch + ('a' - 'A')) : ch;
  }
  return out;
}

} // namespace

std::string normalize_for_speech(const std::string &text) {
  if (text.empty())
    return text;
  std::string s = text;
  // Order mirrors de_phraseology: frequencies before generic digit
  // patterns; wind before altitude since both feature multi-digit numbers
  // but only wind has the "degrees ... knots" anchor pair. There is no
  // German umlaut-restoration pass and no "Information Alpha->Alfa" swap
  // (English templates already carry the ICAO spelling).
  s = expand_runways(s);
  s = expand_keyword_digits(s, "QNH ", 3, 4, 0);
  s = expand_keyword_digits(s, "heading ", 1, 3, 3);
  s = expand_frequencies(s);
  s = expand_wind(s);
  s = expand_altitudes(s);
  s = expand_clock(s);
  s = expand_sequence(s);
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

// Reverse a single spoken NATO letter word -> uppercase letter, or '\0'.
// Tolerant of dash stripping (x-ray) and common variants (alpha, juliet).
static char nato_word_to_letter(const std::string &w) {
  std::string key;
  for (char c : w)
    if (c != '-')
      key += lower(c);
  for (std::size_t i = 0; i < kNatoLetters.size(); ++i) {
    std::string lw;
    for (const char *p = kNatoLetters[i]; *p; ++p) {
      if (*p == '-')
        continue;
      lw += lower(*p);
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
      ++i;
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
        break;
      return {};
    }
    if (icao.size() > 4)
      return {};
  }
  if (icao.size() < 3 || icao.size() > 4)
    return {};
  return icao;
}

} // namespace en_phraseology
