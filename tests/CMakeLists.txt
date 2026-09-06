# Catch2 unit tests for the SDK-free engine. Reuses the xp_atc_engine OBJECT
# library so we test the production code, not a re-implementation. Never links
# the X-Plane SDK — anything that needs XPLM belongs in the plugin target or
# in the testscripts/ scenarios.

set(CATCH2_DIR ${CMAKE_SOURCE_DIR}/vendor/catch2)

add_executable(xp_wellys_vfr_atc_tests
    $<TARGET_OBJECTS:xp_atc_engine>
    ${CMAKE_SOURCE_DIR}/tools/atc_repl/settings_stub.cpp
    ${CMAKE_SOURCE_DIR}/tools/atc_repl/xplane_context_stub.cpp
    ${CMAKE_SOURCE_DIR}/tools/atc_repl/traffic_fixture.cpp
    # keychain.cpp lives in the plugin module, but the wrapper itself
    # has no XPLM dependency — pull it in directly so the unit test
    # can roundtrip against a dedicated test service.
    ${CMAKE_SOURCE_DIR}/src/persistence/keychain.cpp
    # openai_common.cpp is XPLM/libcurl-free — pure WAV encode +
    # last4 helper. Used by the test_openai_common.cpp unit tests.
    ${CMAKE_SOURCE_DIR}/src/backends/openai_common.cpp
    # logging.cpp is the engine's logging sink; pull it in directly
    # so test_audit_logging.cpp can install a capturing sink. (It is
    # already inside xp_atc_engine OBJECT, so the link picks the
    # engine copy — no duplicate symbols.)
    ${CATCH2_DIR}/catch_amalgamated.cpp
    module_reset_listener.cpp
    test_smoke.cpp
    test_frequency_classification.cpp
    test_facility_classification.cpp
    test_info_radio_flow.cpp
    test_intent_parser.cpp
    test_intent_parser_de.cpp
    test_intent_rules_runway_vacated_veto.cpp
    test_post_landing_idle.cpp
    test_prompt_whitelist_drift.cpp
    test_atis_generator_de.cpp
    test_atis_generator_en.cpp
    test_de_phraseology.cpp
    test_en_phraseology.cpp
    test_bzf_compliance.cpp
    test_readback_strict.cpp
    test_initial_call_conformance.cpp
    test_initial_call_conformance_en.cpp
    test_was_airborne_reset.cpp
    test_readback_high_conf_gate.cpp
    test_runway_lock.cpp
    test_state_history.cpp
    test_state_revert_guard.cpp
    test_begin_fresh_flight.cpp
    test_phraseology_hints.cpp
    test_airport_vrps.cpp
    test_openair_db.cpp
    test_traffic_geometry.cpp
    test_traffic_context.cpp
    test_traffic_advisor.cpp
    test_traffic_dialog.cpp
    test_traffic_phase_classifier.cpp
    test_landing_sequence.cpp
    test_cross_country_log.cpp
    test_keychain.cpp
    test_openai_common.cpp
    test_audit_logging.cpp
    test_model_manifest.cpp
    test_pcm_resample.cpp
)

target_include_directories(xp_wellys_vfr_atc_tests PRIVATE
    ${CATCH2_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/tools/atc_repl
)

target_compile_definitions(xp_wellys_vfr_atc_tests PRIVATE
    XP_WELLYS_ATC_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    # Audit-logging source-level invariant test reads on-disk
    # backend TUs and asserts tag separation. Needs the repo root.
    XP_WELLYS_ATC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(xp_wellys_vfr_atc_tests PRIVATE CURL::libcurl)

if(APPLE)
    target_compile_options(xp_wellys_vfr_atc_tests PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    # Security.framework is needed by persistence/keychain.cpp.
    target_link_libraries(xp_wellys_vfr_atc_tests PRIVATE "-framework Security"
                                                      "-framework CoreFoundation")
    # Inherit OSX_ARCHITECTURES from the top-level project so the architectures
    # of the reused xp_atc_engine objects match the test executable.
elseif(WIN32)
    # persistence/keychain.cpp uses the Credential Manager (Advapi32) and
    # model_manifest.cpp the CNG SHA256 provider (Bcrypt) on Windows.
    target_link_libraries(xp_wellys_vfr_atc_tests PRIVATE Advapi32 Bcrypt)
endif()
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(xp_wellys_vfr_atc_tests PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
    target_link_options(xp_wellys_vfr_atc_tests PRIVATE ${XP_WELLYS_ATC_SANITIZER_FLAGS})
endif()

set_target_properties(xp_wellys_vfr_atc_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
