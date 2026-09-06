// EN #7 (#42): ATIS broadcast generates ICAO-VFR English phrasing when
// atc_profile() == "EN", and en_phraseology::normalize_for_speech expands
// ziffernweise numbers, runway, QNH digits and altitude thousand/hundred
// form on the TTS path. Mirrors test_atis_generator_de.cpp.

#include "atc/atis_generator.hpp"
#include "atc/en_phraseology.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using xplane_context::XPlaneContext;

namespace {

// Forces atc_profile to "EN" for the test body and restores the previous
// value on destruction. ATIS does not depend on the profile bundle
// (generate_atis_text_en uses hardcoded ICAO phrasing), so no template load
// is needed.
struct EnProfileGuard {
  std::string saved_profile;
  EnProfileGuard() : saved_profile(settings::atc_profile()) {
    settings::set_atc_profile("EN");
    atis_generator::init(); // reset letter_ to 'A' for deterministic output
  }
  ~EnProfileGuard() { settings::set_atc_profile(saved_profile); }
};

XPlaneContext edny_ctx() {
  XPlaneContext ctx;
  ctx.nearest_airport_id = "EDNY";
  ctx.nearest_airport_name = "Friedrichshafen";
  ctx.active_runway = "25";
  ctx.wind_direction_deg = 240.0f;
  ctx.wind_speed_kt = 8.0f;
  ctx.visibility_m = 10000.0f; // -> "more than 10 kilometers"
  ctx.cloud_type = 1;          // few
  ctx.cloud_base_ft_msl = 3500.0f;
  ctx.temperature_c = 18.0f;
  ctx.dewpoint_c = 12.0f;
  ctx.qnh_inhg = 29.92f; // -> 1013 hPa
  return ctx;
}

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

} // namespace

// ── Visibility ───────────────────────────────────────────────────────

TEST_CASE("EN ATIS: visibility >= 10 km uses 'more than 10 kilometers'",
          "[atis][en][visibility]") {
  EnProfileGuard g;
  auto ctx = edny_ctx();
  ctx.visibility_m = 10000.0f;
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "Visibility more than 10 kilometers"));
}

TEST_CASE("EN ATIS: visibility 8000 m -> '8 kilometers'",
          "[atis][en][visibility]") {
  EnProfileGuard g;
  auto ctx = edny_ctx();
  ctx.visibility_m = 8000.0f;
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "Visibility 8 kilometers"));
}

TEST_CASE("EN ATIS: visibility 800 m -> '800 meters'",
          "[atis][en][visibility]") {
  EnProfileGuard g;
  auto ctx = edny_ctx();
  ctx.visibility_m = 800.0f;
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "Visibility 800 meters"));
}

// ── Wind ─────────────────────────────────────────────────────────────

TEST_CASE("EN ATIS: calm wind -> 'Wind calm'", "[atis][en][wind]") {
  EnProfileGuard g;
  auto ctx = edny_ctx();
  ctx.wind_speed_kt = 1.0f;
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "Wind calm"));
}

TEST_CASE("EN ATIS: wind 240/8 -> 'Wind 240 degrees 08 knots'",
          "[atis][en][wind]") {
  EnProfileGuard g;
  auto text = atis_generator::generate_atis_text(edny_ctx());
  REQUIRE(contains(text, "Wind 240 degrees 08 knots"));
}

// ── Clouds ───────────────────────────────────────────────────────────

TEST_CASE("EN ATIS: clear sky -> 'Sky clear.'", "[atis][en][clouds]") {
  EnProfileGuard g;
  auto ctx = edny_ctx();
  ctx.cloud_type = 0;
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "Sky clear."));
}

TEST_CASE("EN ATIS: few clouds at 3500 ft -> 'few clouds at 3500 feet.'",
          "[atis][en][clouds]") {
  EnProfileGuard g;
  auto text = atis_generator::generate_atis_text(edny_ctx());
  REQUIRE(contains(text, "few clouds at 3500 feet."));
}

TEST_CASE("EN ATIS: overcast -> 'overcast at <ft> feet.'",
          "[atis][en][clouds]") {
  EnProfileGuard g;
  auto ctx = edny_ctx();
  ctx.cloud_type = 4;
  ctx.cloud_base_ft_msl = 1500.0f;
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "overcast at 1500 feet."));
}

// ── Full broadcast assembly ──────────────────────────────────────────

TEST_CASE("EN ATIS: EDNY broadcast contains all canonical phrases",
          "[atis][en][full]") {
  EnProfileGuard g;
  auto text = atis_generator::generate_atis_text(edny_ctx());

  REQUIRE(contains(text, "Friedrichshafen Information Alpha."));
  REQUIRE(contains(text, "Runway 25 in use."));
  REQUIRE(contains(text, "Wind 240 degrees 08 knots."));
  REQUIRE(contains(text, "Visibility more than 10 kilometers."));
  REQUIRE(contains(text, "few clouds at 3500 feet."));
  REQUIRE(contains(text, "Temperature 18, dew point 12."));
  REQUIRE(contains(text, "QNH 1013."));
  REQUIRE(
      contains(text, "On initial contact advise you have information Alpha."));
}

TEST_CASE("EN ATIS: unknown airport falls back to 'Airport'",
          "[atis][en][full]") {
  EnProfileGuard g;
  XPlaneContext ctx; // all defaults — no airport name/id
  ctx.active_runway = "07";
  auto text = atis_generator::generate_atis_text(ctx);
  REQUIRE(contains(text, "Airport Information Alpha."));
}

// ── Normalizer roundtrip (en_phraseology) ────────────────────────────
//
// atc_session::speak_response() runs en_phraseology::normalize_for_speech
// over the ATIS text for EN before handing it to TTS. Asserting the
// normalized output keeps the contract explicit: the raw broadcast emits
// digits + anchor keywords, the normalizer spells them ziffernweise (ICAO
// tree/four/fife/niner) and converts altitudes to thousand/hundred form.

TEST_CASE("EN ATIS: normalized broadcast carries ICAO ziffernweise phrasing",
          "[atis][en][normalizer]") {
  EnProfileGuard g;
  auto raw = atis_generator::generate_atis_text(edny_ctx());
  auto spoken = en_phraseology::normalize_for_speech(raw);

  // Runway 25 -> Runway two fife
  REQUIRE(contains(spoken, "Runway two fife in use"));
  // QNH 1013 -> QNH one zero one tree
  REQUIRE(contains(spoken, "QNH one zero one tree"));
  // 3500 feet -> tree thousand fife hundred feet
  REQUIRE(contains(spoken, "tree thousand fife hundred feet"));
  // Wind 240 degrees 08 knots -> ziffernweise both sides (4 -> "four", #63)
  REQUIRE(contains(spoken, "two four zero degrees zero eight knots"));
  // Information Alpha is untouched (no Alpha->Alfa swap on the EN path)
  REQUIRE(contains(spoken, "Information Alpha."));
  REQUIRE(contains(spoken, "information Alpha."));
}
