idf_component_register(SRCS "main.c" "display.c" "audio.c" "input.c" "usb_hid.c" "font_8x16.c" "eth_connect.c"
					INCLUDE_DIRS ".")

# Quake 2 engine references QG_DrawFrame, QG_SetPalette, QG_GetKey, QG_GetMouseMove
# and SNDDMA_* from the main component; force the linker to pull those objects in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u QG_DrawFrame -u QG_SetPalette -u QG_GetKey -u QG_GetMouseMove -u snd_inited")
