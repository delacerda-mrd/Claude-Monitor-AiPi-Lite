# Board-specific code lives here; the shared meter (audio, voice, "Jarvis",
# brain, usage polling, settings, network, web dashboard) comes from
# meter_core -- a sibling checkout at ../meter_core, or $METER_CORE.
set(METER_CORE "$ENV{METER_CORE}")
if(NOT METER_CORE)
    set(METER_CORE "${CMAKE_CURRENT_LIST_DIR}/../../meter_core")
endif()
include(${METER_CORE}/firmware/meter_core.cmake)

idf_component_register(
    SRCS "main.c" "ui.c" "clawd.c" "power.c" "led.c"
         "fonts/font_jbm_bold_30.c" "fonts/font_jbm_bold_16.c"
         "fonts/font_jbm_medium_11.c" "fonts/font_styrene_12.c"
         "fonts/font_styrene_14.c"
         ${METER_CORE_SRCS}
    INCLUDE_DIRS "." ${METER_CORE_INCLUDE}
    EMBED_TXTFILES ${METER_CORE_EMBED})
