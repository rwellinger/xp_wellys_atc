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

#ifndef XPLANE_CONTEXT_HPP
#define XPLANE_CONTEXT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace airspace_db {
struct Controller;
}

namespace xplane_context {

struct RunwayEnd {
  std::string number; // e.g. "09", "27L"
  double lat = 0.0;
  double lon = 0.0;
  float heading_deg = 0.0f; // computed from lat/lon of both ends
};

struct RunwayInfo {
  RunwayEnd end1;
  RunwayEnd end2;
  float width_m = 0.0f;
  float length_m = 0.0f; // computed via haversine
  int surface_code = 0;  // 1=asphalt, 2=concrete, etc.
};

enum class FrequencyType {
  UNKNOWN,
  DELIVERY,
  GROUND,
  TOWER,
  UNICOM,
  CTAF,
  ATIS,
  INFO,  // German AFIS/Info facility (apt.dat code 1054 named "... Information")
  RADIO, // Radio facility w/ Flugleiter (apt.dat code 1054 named "... Radio")
};

const char *frequency_type_name(FrequencyType ft);

// Operational class of an airport's radio service. Replaces the former bool
// is_towered_airport so AFIS/Info facilities (FrequencyType::INFO / RADIO) are a
// first-class category, distinct from genuinely uncontrolled (UNICOM/CTAF)
// fields. UNKNOWN is the safe default: when no airport is resolved / the
// frequency cache is not ready yet it yields empty hints rather than silently
// wrong ones. Order matters only for the explicit values.
enum class FacilityType {
  UNKNOWN = 0,      // not classified yet (no airport / cache not ready)
  UNCONTROLLED = 1, // UNICOM/CTAF self-announce field
  TOWERED = 2,      // controlled field with Tower (clearances, readback)
  AFIS = 3,         // Info/Radio facility: traffic info + reports, no clearances
};

const char *facility_type_name(FacilityType ft);

// Refine a frequency type using its apt.dat name. apt.dat encodes German
// AFIS/Info and Radio facilities under the Tower row code (1054); the only
// discriminator is the frequency name. Only TOWER is refined: a name ending in
// Info/Information -> INFO, Radio -> RADIO, otherwise (incl. Tower/empty/
// unknown) stays TOWER. All other base types pass through unchanged. SDK-free
// so it is unit-testable without parsing an apt.dat file.
FrequencyType classify_by_name(FrequencyType base, const std::string &name);

struct AirportFrequency {
  uint32_t freq_khz =
      0; // e.g. 121900 for 121.900 MHz (exact integer, no float)
  FrequencyType type = FrequencyType::UNKNOWN;
};

struct AirportFrequencies {
  std::vector<AirportFrequency> all;

  // Has at least one frequency of the given type?
  bool has(FrequencyType ft) const;
  // First frequency of given type as MHz (0.0f if none)
  float first_mhz(FrequencyType ft) const;
  // Match a COM frequency (MHz) to a FrequencyType (UNKNOWN if no match)
  FrequencyType lookup(float freq_mhz) const;
  // Convenience: has(GROUND)
  bool has_ground() const;
};

// Derive the operational FacilityType from an airport's frequency table.
// Single source of truth for the classification (TOWER -> TOWERED, INFO/RADIO ->
// AFIS, UNICOM/CTAF -> UNCONTROLLED, none -> UNKNOWN). SDK-free / unit-testable.
FacilityType classify_facility(const AirportFrequencies &freqs);

// True when the airport has at least one contactable ATC frequency
// (TOWER/INFO/RADIO/UNICOM/CTAF). ATIS-only or Delivery-only fields do NOT
// count. Used to filter out radio-less fields (glider strips) from active-ATC
// selection and the airport picker (issue #60). SDK-free / unit-testable.
bool has_contactable_atc(const AirportFrequencies &freqs);

struct XPlaneContext {
  double latitude = 0.0;
  double longitude = 0.0;
  float altitude_ft_msl = 0.0f;
  float groundspeed_kts = 0.0f;
  float indicated_airspeed_kts = 0.0f;
  float vertical_speed_fpm = 0.0f;
  float heading_true = 0.0f;
  float height_agl_ft = 0.0f;
  bool on_ground = true;
  float com1_freq_mhz = 0.0f;
  float com2_freq_mhz = 0.0f;
  float com1_standby_mhz = 0.0f;
  float com2_standby_mhz = 0.0f;
  int active_com = 1;
  // Transponder (SSR). `transponder_code` is the 4-digit squawk (BCD, e.g.
  // 7000 VFR conspicuity — the German VFR default); `transponder_mode`
  // follows the X-Plane actuator: 0=off, 1=standby, 2=on/alt. Read per frame
  // from DataRefs; only observed today (squawk readback is value-matched from
  // the issued clearance, not from this field). Assignment on controlled entry
  // (Class D / RMZ) is a follow-up once the airspace base is in place.
  int transponder_code = 7000;
  int transponder_mode = 0;
  std::string aircraft_icao;
  std::string nearest_airport_id;   // active airport (may be frequency-tuned)
  std::string geometric_nearest_id; // raw geographic nearest from XPLM
  std::string nearest_airport_name; // from apt.dat, e.g. "Grenchen"
  FacilityType facility_type = FacilityType::UNKNOWN;
  FrequencyType frequency_type = FrequencyType::UNKNOWN;
  bool avionics_on = false;
  bool com_radio_powered = false;
  float qnh_inhg = 29.92f;
  float wind_direction_deg = 0.0f;
  float wind_speed_kt = 0.0f;
  float visibility_m = 9999.0f;
  float cloud_base_ft_msl = 99999.0f;
  int cloud_type = 0; // 0=clear,1=few,2=scattered,3=broken,4=overcast
  float temperature_c = 15.0f;
  float dewpoint_c = 10.0f;
  float atis_freq_mhz = 0.0f;
  AirportFrequencies airport_freqs; // all frequencies for nearest airport
  bool tower_only = false;          // towered but no separate ground freq
  double airport_lat = 0.0;         // airport position (for range checks)
  double airport_lon = 0.0;
  std::vector<RunwayInfo> runways;
  std::string active_runway; // wind-determined, e.g. "28", "09L"
  // Controllers (TWR/TRACON/CTR from atc.dat) whose polygon + altitude band
  // enclose the current aircraft position. Refreshed once per second.
  // Empty if airspace_db is disabled (atc.dat missing).
  std::vector<const airspace_db::Controller *> enclosing_airspaces;
  // Monotonic clock at the time this context snapshot was taken. Plugin
  // populates from XPLMGetElapsedTime(); CLI / scenarios use a synthetic
  // step counter. Consumers that need a "how recent is X" check (e.g.
  // intent rules with a require_just_landed flag) read this instead of
  // taking the timestamp as a separate parameter.
  double now_secs = 0.0;

  // Derived binary view kept for the (unchanged) state machine / template / flow
  // logic that only cares about "controlled vs not". AFIS and UNCONTROLLED both
  // return false here, exactly as the former bool is_towered_airport did.
  bool is_towered() const noexcept {
    return facility_type == FacilityType::TOWERED;
  }
};

void init();
void stop();
void update();

const XPlaneContext &get();

// Write a frequency (in kHz, e.g. 121900) to the active COM's standby slot
void set_standby_freq(uint32_t freq_khz);

// ── Airport picker / lock ────────────────────────────────────────
// Force `nearest_airport_id` (and all derived fields) to a specific ICAO.
// Overrides both geometric-nearest and frequency-match logic.
// No-op if ICAO is not in the airport cache.
void lock_airport(const std::string &icao);
void unlock_airport();
const std::string &locked_airport() noexcept;

struct NearbyAirport {
  std::string icao;
  std::string name;
  double distance_nm = 0.0;
  bool has_atis = false;
  bool has_ground = false;
  bool has_tower = false;
  bool has_afis = false;   // INFO or RADIO -> AFIS facility
  bool has_unicom = false; // UNICOM or CTAF -> uncontrolled facility
};

// Return up to `max_count` airports within `max_nm` of the aircraft,
// sorted ascending by distance. Empty until the airport cache is ready.
std::vector<NearbyAirport> find_nearby_airports(double max_nm,
                                                size_t max_count);

// Field elevation in feet for the given ICAO. Returns 0.0f if the cache
// is not ready or the airport is unknown — callers that need to gate on
// "have we got real data" should check `airport_elevation_known()` too.
float airport_elevation_ft(const std::string &icao);
bool airport_elevation_known(const std::string &icao);

} // namespace xplane_context

#endif // XPLANE_CONTEXT_HPP
