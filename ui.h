#ifndef MEGAMENU_UI_H
#define MEGAMENU_UI_H

#define PAIR_MENU 1
#define PAIR_SELECTED 2
#define PAIR_OUTPUT 3
#define PAIR_BORDER 4

void init_ui(void);
void apply_theme(int theme_index);
int ui_theme_count(void);
const char *ui_theme_name(int theme_index);
void show_system_info_popup(void);

#endif
