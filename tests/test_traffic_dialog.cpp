#include "atc/atc_templates.hpp"
#include "atc/intent_parser.hpp"
#include "atc/traffic_dialog.hpp"
#include "core/xplane_context.hpp"
#include "data/traffic_context.hpp"

#include <catch2/catch_amalgamated.hpp>

namespace {

// Minimal test ctx — traffic_dialog only consumes the fields
// atc_state_machine::build_vars touches (callsign, airport, freqs).
xplane_context::XPlaneContext minimal_ctx() {
  xplane_context::XPlaneContext ctx;
  ctx.nearest_airport_id = "LSZH";
  ctx.nearest_airport_name = "Zurich";
  ctx.frequency_type = xplane_context::FrequencyType::TOWER;
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  return ctx;
}

intent_parser::PilotMessage build_msg(intent_parser::PilotIntent intent,
                                      const std::string &transcript = {}) {
  intent_parser::PilotMessage m;
  m.intent = intent;
  m.confidence = 0.9f;
  m.callsign = "Hotel Bravo X-ray Yankee Zulu";
  m.raw_transcript = transcript;
  return m;
}

void prime() {
  atc_templates::reload();
  traffic_dialog::reset();
}

} // namespace

TEST_CASE("traffic_dialog: idle returns unhandled when not awaiting ack",
          "[traffic][dialog]") {
  prime();
  auto ctx = minimal_ctx();

  REQUIRE_FALSE(traffic_dialog::is_awaiting_ack());

  auto reply = traffic_dialog::handle_pilot(
      build_msg(intent_parser::PilotIntent::TRAFFIC_IN_SIGHT, "in sight"), ctx);
  REQUIRE_FALSE(reply.handled);
  REQUIRE(reply.text.empty());
}

TEST_CASE("traffic_dialog: TRAFFIC_IN_SIGHT acks and clears state",
          "[traffic][dialog]") {
  prime();
  auto ctx = minimal_ctx();

  traffic_dialog::on_advisory_emitted(424242);
  REQUIRE(traffic_dialog::is_awaiting_ack());
  REQUIRE(traffic_dialog::pending_target_id() == 424242);

  auto reply = traffic_dialog::handle_pilot(
      build_msg(intent_parser::PilotIntent::TRAFFIC_IN_SIGHT, "in sight"), ctx);
  REQUIRE(reply.handled);
  REQUIRE(reply.acknowledged_with_visual);
  REQUIRE(reply.text.find("halten Sie visuelle Staffelung") !=
          std::string::npos);
  REQUIRE_FALSE(traffic_dialog::is_awaiting_ack());
}

TEST_CASE("traffic_dialog: NEGATIVE_CONTACT re-issues with refreshed geometry",
          "[traffic][dialog]") {
  prime();
  auto ctx = minimal_ctx();

  // Push a deterministic snapshot the dialog can read clock/distance from.
  traffic_context::TrafficContext snap;
  traffic_context::TrafficTarget t;
  t.modeS_id = 100;
  t.clock_position = 2.0;
  t.distance_to_user_nm = 3.0;
  snap.targets.push_back(t);
  traffic_context::set_for_test(std::move(snap));

  traffic_dialog::on_advisory_emitted(100);
  auto reply = traffic_dialog::handle_pilot(
      build_msg(intent_parser::PilotIntent::TRAFFIC_NEGATIVE_CONTACT,
                "negative contact"),
      ctx);
  REQUIRE(reply.handled);
  REQUIRE_FALSE(reply.acknowledged_with_visual);
  REQUIRE(reply.text.find("Verkehr jetzt 2 Uhr") != std::string::npos);
  REQUIRE(reply.text.find("3 Meilen") != std::string::npos);
  REQUIRE_FALSE(traffic_dialog::is_awaiting_ack());

  // Reset shared snapshot so neighbouring tests don't see leakage.
  traffic_context::set_for_test(traffic_context::TrafficContext{});
}

TEST_CASE("traffic_dialog: non-traffic intent stays unhandled",
          "[traffic][dialog]") {
  prime();
  auto ctx = minimal_ctx();

  traffic_dialog::on_advisory_emitted(7);
  auto reply = traffic_dialog::handle_pilot(
      build_msg(intent_parser::PilotIntent::READBACK, "readback"), ctx);
  REQUIRE_FALSE(reply.handled);
  // Dialog state is unchanged — caller routes through main flow instead.
  REQUIRE(traffic_dialog::is_awaiting_ack());
  traffic_dialog::reset();
}
