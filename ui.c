#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#define LINE_BUFFER_SIZE 2048

typedef struct {
    const char *name;
    short menu_fg;
    short menu_bg;
    short selected_fg;
    short selected_bg;
    short output_fg;
    short output_bg;
    short border_fg;
    short border_bg;
} Theme;

static const Theme THEMES[] = {
    {"Classic", COLOR_WHITE, COLOR_BLACK, COLOR_BLACK, COLOR_CYAN, COLOR_WHITE,
     COLOR_BLACK, COLOR_CYAN, COLOR_BLACK},
    {"Dracula-ish", COLOR_WHITE, COLOR_BLUE, COLOR_YELLOW, COLOR_MAGENTA, COLOR_WHITE,
     COLOR_BLUE, COLOR_MAGENTA, COLOR_BLUE},
    {"Solarized-ish", COLOR_YELLOW, COLOR_BLUE, COLOR_BLUE, COLOR_YELLOW, COLOR_CYAN,
     COLOR_BLUE, COLOR_YELLOW, COLOR_BLUE},
    {"Gruvbox-ish", COLOR_YELLOW, COLOR_BLACK, COLOR_BLACK, COLOR_YELLOW, COLOR_WHITE,
     COLOR_BLACK, COLOR_GREEN, COLOR_BLACK},
    {"Nord-ish", COLOR_CYAN, COLOR_BLACK, COLOR_BLACK, COLOR_CYAN, COLOR_WHITE,
     COLOR_BLACK, COLOR_BLUE, COLOR_BLACK},
};

static const int THEME_COUNT = (int)(sizeof(THEMES) / sizeof(THEMES[0]));

/* Format bytes into a compact human-readable unit string. */
static void format_bytes(unsigned long long bytes, char *out, size_t out_size) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < (int)(sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        unit++;
    }
    snprintf(out, out_size, "%.1f %s", value, units[unit]);
}

/* Read a short CPU model string from /proc/cpuinfo when available. */
static void read_cpu_model(char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "Unavailable");

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        return;
    }

    char line[LINE_BUFFER_SIZE];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0) {
            char *sep = strchr(line, ':');
            if (!sep) {
                break;
            }
            sep++;
            while (*sep == ' ' || *sep == '\t') {
                sep++;
            }
            sep[strcspn(sep, "\r\n")] = '\0';
            snprintf(buffer, buffer_size, "%s", sep);
            break;
        }
    }
    fclose(f);
}

/* Initialize ncurses runtime and terminal input behavior. */
void init_ui(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(100);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    refresh();
}

/* Configure ncurses color pairs for the selected theme index. */
void apply_theme(int theme_index) {
    if (!has_colors()) {
        return;
    }
    if (theme_index < 0 || theme_index >= THEME_COUNT) {
        theme_index = 0;
    }

    const Theme *theme = &THEMES[theme_index];
    init_pair(PAIR_MENU, theme->menu_fg, theme->menu_bg);
    init_pair(PAIR_SELECTED, theme->selected_fg, theme->selected_bg);
    init_pair(PAIR_OUTPUT, theme->output_fg, theme->output_bg);
    init_pair(PAIR_BORDER, theme->border_fg, theme->border_bg);
}

int ui_theme_count(void) { return THEME_COUNT; }

const char *ui_theme_name(int theme_index) {
    if (theme_index < 0 || theme_index >= THEME_COUNT) {
        return THEMES[0].name;
    }
    return THEMES[theme_index].name;
}

/* Show a system information popup and close it on ENTER. */
void show_system_info_popup(void) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);

    if (rows < 14 || cols < 60) {
        erase();
        mvprintw(0, 0, "Terminal too small for system info (need at least 60x14).");
        mvprintw(1, 0, "Press ENTER to return.");
        refresh();
        int ch;
        while ((ch = getch()) != '\n' && ch != KEY_ENTER && ch != '\r') {
        }
        return;
    }

    int popup_height = 14;
    int popup_width = cols - 8;
    if (popup_width > 110) {
        popup_width = 110;
    }
    if (popup_width < 60) {
        popup_width = 60;
    }

    int start_y = (rows - popup_height) / 2;
    int start_x = (cols - popup_width) / 2;
    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) {
        return;
    }

    keypad(popup, TRUE);
    if (has_colors()) {
        wbkgd(popup, COLOR_PAIR(PAIR_OUTPUT));
        wattron(popup, COLOR_PAIR(PAIR_BORDER));
    }
    box(popup, 0, 0);
    mvwprintw(popup, 0, 2, " System Info ");
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_BORDER));
        wattron(popup, COLOR_PAIR(PAIR_OUTPUT));
    }

    struct sysinfo info;
    struct utsname uname_info;
    int have_sysinfo = (sysinfo(&info) == 0);
    int have_uname = (uname(&uname_info) == 0);

    char hostname[128] = "Unavailable";
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0';
    }

    char cpu_model[256];
    read_cpu_model(cpu_model, sizeof(cpu_model));

    char total_mem[64] = "Unavailable";
    char free_mem[64] = "Unavailable";
    char used_mem[64] = "Unavailable";
    char load1[32] = "Unavailable";
    char load5[32] = "Unavailable";
    char load15[32] = "Unavailable";
    char uptime[64] = "Unavailable";
    if (have_sysinfo) {
        unsigned long long unit = (unsigned long long)info.mem_unit;
        unsigned long long total_bytes = (unsigned long long)info.totalram * unit;
        unsigned long long free_bytes = (unsigned long long)info.freeram * unit;
        unsigned long long used_bytes = total_bytes > free_bytes ? (total_bytes - free_bytes) : 0;
        format_bytes(total_bytes, total_mem, sizeof(total_mem));
        format_bytes(free_bytes, free_mem, sizeof(free_mem));
        format_bytes(used_bytes, used_mem, sizeof(used_mem));

        double l1 = info.loads[0] / 65536.0;
        double l5 = info.loads[1] / 65536.0;
        double l15 = info.loads[2] / 65536.0;
        snprintf(load1, sizeof(load1), "%.2f", l1);
        snprintf(load5, sizeof(load5), "%.2f", l5);
        snprintf(load15, sizeof(load15), "%.2f", l15);

        long up = info.uptime;
        long days = up / 86400;
        long hours = (up % 86400) / 3600;
        long mins = (up % 3600) / 60;
        long secs = up % 60;
        snprintf(uptime, sizeof(uptime), "%ldd %02ldh %02ldm %02lds", days, hours, mins,
                 secs);
    }

    mvwprintw(popup, 2, 2, "Host       : %.*s", popup_width - 16, hostname);
    mvwprintw(popup, 3, 2, "Kernel     : %.*s", popup_width - 16,
              have_uname ? uname_info.release : "Unavailable");
    mvwprintw(popup, 4, 2, "Machine    : %.*s", popup_width - 16,
              have_uname ? uname_info.machine : "Unavailable");
    mvwprintw(popup, 5, 2, "CPU        : %.*s", popup_width - 16, cpu_model);
    mvwprintw(popup, 6, 2, "Uptime     : %.*s", popup_width - 16, uptime);
    mvwprintw(popup, 7, 2, "Memory Used: %.*s", popup_width - 16, used_mem);
    mvwprintw(popup, 8, 2, "Memory Free: %.*s", popup_width - 16, free_mem);
    mvwprintw(popup, 9, 2, "Memory Tot.: %.*s", popup_width - 16, total_mem);
    mvwprintw(popup, 10, 2, "Load Avg   : %s / %s / %s", load1, load5, load15);
    mvwprintw(popup, 12, 2, "Press ENTER to return to menu.");
    wrefresh(popup);

    int key = 0;
    while (key != '\n' && key != KEY_ENTER && key != '\r') {
        key = wgetch(popup);
    }

    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
    }
    delwin(popup);
}
