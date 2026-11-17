/* Files sidebar with the mounted removable drives (aurora_places.c). */
#ifndef AURORA_PLACES_H
#define AURORA_PLACES_H
#include <stdbool.h>
#include <stddef.h>
#define PLACES_WIDTH 168
void places_draw(int x, int y, int h, const char* current);
/* x, y relative to the sidebar's top left. path: in = folder shown, out = folder to show.
 * Returns bit 1: show path; bit 2: message was set; 0: nothing hit. */
int places_click(int x, int y, char* path, size_t capacity, char* message, size_t message_capacity);
bool places_tick(void);   /* true when the mounted drives changed (checked at most every 500 ms) */
#endif
