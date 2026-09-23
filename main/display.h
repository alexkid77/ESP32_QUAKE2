// Author: Alejandro Villegas Alonso
//         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
//
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#define QUAKE2_RES_X 320
#define QUAKE2_RES_Y 240

void display_init(void);
void display_quit(void);
void QG_DrawFrame(void *pixels);
void QG_SetPalette(unsigned char palette[768]);

#endif /* DISPLAY_H */