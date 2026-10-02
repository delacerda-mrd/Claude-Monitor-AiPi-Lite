idf_component_register(
    SRCS "main.c" "ui.c" "clawd.c" "usage.c" "net.c" "web.c"
         "power.c" "led.c" "settings.c" "audio.c"
         "fonts/font_jbm_bold_30.c" "fonts/font_jbm_bold_16.c"
         "fonts/font_jbm_medium_11.c" "fonts/font_styrene_12.c"
         "fonts/font_styrene_14.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html")
