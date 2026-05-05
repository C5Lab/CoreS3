#ifndef KEYBOARD_SCREEN_H
#define KEYBOARD_SCREEN_H

/**
 * Show a temporary text-input screen powered by the CardKB.
 * The user types freely; pressing Enter confirms and the
 * callback is invoked with the entered text, then the screen
 * transitions to the home screen.
 *
 * Call from app_main inside a bsp_display_lock / unlock pair.
 * The function returns immediately – input is handled by LVGL timer.
 *
 * @param on_done  Called (inside LVGL lock) once the user presses Enter.
 *                 May be NULL if you only care about the transition.
 */
typedef void (*keyboard_done_cb_t)(const char *text);
void show_keyboard_screen(keyboard_done_cb_t on_done);

#endif
