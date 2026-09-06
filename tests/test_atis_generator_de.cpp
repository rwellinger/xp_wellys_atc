// M8 DoD: ATIS broadcast generates correct German phrasing when
// flow_region() == "DE" and the M3 normalizer expands ziffernweise
// numbers, runway, QNH unit and altitude tausend/hundert form.

#include "atc/atis_generator.hpp"
#include "atc/de_phraseology.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using xplane_context::XPlaneContext;

namespace {

// Forces flow_region to "DE" for the test body and restores the
// previous value on destruction. ATIS does not depend on intent_rules,
// so the lighter guard (no reload) is sufficient.
struct DeRegionGuard {
    std::string saved_region;
    DeRegionGuard() : saved_region(settings::atc_profile()) {
        settings::set_atc_profile("DE");
        atis_generator::init(); // reset letter_ to 'A' for deterministic output
    }
    ~DeRegionGuard() { settings::set_atc_profile(saved_region); }
};

XPlaneContext edny_ctx() {
    XPlaneContext ctx;
    ctx.nearest_airport_id = "EDNY";
    ctx.nearest_airport_name = "Friedrichshafen";
    ctx.active_runway = "25";
    ctx.wind_direction_deg = 240.0f;
    ctx.wind_speed_kt = 8.0f;
    ctx.visibility_m = 10000.0f; // CAVOK-aequivalent -> "ueber 10 Kilometer"
    ctx.cloud_type = 1; // few
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

TEST_CASE("DE ATIS: visibility >= 10 km uses 'ueber 10 Kilometer'",
          "[atis][de][visibility]") {
    DeRegionGuard g;
    auto ctx = edny_ctx();
    ctx.visibility_m = 10000.0f;
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "Sicht ueber 10 Kilometer"));
}

TEST_CASE("DE ATIS: visibility 8000 m -> '8 Kilometer'",
          "[atis][de][visibility]") {
    DeRegionGuard g;
    auto ctx = edny_ctx();
    ctx.visibility_m = 8000.0f;
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "Sicht 8 Kilometer"));
}

TEST_CASE("DE ATIS: visibility 800 m -> '800 Meter'",
          "[atis][de][visibility]") {
    DeRegionGuard g;
    auto ctx = edny_ctx();
    ctx.visibility_m = 800.0f;
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "Sicht 800 Meter"));
}

// ── Wind ─────────────────────────────────────────────────────────────

TEST_CASE("DE ATIS: calm wind -> 'Wind ruhig'", "[atis][de][wind]") {
    DeRegionGuard g;
    auto ctx = edny_ctx();
    ctx.wind_speed_kt = 1.0f;
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "Wind ruhig"));
    REQUIRE_FALSE(contains(text, "Wind still"));
}

TEST_CASE("DE ATIS: wind 240/8 -> 'Wind 240 Grad 8 Knoten'",
          "[atis][de][wind]") {
    DeRegionGuard g;
    auto text = atis_generator::generate_atis_text(edny_ctx());
    REQUIRE(contains(text, "Wind 240 Grad 8 Knoten"));
}

// ── Clouds ───────────────────────────────────────────────────────────

TEST_CASE("DE ATIS: clear sky -> 'Wolkenlos.'", "[atis][de][clouds]") {
    DeRegionGuard g;
    auto ctx = edny_ctx();
    ctx.cloud_type = 0;
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "Wolkenlos."));
}

TEST_CASE("DE ATIS: few clouds at 3500 ft -> 'wenige Wolken in 3500 Fuss.'",
          "[atis][de][clouds]") {
    DeRegionGuard g;
    auto text = atis_generator::generate_atis_text(edny_ctx());
    REQUIRE(contains(text, "wenige Wolken in 3500 Fuss."));
}

TEST_CASE("DE ATIS: overcast -> 'bedeckt in <ft> Fuss.'",
          "[atis][de][clouds]") {
    DeRegionGuard g;
    auto ctx = edny_ctx();
    ctx.cloud_type = 4;
    ctx.cloud_base_ft_msl = 1500.0f;
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "bedeckt in 1500 Fuss."));
}

// ── Full broadcast assembly ──────────────────────────────────────────

TEST_CASE("DE ATIS: EDNY broadcast contains all canonical phrases",
          "[atis][de][full]") {
    DeRegionGuard g;
    auto text = atis_generator::generate_atis_text(edny_ctx());

    REQUIRE(contains(text, "Friedrichshafen Information Alfa aktuell."));
    REQUIRE(contains(text, "Piste 25 in Betrieb."));
    REQUIRE(contains(text, "Wind 240 Grad 8 Knoten."));
    REQUIRE(contains(text, "Sicht ueber 10 Kilometer."));
    REQUIRE(contains(text, "wenige Wolken in 3500 Fuss."));
    REQUIRE(contains(text, "Temperatur 18, Taupunkt 12."));
    REQUIRE(contains(text, "QNH 1013."));
    REQUIRE(contains(text, "Bei Erstanruf Information Alfa angeben."));
}

TEST_CASE("DE ATIS: unknown airport falls back to 'Flugplatz'",
          "[atis][de][full]") {
    DeRegionGuard g;
    XPlaneContext ctx; // all defaults — no airport name/id
    ctx.active_runway = "07";
    auto text = atis_generator::generate_atis_text(ctx);
    REQUIRE(contains(text, "Flugplatz Information Alfa aktuell."));
}

// ── Normalizer roundtrip (M3 ↔ M8) ───────────────────────────────────
//
// atc_session::speak_response() runs de_phraseology::normalize_for_speech
// over the ATIS text for DE before handing it to TTS. Asserting the
// normalized output keeps the contract between M3 and M8 explicit: the
// raw broadcast emits digits + anchor keywords, the normalizer expands
// them ziffernweise / appends "Hektopascal" / converts altitudes to
// tausend/hundert form.

TEST_CASE("DE ATIS: normalized broadcast carries BZF phrasing",
          "[atis][de][normalizer]") {
    DeRegionGuard g;
    auto raw = atis_generator::generate_atis_text(edny_ctx());
    auto spoken = de_phraseology::normalize_for_speech(raw);

    // Piste 25 -> Piste zwo fünf (UTF-8 umlaut from restore_umlaute pass)
    REQUIRE(contains(spoken, "Piste zwo fünf in Betrieb"));
    // QNH 1013 -> QNH eins null eins drei Hektopascal (unit auto-appended)
    REQUIRE(contains(spoken, "QNH eins null eins drei Hektopascal"));
    // 3500 Fuss -> drei tausend fünfhundert Fuß
    REQUIRE(contains(spoken, "drei tausend fünfhundert Fuß"));
    // Wind 240 Grad 8 Knoten -> ziffernweise both sides
    REQUIRE(contains(spoken,
                     "Wind zwo vier null Grad acht Knoten"));
    // Information Alfa is idempotent through Pass 10 (Alpha->Alfa
    // swap does not touch Alfa).
    REQUIRE(contains(spoken, "Information Alfa aktuell"));
    REQUIRE(contains(spoken, "Bei Erstanruf Information Alfa angeben"));
}
