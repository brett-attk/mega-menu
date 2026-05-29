#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <ncurses.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "config.h"
#include "ui.h"

#define LINE_BUFFER_SIZE 2048
#define SUDO_PASSWORD_BUFFER_SIZE 256
#define STARTUP_REPAINT_FRAMES 5

typedef struct {
    char **lines;
    size_t count;
    size_t capacity;
} OutputBuffer;

typedef struct {
    WINDOW *menu_win;
    WINDOW *output_win;
    int rows;
    int cols;
    int menu_height;
    int output_height;
} UiWindows;

/* Allocate and return a heap copy of a C string. */
static char *duplicate_string(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len);
    return copy;
}

/* Free all stored output lines and reset buffer metadata. */
static void free_output_buffer(OutputBuffer *output) {
    if (!output || !output->lines) {
        return;
    }

    for (size_t i = 0; i < output->count; i++) {
        free(output->lines[i]);
    }
    free(output->lines);
    output->lines = NULL;
    output->count = 0;
    output->capacity = 0;
}

/* Clear output contents while keeping allocated line capacity. */
static void clear_output_buffer(OutputBuffer *output) {
    if (!output) {
        return;
    }
    for (size_t i = 0; i < output->count; i++) {
        free(output->lines[i]);
    }
    output->count = 0;
}

/* Append one line to the output buffer, growing storage as needed. */
static int append_output_line(OutputBuffer *output, const char *line) {
    if (output->count == output->capacity) {
        size_t new_capacity = output->capacity == 0 ? 16 : output->capacity * 2;
        char **new_lines =
            (char **)realloc(output->lines, sizeof(char *) * new_capacity);
        if (!new_lines) {
            return -1;
        }
        output->lines = new_lines;
        output->capacity = new_capacity;
    }

    output->lines[output->count] = duplicate_string(line);
    if (!output->lines[output->count]) {
        return -1;
    }
    output->count++;
    return 0;
}

/* Trim leading and trailing whitespace in-place. */
static void trim_in_place(char *s) {
    size_t start = 0;
    size_t end = strlen(s);

    while (isspace((unsigned char)s[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)s[end - 1])) {
        end--;
    }

    if (start > 0) {
        memmove(s, s + start, end - start);
    }
    s[end - start] = '\0';
}

/* Overwrite sensitive buffer contents to reduce in-memory exposure. */
static void secure_clear_buffer(char *buffer, size_t len) {
    if (!buffer || len == 0) {
        return;
    }
    volatile char *p = buffer;
    while (len-- > 0) {
        *p++ = '\0';
    }
}

/* Return pointer to first non-whitespace character. */
static const char *skip_whitespace(const char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

/* Check whether a command begins with the sudo token. */
static int command_starts_with_sudo(const char *command) {
    if (!command) {
        return 0;
    }
    const char *s = skip_whitespace(command);
    if (strncmp(s, "sudo", 4) != 0) {
        return 0;
    }
    return s[4] == '\0' || isspace((unsigned char)s[4]);
}

/* Rewrite sudo commands to use stdin password mode with no prompt text. */
static int build_sudo_prompt_command(const char *command, char *out, size_t out_size) {
    if (!command || !out || out_size == 0) {
        return -1;
    }
    const char *s = skip_whitespace(command);
    if (!command_starts_with_sudo(s)) {
        return -1;
    }

    s += 4;
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    int written = snprintf(out, out_size, "sudo -S -p '' %s", s);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

/* Write all bytes to a file descriptor, retrying on interruptions. */
static int write_all(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

/* Redirect stdin/stdout/stderr to /dev/null for detached launches. */
static void redirect_stdio_to_devnull(void) {
    FILE *devnull = fopen("/dev/null", "r+");
    if (!devnull) {
        return;
    }

    int fd = fileno(devnull);
    if (fd >= 0) {
        (void)dup2(fd, STDIN_FILENO);
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
    }
    if (fd > STDERR_FILENO) {
        fclose(devnull);
    }
}

/* Detect whether a graphical display session is available. */
static int has_graphical_display(void) {
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    return (display && display[0] != '\0') || (wayland && wayland[0] != '\0');
}

/*
 * Launch a command detached. When a graphical session exists, first try opening
 * it in a new terminal window; otherwise fall back to background detached launch.
 *
 * Return values:
 *   1 => launched via a new terminal window
 *   0 => launched detached in background fallback mode
 *  -1 => failed to launch
 */
static int launch_detached_command(const char *command) {
    int status_pipe[2];
    if (pipe(status_pipe) != 0) {
        return -1;
    }

    if (fcntl(status_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(status_pipe[0]);
        close(status_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(status_pipe[0]);
        close(status_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(status_pipe[0]);
        if (setsid() < 0) {
            _exit(127);
        }

        pid_t worker = fork();
        if (worker < 0) {
            _exit(127);
        }
        if (worker > 0) {
            _exit(0);
        }

        if (has_graphical_display()) {
            execlp("x-terminal-emulator", "x-terminal-emulator", "-e", "sh", "-lc",
                   command, (char *)NULL);
            execlp("gnome-terminal", "gnome-terminal", "--", "sh", "-lc", command,
                   (char *)NULL);
            execlp("konsole", "konsole", "-e", "sh", "-lc", command, (char *)NULL);
            execlp("xfce4-terminal", "xfce4-terminal", "-e", "sh", "-lc", command,
                   (char *)NULL);
            execlp("xterm", "xterm", "-e", "sh", "-lc", command, (char *)NULL);
            execlp("kitty", "kitty", "-e", "sh", "-lc", command, (char *)NULL);
            execlp("alacritty", "alacritty", "-e", "sh", "-lc", command, (char *)NULL);
            (void)write_all(status_pipe[1], "N", 1);
        } else {
            (void)write_all(status_pipe[1], "D", 1);
        }

        redirect_stdio_to_devnull();
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(status_pipe[1]);
    (void)waitpid(pid, NULL, 0);

    char mode = '\0';
    ssize_t n = read(status_pipe[0], &mode, 1);
    close(status_pipe[0]);

    if (n == 1 && (mode == 'N' || mode == 'D')) {
        return 0;
    }
    return 1;
}

/* Execute a shell command, capture combined stdout/stderr, and record exit status. */
static int run_command_capture_output(const char *command, const char *stdin_data,
                                      OutputBuffer *output) {
    int output_pipe[2];
    int input_pipe[2];
    int use_input_pipe = (stdin_data != NULL);

    if (pipe(output_pipe) != 0) {
        append_output_line(output, "Error: failed to create output pipe.");
        return -1;
    }
    if (use_input_pipe && pipe(input_pipe) != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        append_output_line(output, "Error: failed to create input pipe.");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (use_input_pipe) {
            close(input_pipe[0]);
            close(input_pipe[1]);
        }
        append_output_line(output, "Error: failed to start command.");
        return -1;
    }

    if (pid == 0) {
        /* Child: route stdout/stderr (and optional stdin), then exec shell command. */
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(output_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(output_pipe[0]);
        close(output_pipe[1]);

        if (use_input_pipe) {
            close(input_pipe[1]);
            if (dup2(input_pipe[0], STDIN_FILENO) < 0) {
                _exit(127);
            }
            close(input_pipe[0]);
        }

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(output_pipe[1]);
    if (use_input_pipe) {
        close(input_pipe[0]);
        size_t len = strlen(stdin_data);
        if (len > 0) {
            (void)write_all(input_pipe[1], stdin_data, len);
        }
        (void)write_all(input_pipe[1], "\n", 1);
        close(input_pipe[1]);
    }

    FILE *reader = fdopen(output_pipe[0], "r");
    if (!reader) {
        close(output_pipe[0]);
        append_output_line(output, "Error: failed to read command output.");
        waitpid(pid, NULL, 0);
        return -1;
    }

    char line[LINE_BUFFER_SIZE];
    /* Read command output line-by-line so it can be rendered in the UI buffer. */
    while (fgets(line, sizeof(line), reader)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (append_output_line(output, line) != 0) {
            break;
        }
    }
    fclose(reader);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        append_output_line(output, "Command finished (unknown status).");
        return -1;
    }

    char status_line[128];
    if (WIFEXITED(status)) {
        snprintf(status_line, sizeof(status_line), "Command exited with code %d.",
                 WEXITSTATUS(status));
    } else {
        snprintf(status_line, sizeof(status_line), "Command terminated abnormally.");
    }
    append_output_line(output, "------------------------------");
    append_output_line(output, status_line);
    return 0;
}

/* Show a masked popup prompt for sudo password entry. */
static int prompt_sudo_password(char *password, size_t password_size, OutputBuffer *output) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);

    if (rows < 11 || cols < 50) {
        append_output_line(output, "Terminal too small for sudo password prompt.");
        return -1;
    }

    int popup_height = 9;
    int popup_width = cols - 8;
    if (popup_width > 88) {
        popup_width = 88;
    }
    if (popup_width < 46) {
        popup_width = 46;
    }

    int start_y = (rows - popup_height) / 2;
    int start_x = (cols - popup_width) / 2;
    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) {
        append_output_line(output, "Failed to create sudo password popup.");
        return -1;
    }

    keypad(popup, TRUE);
    if (has_colors()) {
        wbkgd(popup, COLOR_PAIR(PAIR_OUTPUT));
        wattron(popup, COLOR_PAIR(PAIR_BORDER));
    }
    box(popup, 0, 0);
    mvwprintw(popup, 0, 2, " Sudo Password Required ");
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_BORDER));
        wattron(popup, COLOR_PAIR(PAIR_OUTPUT));
    }

    mvwprintw(popup, 1, 2, "Enter your sudo password and press ENTER.");
    mvwprintw(popup, 2, 2, "Press ESC to cancel.");
    mvwprintw(popup, 4, 2, "Password: ");
    wmove(popup, 4, 12);
    wrefresh(popup);

    memset(password, 0, password_size);
    int len = 0;
    int cancelled = 0;
    curs_set(1);
    noecho();

    while (1) {
        int ch = wgetch(popup);
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
            break;
        }
        if (ch == 27) {
            cancelled = 1;
            break;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                password[len] = '\0';
                mvwaddch(popup, 4, 12 + len, ' ');
                wmove(popup, 4, 12 + len);
                wrefresh(popup);
            }
            continue;
        }
        if (isprint(ch) && len < (int)password_size - 1) {
            password[len++] = (char)ch;
            mvwaddch(popup, 4, 12 + len - 1, '*');
            wrefresh(popup);
        }
    }

    curs_set(0);
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
    }
    delwin(popup);

    if (cancelled) {
        secure_clear_buffer(password, password_size);
        append_output_line(output, "Sudo command cancelled.");
        return 1;
    }
    if (password[0] == '\0') {
        append_output_line(output, "No password entered; sudo command cancelled.");
        return 1;
    }
    return 0;
}

/* Apply active theme background attributes to existing windows. */
static void apply_theme_to_windows(const UiWindows *ui) {
    if (!ui || !has_colors()) {
        return;
    }
    if (ui->menu_win) {
        wbkgd(ui->menu_win, COLOR_PAIR(PAIR_MENU));
    }
    if (ui->output_win) {
        wbkgd(ui->output_win, COLOR_PAIR(PAIR_OUTPUT));
    }
    bkgd(COLOR_PAIR(PAIR_MENU));
}

/* Destroy menu/output windows if they currently exist. */
static void destroy_ui_windows(UiWindows *ui) {
    if (!ui) {
        return;
    }
    if (ui->menu_win) {
        delwin(ui->menu_win);
        ui->menu_win = NULL;
    }
    if (ui->output_win) {
        delwin(ui->output_win);
        ui->output_win = NULL;
    }
}

/* Ensure UI windows exist and are sized to the current terminal dimensions. */
static int setup_ui_windows(UiWindows *ui) {
    if (!ui) {
        return -1;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    ui->rows = rows;
    ui->cols = cols;

    if (rows < 8 || cols < 20) {
        destroy_ui_windows(ui);
        return -1;
    }

    /* Keep the menu and output panes usable across small terminal sizes. */
    int menu_height = rows / 2;
    if (menu_height < 5) {
        menu_height = 5;
    }
    if (menu_height > rows - 3) {
        menu_height = rows - 3;
    }
    int output_height = rows - menu_height;
    if (output_height < 3) {
        output_height = 3;
        menu_height = rows - output_height;
    }

    if (ui->menu_win && ui->output_win && ui->menu_height == menu_height &&
        ui->output_height == output_height && ui->cols == cols && ui->rows == rows) {
        return 0;
    }

    destroy_ui_windows(ui);
    ui->menu_win = newwin(menu_height, cols, 0, 0);
    ui->output_win = newwin(output_height, cols, menu_height, 0);
    if (!ui->menu_win || !ui->output_win) {
        destroy_ui_windows(ui);
        return -1;
    }

    keypad(ui->menu_win, TRUE);
    ui->menu_height = menu_height;
    ui->output_height = output_height;
    return 0;
}

/* Draw the output pane with the most recent lines that fit on screen. */
static void draw_output_window(WINDOW *output_win, const OutputBuffer *output) {
    int height = 0;
    int width = 0;
    getmaxyx(output_win, height, width);

    werase(output_win);
    if (has_colors()) {
        wattron(output_win, COLOR_PAIR(PAIR_BORDER));
    }
    box(output_win, 0, 0);
    mvwprintw(output_win, 0, 2, " Output ");
    if (has_colors()) {
        wattroff(output_win, COLOR_PAIR(PAIR_BORDER));
        wattron(output_win, COLOR_PAIR(PAIR_OUTPUT));
    }

    int visible_rows = height - 2;
    if (visible_rows <= 0) {
        wrefresh(output_win);
        return;
    }

    size_t start = 0;
    if (output->count > (size_t)visible_rows) {
        start = output->count - (size_t)visible_rows;
    }

    for (int row = 0; row < visible_rows; row++) {
        size_t idx = start + (size_t)row;
        if (idx >= output->count) {
            break;
        }
        int printable = width - 2;
        if (printable < 1) {
            printable = 1;
        }
        mvwprintw(output_win, row + 1, 1, "%.*s", printable, output->lines[idx]);
    }
    if (has_colors()) {
        wattroff(output_win, COLOR_PAIR(PAIR_OUTPUT));
    }
    wrefresh(output_win);
}

/* Build an array of item indices in current group display order. */
static int *build_grouped_item_order(const MenuConfig *config) {
    if (!config || config->count == 0) {
        return NULL;
    }

    int *order = (int *)malloc(sizeof(int) * config->count);
    if (!order) {
        return NULL;
    }
    size_t idx = 0;
    for (size_t g = 0; g < config->group_count; g++) {
        for (size_t i = 0; i < config->count; i++) {
            if (strcmp(config->items[i].group, config->groups[g]) == 0) {
                order[idx++] = (int)i;
            }
        }
    }
    /* Include orphaned groups defensively if config data is inconsistent. */
    for (size_t i = 0; i < config->count; i++) {
        int found = 0;
        for (size_t j = 0; j < idx; j++) {
            if (order[j] == (int)i) {
                found = 1;
                break;
            }
        }
        if (!found) {
            order[idx++] = (int)i;
        }
    }
    return order;
}

/* Move current selection in grouped display order by +/-1 with wrap-around. */
static int move_selection_in_group_order(const MenuConfig *config, int selected, int direction) {
    if (!config || config->count == 0 || (direction != -1 && direction != 1)) {
        return selected;
    }

    int *order = build_grouped_item_order(config);
    if (!order) {
        return selected;
    }

    int pos = 0;
    for (size_t i = 0; i < config->count; i++) {
        if (order[i] == selected) {
            pos = (int)i;
            break;
        }
    }
    pos = (pos + direction + (int)config->count) % (int)config->count;
    int next = order[pos];
    free(order);
    return next;
}

/* Count rendered rows before selected item in grouped menu view. */
static int grouped_row_for_selected_item(const MenuConfig *config, int selected) {
    if (!config) {
        return 0;
    }
    int row = 0;
    for (size_t g = 0; g < config->group_count; g++) {
        int group_has_items = 0;
        for (size_t i = 0; i < config->count; i++) {
            if (strcmp(config->items[i].group, config->groups[g]) == 0) {
                group_has_items = 1;
                break;
            }
        }
        if (!group_has_items) {
            continue;
        }

        row++; /* group header row */
        for (size_t i = 0; i < config->count; i++) {
            if (strcmp(config->items[i].group, config->groups[g]) != 0) {
                continue;
            }
            if ((int)i == selected) {
                return row;
            }
            row++;
        }
        int has_later_visible_group = 0;
        for (size_t next_g = g + 1; next_g < config->group_count; next_g++) {
            for (size_t i = 0; i < config->count; i++) {
                if (strcmp(config->items[i].group, config->groups[next_g]) == 0) {
                    has_later_visible_group = 1;
                    break;
                }
            }
            if (has_later_visible_group) {
                break;
            }
        }
        if (has_later_visible_group) {
            row++; /* spacer row between groups */
        }
    }
    return 0;
}

/* Render both menu and output panes for the current application state. */
static void draw_menu(const MenuConfig *config, int selected, const OutputBuffer *output,
                      UiWindows *ui) {
    if (setup_ui_windows(ui) != 0) {
        erase();
        mvprintw(0, 0, "Terminal too small. Resize to at least 20x8.");
        mvprintw(1, 0, "Current size: %dx%d", ui->cols, ui->rows);
        mvprintw(2, 0, "Press q to quit.");
        refresh();
        return;
    }

    WINDOW *menu_win = ui->menu_win;
    WINDOW *output_win = ui->output_win;
    apply_theme_to_windows(ui);

    werase(menu_win);
    if (has_colors()) {
        wattron(menu_win, COLOR_PAIR(PAIR_BORDER));
    }
    box(menu_win, 0, 0);
    mvwprintw(menu_win, 0, 2, " MegaMenu ");
    mvwprintw(menu_win, 1, 1,
              "UP/DOWN move | ENTER run | a add | e edit | d delete | g groups | c theme | q quit");
    if (has_colors()) {
        wattroff(menu_win, COLOR_PAIR(PAIR_BORDER));
        wattron(menu_win, COLOR_PAIR(PAIR_MENU));
    }

    int list_start_row = 3;
    int visible_items = ui->menu_height - list_start_row - 1;
    if (visible_items < 1) {
        visible_items = 1;
    }

    int selected_row = grouped_row_for_selected_item(config, selected);
    int first_visible = 0;
    if (selected_row >= visible_items) {
        first_visible = selected_row - visible_items + 1;
    }

    int render_row = 0;
    int display_row = 0;
    for (size_t g = 0; g < config->group_count; g++) {
        int group_has_items = 0;
        for (size_t i = 0; i < config->count; i++) {
            if (strcmp(config->items[i].group, config->groups[g]) == 0) {
                group_has_items = 1;
                break;
            }
        }
        if (!group_has_items) {
            continue;
        }

        if (display_row >= first_visible && render_row < visible_items) {
            int printable = ui->cols - 4;
            if (printable < 1) {
                printable = 1;
            }
            mvwprintw(menu_win, list_start_row + render_row, 2, "[%.*s]", printable - 2,
                      config->groups[g]);
            render_row++;
        }
        display_row++;

        for (size_t i = 0; i < config->count; i++) {
            if (strcmp(config->items[i].group, config->groups[g]) != 0) {
                continue;
            }
            if (display_row >= first_visible && render_row < visible_items) {
                if ((int)i == selected) {
                    if (has_colors()) {
                        wattron(menu_win, COLOR_PAIR(PAIR_SELECTED));
                    } else {
                        wattron(menu_win, A_REVERSE);
                    }
                }
                int printable = ui->cols - 6;
                if (printable < 1) {
                    printable = 1;
                }
                mvwprintw(menu_win, list_start_row + render_row, 4, "%.*s", printable,
                          config->items[i].label);
                if ((int)i == selected) {
                    if (has_colors()) {
                        wattroff(menu_win, COLOR_PAIR(PAIR_SELECTED));
                    } else {
                        wattroff(menu_win, A_REVERSE);
                    }
                }
                render_row++;
            }
            display_row++;
            if (render_row >= visible_items) {
                break;
            }
        }
        if (render_row >= visible_items) {
            break;
        }

        int has_later_visible_group = 0;
        for (size_t next_g = g + 1; next_g < config->group_count; next_g++) {
            for (size_t i = 0; i < config->count; i++) {
                if (strcmp(config->items[i].group, config->groups[next_g]) == 0) {
                    has_later_visible_group = 1;
                    break;
                }
            }
            if (has_later_visible_group) {
                break;
            }
        }
        if (has_later_visible_group) {
            if (display_row >= first_visible && render_row < visible_items) {
                /* leave one blank spacer row between groups */
                render_row++;
            }
            display_row++;
        }
    }
    if (has_colors()) {
        wattroff(menu_win, COLOR_PAIR(PAIR_MENU));
    }

    wrefresh(menu_win);
    draw_output_window(output_win, output);
}

/* Force a full repaint pass to avoid partial first-frame artifacts. */
static void force_initial_paint(const UiWindows *ui) {
    if (!ui) {
        return;
    }
    if (ui->menu_win) {
        touchwin(ui->menu_win);
        wrefresh(ui->menu_win);
    }
    if (ui->output_win) {
        touchwin(ui->output_win);
        wrefresh(ui->output_win);
    }
}

/* Execute the selected menu command and stream its output into the UI buffer. */
static void execute_item(const MenuItem *item, OutputBuffer *output) {
    if (item->launch_detached) {
        clear_output_buffer(output);
        if (append_output_line(output, "------------------------------") != 0 ||
            append_output_line(output, "Launching command detached:") != 0 ||
            append_output_line(output, item->command) != 0) {
            return;
        }

        int detached_result = launch_detached_command(item->command);
        if (detached_result < 0) {
            append_output_line(output, "Failed to start detached command.");
            append_output_line(output, "------------------------------");
            return;
        }
        if (detached_result == 1) {
            append_output_line(output, "Launched in a new terminal window.");
        } else {
            append_output_line(output, "No terminal window available; launched in background.");
        }
        append_output_line(output, "Detached launch requested; menu is ready.");
        append_output_line(output, "------------------------------");
        return;
    }

    clear_output_buffer(output);
    if (append_output_line(output, "------------------------------") != 0 ||
        append_output_line(output, "Running command:") != 0 ||
        append_output_line(output, item->command) != 0 ||
        append_output_line(output, "------------------------------") != 0) {
        return;
    }

    char shell_cmd[LINE_BUFFER_SIZE];
    const char *stdin_data = NULL;
    char sudo_password[SUDO_PASSWORD_BUFFER_SIZE];
    int has_sudo_password = 0;

    /*
     * For sudo commands, ask for password in-app and pass it through stdin
     * to avoid leaking credentials through command line arguments.
     */
    if (command_starts_with_sudo(item->command) && geteuid() != 0) {
        int prompt_status = prompt_sudo_password(sudo_password, sizeof(sudo_password), output);
        if (prompt_status != 0) {
            return;
        }
        if (build_sudo_prompt_command(item->command, shell_cmd, sizeof(shell_cmd)) != 0) {
            secure_clear_buffer(sudo_password, sizeof(sudo_password));
            append_output_line(output, "Error: sudo command is too long.");
            return;
        }
        stdin_data = sudo_password;
        has_sudo_password = 1;
    } else {
        if (snprintf(shell_cmd, sizeof(shell_cmd), "%s", item->command) >=
            (int)sizeof(shell_cmd)) {
            append_output_line(output, "Error: command is too long.");
            return;
        }
    }

    run_command_capture_output(shell_cmd, stdin_data, output);
    /* Always wipe sudo password memory once the command has been launched. */
    if (has_sudo_password) {
        secure_clear_buffer(sudo_password, sizeof(sudo_password));
    }
}

/* Prompt in-popup group selector over available groups. */
static int prompt_group_choice(WINDOW *popup, int y, int x, int width, const MenuConfig *config,
                               int *group_index) {
    if (!popup || !config || config->group_count == 0 || !group_index) {
        return -1;
    }

    while (1) {
        int idx = *group_index;
        if (idx < 0 || idx >= (int)config->group_count) {
            idx = 0;
        }
        *group_index = idx;

        mvwprintw(popup, y, x, "%*s", width, "");
        mvwprintw(popup, y, x, "%.*s", width, config->groups[idx]);
        wmove(popup, y, x);
        wrefresh(popup);

        int ch = wgetch(popup);
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
            return 0;
        }
        if (ch == KEY_LEFT || ch == KEY_UP) {
            *group_index = (*group_index - 1 + (int)config->group_count) % (int)config->group_count;
            continue;
        }
        if (ch == KEY_RIGHT || ch == KEY_DOWN || ch == ' ') {
            *group_index = (*group_index + 1) % (int)config->group_count;
            continue;
        }
    }
}

/* Manage groups popup for adding/deleting groups and saving config changes. */
static int manage_groups_interactively(const char *config_path, MenuConfig *config, int *selected,
                                       OutputBuffer *output) {
    (void)selected;
    if (!config || add_group(config, DEFAULT_GROUP_NAME) != 0) {
        append_output_line(output, "Failed to initialize groups.");
        return -1;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 14 || cols < 60) {
        append_output_line(output, "Terminal too small for group manager.");
        return -1;
    }

    int popup_height = 14;
    int popup_width = cols - 10;
    if (popup_width > 96) {
        popup_width = 96;
    }
    if (popup_width < 60) {
        popup_width = 60;
    }

    int start_y = (rows - popup_height) / 2;
    int start_x = (cols - popup_width) / 2;
    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) {
        append_output_line(output, "Failed to create group manager popup.");
        return -1;
    }

    keypad(popup, TRUE);
    int selected_group = 0;
    int done = 0;
    while (!done) {
        werase(popup);
        if (has_colors()) {
            wbkgd(popup, COLOR_PAIR(PAIR_OUTPUT));
            wattron(popup, COLOR_PAIR(PAIR_BORDER));
        }
        box(popup, 0, 0);
        mvwprintw(popup, 0, 2, " Manage Groups ");
        if (has_colors()) {
            wattroff(popup, COLOR_PAIR(PAIR_BORDER));
            wattron(popup, COLOR_PAIR(PAIR_OUTPUT));
        }
        mvwprintw(popup, 1, 2, "UP/DOWN select | a add | r rename | d delete | ENTER close");

        int list_start = 3;
        int visible = popup_height - list_start - 2;
        if (visible < 1) {
            visible = 1;
        }
        if (selected_group >= (int)config->group_count) {
            selected_group = (int)config->group_count - 1;
        }
        if (selected_group < 0) {
            selected_group = 0;
        }

        int first = 0;
        if (selected_group >= visible) {
            first = selected_group - visible + 1;
        }

        for (int row = 0; row < visible; row++) {
            int gi = first + row;
            if (gi >= (int)config->group_count) {
                break;
            }
            int in_use = count_items_in_group(config, config->groups[gi]);
            if (gi == selected_group) {
                if (has_colors()) {
                    wattron(popup, COLOR_PAIR(PAIR_SELECTED));
                } else {
                    wattron(popup, A_REVERSE);
                }
            }
            mvwprintw(popup, list_start + row, 2, "%.*s%s", popup_width - 8, config->groups[gi],
                      in_use > 0 ? " *" : "");
            if (gi == selected_group) {
                if (has_colors()) {
                    wattroff(popup, COLOR_PAIR(PAIR_SELECTED));
                } else {
                    wattroff(popup, A_REVERSE);
                }
            }
        }
        mvwprintw(popup, popup_height - 2, 2, "* group contains items");
        wrefresh(popup);

        int ch = wgetch(popup);
        if (ch == KEY_UP) {
            selected_group =
                (selected_group - 1 + (int)config->group_count) % (int)config->group_count;
            continue;
        }
        if (ch == KEY_DOWN) {
            selected_group = (selected_group + 1) % (int)config->group_count;
            continue;
        }
        if (ch == 'a' || ch == 'A') {
            char name[128] = {0};
            mvwprintw(popup, popup_height - 2, 2, "%*s", popup_width - 4, "");
            mvwprintw(popup, popup_height - 2, 2, "New group: ");
            wmove(popup, popup_height - 2, 13);
            echo();
            curs_set(1);
            wgetnstr(popup, name, (int)sizeof(name) - 1);
            noecho();
            curs_set(0);
            trim_in_place(name);
            if (name[0] == '\0') {
                continue;
            }
            if (add_group(config, name) != 0) {
                append_output_line(output, "Failed to add group.");
                continue;
            }
            if (write_config_file(config_path, config) != 0) {
                append_output_line(output, "Added group in memory, but failed to write config.");
                continue;
            }
            selected_group = find_group_index(config, name);
            char msg[192];
            snprintf(msg, sizeof(msg), "Added group: %s", name);
            append_output_line(output, msg);
            continue;
        }
        if (ch == 'r' || ch == 'R') {
            const char *group_name = config->groups[selected_group];
            if (strcmp(group_name, DEFAULT_GROUP_NAME) == 0) {
                append_output_line(output, "Cannot rename default group.");
                continue;
            }

            char new_name[128] = {0};
            mvwprintw(popup, popup_height - 2, 2, "%*s", popup_width - 4, "");
            mvwprintw(popup, popup_height - 2, 2, "Rename group to: ");
            wmove(popup, popup_height - 2, 19);
            echo();
            curs_set(1);
            wgetnstr(popup, new_name, (int)sizeof(new_name) - 1);
            noecho();
            curs_set(0);
            trim_in_place(new_name);

            if (new_name[0] == '\0' || strcmp(new_name, group_name) == 0) {
                continue;
            }
            if (find_group_index(config, new_name) >= 0) {
                append_output_line(output, "Group already exists.");
                continue;
            }

            char *old_name_copy = duplicate_string(group_name);
            char *group_replacement = duplicate_string(new_name);
            if (!old_name_copy || !group_replacement) {
                free(old_name_copy);
                free(group_replacement);
                append_output_line(output, "Out of memory while renaming group.");
                continue;
            }

            free(config->groups[selected_group]);
            config->groups[selected_group] = group_replacement;

            int rename_failed = 0;
            for (size_t i = 0; i < config->count; i++) {
                if (strcmp(config->items[i].group, old_name_copy) == 0) {
                    char *replacement = duplicate_string(new_name);
                    if (!replacement) {
                        rename_failed = 1;
                        break;
                    }
                    free(config->items[i].group);
                    config->items[i].group = replacement;
                }
            }

            if (rename_failed || write_config_file(config_path, config) != 0) {
                append_output_line(output, "Renamed in memory, but failed to write config.");
                free(old_name_copy);
                continue;
            }

            char msg[192];
            snprintf(msg, sizeof(msg), "Renamed group: %s -> %s", old_name_copy, new_name);
            append_output_line(output, msg);
            free(old_name_copy);
            continue;
        }
        if (ch == 'd' || ch == 'D') {
            const char *group_name = config->groups[selected_group];
            if (strcmp(group_name, DEFAULT_GROUP_NAME) == 0) {
                append_output_line(output, "Cannot delete default group.");
                continue;
            }

            for (size_t i = 0; i < config->count; i++) {
                if (strcmp(config->items[i].group, group_name) == 0) {
                    char *replacement = duplicate_string(DEFAULT_GROUP_NAME);
                    if (!replacement) {
                        append_output_line(output, "Out of memory while reassigning groups.");
                        continue;
                    }
                    free(config->items[i].group);
                    config->items[i].group = replacement;
                }
            }

            free(config->groups[selected_group]);
            for (size_t i = (size_t)selected_group; i + 1 < config->group_count; i++) {
                config->groups[i] = config->groups[i + 1];
            }
            config->group_count--;
            if (selected_group >= (int)config->group_count) {
                selected_group = (int)config->group_count - 1;
            }
            if (selected_group < 0) {
                selected_group = 0;
            }

            if (write_config_file(config_path, config) != 0) {
                append_output_line(output, "Deleted group in memory, but failed to write config.");
                continue;
            }

            append_output_line(output, "Deleted group; affected items moved to Ungrouped.");
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
            done = 1;
        }
    }

    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
    }
    delwin(popup);
    return 0;
}

/* Prompt for and append a new menu item, updating file and memory state. */
static int add_item_interactively(const char *config_path, MenuConfig *config,
                                  OutputBuffer *output, int *selected) {
    if (!config || (config->group_count == 0 && add_group(config, DEFAULT_GROUP_NAME) != 0)) {
        append_output_line(output, "No groups available for add-item popup.");
        return -1;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);

    if (rows < 15 || cols < 50) {
        append_output_line(output, "Terminal too small for add-item popup.");
        return -1;
    }

    int popup_height = 16;
    int popup_width = cols - 6;
    if (popup_width > 100) {
        popup_width = 100;
    }
    if (popup_width < 44) {
        popup_width = 44;
    }

    int start_y = (rows - popup_height) / 2;
    int start_x = (cols - popup_width) / 2;
    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) {
        append_output_line(output, "Failed to create add-item popup window.");
        return -1;
    }

    keypad(popup, TRUE);
    if (has_colors()) {
        wbkgd(popup, COLOR_PAIR(PAIR_OUTPUT));
        wattron(popup, COLOR_PAIR(PAIR_BORDER));
    }
    box(popup, 0, 0);
    mvwprintw(popup, 0, 2, " Add Menu Item ");
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_BORDER));
        wattron(popup, COLOR_PAIR(PAIR_OUTPUT));
    }

    mvwprintw(popup, 1, 2, "Enter values and press ENTER after each field.");
    mvwprintw(popup, 2, 2, "Leave Label empty to cancel.");
    mvwprintw(popup, 4, 2, "Label  : ");
    mvwprintw(popup, 6, 2, "Command: ");
    mvwprintw(popup, 8, 2, "[ ] Launch detached from menu process");
    mvwprintw(popup, 10, 2, "Group  : ");
    mvwprintw(popup, 12, 2, "SPACE toggles checkbox; arrows change group; ENTER confirms.");
    wrefresh(popup);

    char label[256] = {0};
    char command[LINE_BUFFER_SIZE] = {0};
    int launch_detached = 0;
    int selected_group = find_group_index(config, DEFAULT_GROUP_NAME);
    if (selected_group < 0) {
        selected_group = 0;
    }

    echo();
    curs_set(1);

    wmove(popup, 4, 11);
    wgetnstr(popup, label, (int)sizeof(label) - 1);
    trim_in_place(label);

    if (label[0] != '\0') {
        wmove(popup, 6, 11);
        wgetnstr(popup, command, (int)sizeof(command) - 1);
        trim_in_place(command);

        mvwaddch(popup, 8, 3, launch_detached ? 'x' : ' ');
        wmove(popup, 8, 3);
        wrefresh(popup);
        while (1) {
            int ch = wgetch(popup);
            if (ch == ' ') {
                launch_detached = !launch_detached;
                mvwaddch(popup, 8, 3, launch_detached ? 'x' : ' ');
                wmove(popup, 8, 3);
                wrefresh(popup);
                continue;
            }
            if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
                break;
            }
        }

        if (prompt_group_choice(popup, 10, 11, popup_width - 14, config, &selected_group) != 0) {
            noecho();
            curs_set(0);
            if (has_colors()) {
                wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
            }
            delwin(popup);
            append_output_line(output, "Failed to select group.");
            return -1;
        }
    }

    noecho();
    curs_set(0);

    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
    }
    delwin(popup);

    if (label[0] == '\0') {
        append_output_line(output, "Add item cancelled.");
        return 0;
    }

    if (strchr(label, '|') != NULL) {
        append_output_line(output, "Invalid label: '|' is not allowed.");
        return -1;
    }

    if (command[0] == '\0') {
        append_output_line(output, "Invalid command: cannot be empty.");
        return -1;
    }

    if (add_item(config, label, command, launch_detached, config->groups[selected_group]) != 0) {
        append_output_line(output, "Failed to add in-memory item.");
        return -1;
    }

    if (write_config_file(config_path, config) != 0) {
        size_t last_index = config->count - 1;
        free(config->items[last_index].label);
        free(config->items[last_index].command);
        free(config->items[last_index].group);
        config->count--;
        append_output_line(output, "Added in memory, but failed to write config file.");
        return -1;
    }

    if (selected) {
        *selected = (int)config->count - 1;
    }

    char message[LINE_BUFFER_SIZE + 64];
    snprintf(message, sizeof(message), "Added item: %s [%s]%s", label, config->groups[selected_group],
             launch_detached ? " (detached)" : "");
    append_output_line(output, message);
    return 0;
}

/* Render and edit a single text field with an initial prefilled value. */
static void edit_prefilled_field(WINDOW *popup, int y, int x, char *buffer, size_t buffer_size,
                                 int field_width) {
    if (!popup || !buffer || buffer_size == 0) {
        return;
    }

    size_t len = strlen(buffer);
    if (len >= buffer_size) {
        len = buffer_size - 1;
        buffer[len] = '\0';
    }
    size_t cursor = len;

    while (1) {
        mvwprintw(popup, y, x, "%*s", field_width, "");
        mvwprintw(popup, y, x, "%.*s", field_width, buffer);
        wmove(popup, y, x + (int)cursor);
        wrefresh(popup);

        int ch = wgetch(popup);
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
            break;
        }
        if (ch == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
            }
            continue;
        }
        if (ch == KEY_RIGHT) {
            if (cursor < len) {
                cursor++;
            }
            continue;
        }
        if (ch == KEY_HOME) {
            cursor = 0;
            continue;
        }
        if (ch == KEY_END) {
            cursor = len;
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cursor > 0) {
                memmove(buffer + cursor - 1, buffer + cursor, len - cursor + 1);
                cursor--;
                len--;
            }
            continue;
        }
        if (ch == KEY_DC) {
            if (cursor < len) {
                memmove(buffer + cursor, buffer + cursor + 1, len - cursor);
                len--;
            }
            continue;
        }
        if (isprint(ch) && len < buffer_size - 1 && (int)len < field_width) {
            memmove(buffer + cursor + 1, buffer + cursor, len - cursor + 1);
            buffer[cursor] = (char)ch;
            cursor++;
            len++;
        }
    }
}

/* Prompt to edit the selected menu item and persist any changes. */
static int edit_item_interactively(const char *config_path, MenuConfig *config,
                                   int selected, OutputBuffer *output) {
    if (!config || (config->group_count == 0 && add_group(config, DEFAULT_GROUP_NAME) != 0)) {
        append_output_line(output, "No groups available for edit-item popup.");
        return -1;
    }
    if (!config || config->count == 0 || selected < 0 ||
        selected >= (int)config->count) {
        append_output_line(output, "No valid item selected to edit.");
        return -1;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 18 || cols < 60) {
        append_output_line(output, "Terminal too small for edit-item popup.");
        return -1;
    }

    int popup_height = 17;
    int popup_width = cols - 6;
    if (popup_width > 110) {
        popup_width = 110;
    }
    if (popup_width < 56) {
        popup_width = 56;
    }

    int start_y = (rows - popup_height) / 2;
    int start_x = (cols - popup_width) / 2;
    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) {
        append_output_line(output, "Failed to create edit-item popup window.");
        return -1;
    }

    keypad(popup, TRUE);
    if (has_colors()) {
        wbkgd(popup, COLOR_PAIR(PAIR_OUTPUT));
        wattron(popup, COLOR_PAIR(PAIR_BORDER));
    }
    box(popup, 0, 0);
    mvwprintw(popup, 0, 2, " Edit Menu Item ");
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_BORDER));
        wattron(popup, COLOR_PAIR(PAIR_OUTPUT));
    }

    mvwprintw(popup, 1, 2, "Edit values directly, then press ENTER for each field.");
    mvwprintw(popup, 2, 2, "Use arrow keys, backspace, delete, home, and end.");
    mvwprintw(popup, 3, 2, "Current Label  : %.*s", popup_width - 20,
              config->items[selected].label);
    mvwprintw(popup, 4, 2, "Current Command: %.*s", popup_width - 20,
              config->items[selected].command);
    mvwprintw(popup, 5, 2, "Current Detached: %s",
              config->items[selected].launch_detached ? "yes" : "no");
    mvwprintw(popup, 6, 2, "Current Group   : %.*s", popup_width - 20,
              config->items[selected].group);
    mvwprintw(popup, 7, 2, "Label      : ");
    mvwprintw(popup, 9, 2, "Command    : ");
    mvwprintw(popup, 11, 2, "[ ] Launch detached from menu process");
    mvwprintw(popup, 13, 2, "Group      : ");
    mvwprintw(popup, 15, 2, "SPACE toggles checkbox; arrows change group; ENTER confirms.");
    wrefresh(popup);

    char new_label[256];
    char new_command[LINE_BUFFER_SIZE];
    snprintf(new_label, sizeof(new_label), "%s", config->items[selected].label);
    snprintf(new_command, sizeof(new_command), "%s", config->items[selected].command);
    int new_launch_detached = config->items[selected].launch_detached;
    int selected_group = find_group_index(config, config->items[selected].group);
    if (selected_group < 0) {
        selected_group = 0;
    }

    curs_set(1);
    noecho();

    int label_x = 15;
    int command_x = 15;
    int field_width = popup_width - label_x - 2;
    if (field_width < 10) {
        field_width = 10;
    }

    edit_prefilled_field(popup, 7, label_x, new_label, sizeof(new_label), field_width);
    trim_in_place(new_label);

    edit_prefilled_field(popup, 9, command_x, new_command, sizeof(new_command), field_width);
    trim_in_place(new_command);

    mvwaddch(popup, 11, 3, new_launch_detached ? 'x' : ' ');
    wmove(popup, 11, 3);
    wrefresh(popup);
    while (1) {
        int ch = wgetch(popup);
        if (ch == ' ') {
            new_launch_detached = !new_launch_detached;
            mvwaddch(popup, 11, 3, new_launch_detached ? 'x' : ' ');
            wmove(popup, 11, 3);
            wrefresh(popup);
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
            break;
        }
    }

    if (prompt_group_choice(popup, 13, 15, popup_width - 18, config, &selected_group) != 0) {
        curs_set(0);
        if (has_colors()) {
            wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
        }
        delwin(popup);
        append_output_line(output, "Failed to select group.");
        return -1;
    }

    curs_set(0);

    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
    }
    delwin(popup);

    const char *final_label = new_label;
    const char *final_command = new_command;

    if (strchr(final_label, '|') != NULL) {
        append_output_line(output, "Invalid label: '|' is not allowed.");
        return -1;
    }
    if (final_command[0] == '\0') {
        append_output_line(output, "Invalid command: cannot be empty.");
        return -1;
    }

    if (strcmp(final_label, config->items[selected].label) == 0 &&
        strcmp(final_command, config->items[selected].command) == 0 &&
        new_launch_detached == config->items[selected].launch_detached &&
        strcmp(config->groups[selected_group], config->items[selected].group) == 0) {
        append_output_line(output, "Edit cancelled (no changes).");
        return 0;
    }

    char *label_copy = duplicate_string(final_label);
    char *command_copy = duplicate_string(final_command);
    char *group_copy = duplicate_string(config->groups[selected_group]);
    if (!label_copy || !command_copy || !group_copy) {
        free(label_copy);
        free(command_copy);
        free(group_copy);
        append_output_line(output, "Out of memory while editing item.");
        return -1;
    }

    free(config->items[selected].label);
    free(config->items[selected].command);
    free(config->items[selected].group);
    config->items[selected].label = label_copy;
    config->items[selected].command = command_copy;
    config->items[selected].group = group_copy;
    config->items[selected].launch_detached = new_launch_detached;

    if (write_config_file(config_path, config) != 0) {
        append_output_line(output, "Edited in memory, but failed to write config file.");
        return -1;
    }

    char message[LINE_BUFFER_SIZE + 64];
    snprintf(message, sizeof(message), "Edited item: %s [%s]%s", config->items[selected].label,
             config->items[selected].group,
             config->items[selected].launch_detached ? " (detached)" : "");
    append_output_line(output, message);
    return 0;
}

/* Confirm and delete the selected menu item, then rewrite config file. */
static int delete_item_interactively(const char *config_path, MenuConfig *config,
                                     int *selected, OutputBuffer *output) {
    if (!config || !selected || config->count == 0 || *selected < 0 ||
        *selected >= (int)config->count) {
        append_output_line(output, "No valid item selected to delete.");
        return -1;
    }

    if (config->count <= 1) {
        append_output_line(output, "Cannot delete the last remaining menu item.");
        return -1;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 12 || cols < 60) {
        append_output_line(output, "Terminal too small for delete-item popup.");
        return -1;
    }

    int popup_height = 10;
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
        append_output_line(output, "Failed to create delete-item popup window.");
        return -1;
    }

    keypad(popup, TRUE);
    if (has_colors()) {
        wbkgd(popup, COLOR_PAIR(PAIR_OUTPUT));
        wattron(popup, COLOR_PAIR(PAIR_BORDER));
    }
    box(popup, 0, 0);
    mvwprintw(popup, 0, 2, " Delete Menu Item ");
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_BORDER));
        wattron(popup, COLOR_PAIR(PAIR_OUTPUT));
    }

    const int current_index = *selected;
    mvwprintw(popup, 2, 2, "Delete selected item?");
    mvwprintw(popup, 4, 2, "Label  : %.*s", popup_width - 13,
              config->items[current_index].label);
    mvwprintw(popup, 5, 2, "Command: %.*s", popup_width - 13,
              config->items[current_index].command);
    mvwprintw(popup, 7, 2, "Press y to confirm, any other key to cancel.");
    wrefresh(popup);

    int key = wgetch(popup);
    if (has_colors()) {
        wattroff(popup, COLOR_PAIR(PAIR_OUTPUT));
    }
    delwin(popup);

    if (!(key == 'y' || key == 'Y')) {
        append_output_line(output, "Delete cancelled.");
        return 0;
    }

    char *deleted_label = duplicate_string(config->items[current_index].label);
    if (!deleted_label) {
        append_output_line(output, "Out of memory while deleting item.");
        return -1;
    }

    free(config->items[current_index].label);
    free(config->items[current_index].command);
    free(config->items[current_index].group);
    for (size_t i = (size_t)current_index; i + 1 < config->count; i++) {
        config->items[i] = config->items[i + 1];
    }
    config->count--;

    if (*selected >= (int)config->count) {
        *selected = (int)config->count - 1;
    }

    if (write_config_file(config_path, config) != 0) {
        append_output_line(output, "Deleted in memory, but failed to write config file.");
        free(deleted_label);
        return -1;
    }

    char message[LINE_BUFFER_SIZE + 64];
    snprintf(message, sizeof(message), "Deleted item: %s", deleted_label);
    append_output_line(output, message);
    free(deleted_label);
    return 0;
}

/* Load config, run the ncurses event loop, and clean up on exit. */
int main(int argc, char **argv) {
    const char *config_path = "megamenu.config";
    if (argc > 1) {
        config_path = argv[1];
    }

    MenuConfig config = {0};
    if (parse_config_file(config_path, &config) != 0) {
        free_menu_config(&config);
        return EXIT_FAILURE;
    }

    if (config.count == 0) {
        fprintf(stderr, "No menu items found in %s\n", config_path);
        free_menu_config(&config);
        return EXIT_FAILURE;
    }

    init_ui();
    int theme_index = config.theme_index;
    if (theme_index < 0 || theme_index >= ui_theme_count()) {
        theme_index = 0;
        config.theme_index = theme_index;
    }
    apply_theme(theme_index);

    int selected = 0;
    int running = 1;
    OutputBuffer output = {0};
    UiWindows ui = {0};
    append_output_line(&output, "Select an item and press ENTER to run.");
    if (has_colors()) {
        char line[128];
        snprintf(line, sizeof(line), "Theme: %s (press 'c' to cycle)",
                 ui_theme_name(theme_index));
        append_output_line(&output, line);
    } else {
        append_output_line(&output, "Terminal does not support color themes.");
    }

    clear();
    refresh();
    draw_menu(&config, selected, &output, &ui);
    force_initial_paint(&ui);
    int startup_repaints = STARTUP_REPAINT_FRAMES;
    /* Main input loop: handle keys, mutate state, redraw when needed. */
    while (running) {
        int key = getch();
        int needs_redraw = 0;
        switch (key) {
            case ERR:
                /* Poll timeout: repaint a few startup frames to stabilize initial render. */
                if (startup_repaints > 0) {
                    startup_repaints--;
                    needs_redraw = 1;
                }
                break;
            case KEY_UP:
                selected = move_selection_in_group_order(&config, selected, -1);
                needs_redraw = 1;
                break;
            case KEY_DOWN:
                selected = move_selection_in_group_order(&config, selected, 1);
                needs_redraw = 1;
                break;
            case '\n':
            case KEY_ENTER:
                execute_item(&config.items[selected], &output);
                needs_redraw = 1;
                break;
            case 'q':
            case 'Q':
                running = 0;
                break;
            case 'c':
            case 'C': {
                if (has_colors()) {
                    theme_index = (theme_index + 1) % ui_theme_count();
                    apply_theme(theme_index);
                    config.theme_index = theme_index;
                    if (write_config_file(config_path, &config) != 0) {
                        append_output_line(&output, "Warning: failed to save theme preference.");
                    }
                    char line[128];
                    snprintf(line, sizeof(line), "Theme: %s", ui_theme_name(theme_index));
                    append_output_line(&output, line);
                } else {
                    append_output_line(&output, "Color themes unavailable in this terminal.");
                }
                needs_redraw = 1;
                break;
            }
            case 'a':
            case 'A':
                add_item_interactively(config_path, &config, &output, &selected);
                needs_redraw = 1;
                break;
            case 'e':
            case 'E':
                edit_item_interactively(config_path, &config, selected, &output);
                needs_redraw = 1;
                break;
            case 'd':
            case 'D':
                delete_item_interactively(config_path, &config, &selected, &output);
                needs_redraw = 1;
                break;
            case 'g':
            case 'G':
                manage_groups_interactively(config_path, &config, &selected, &output);
                needs_redraw = 1;
                break;
            case 'i':
            case 'I':
                show_system_info_popup();
                needs_redraw = 1;
                break;
            case KEY_RESIZE:
                /* Recompute pane sizes on terminal resize before redrawing. */
                setup_ui_windows(&ui);
                needs_redraw = 1;
                break;
            default:
                needs_redraw = 1;
                break;
        }

        if (running && needs_redraw) {
            draw_menu(&config, selected, &output, &ui);
        }
    }

    destroy_ui_windows(&ui);
    endwin();
    free_output_buffer(&output);
    free_menu_config(&config);
    return EXIT_SUCCESS;
}
