#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_ITEMS_CAPACITY 8
#define INITIAL_GROUPS_CAPACITY 8

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

/* Return index of the matching group name, or -1 if missing. */
int find_group_index(const MenuConfig *config, const char *group_name) {
    if (!config || !group_name) {
        return -1;
    }
    for (size_t i = 0; i < config->group_count; i++) {
        if (strcmp(config->groups[i], group_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Ensure a group exists in the config list. */
int add_group(MenuConfig *config, const char *group_name) {
    if (!config || !group_name || group_name[0] == '\0') {
        return -1;
    }
    if (find_group_index(config, group_name) >= 0) {
        return 0;
    }

    if (config->group_count == config->groups_capacity) {
        size_t new_capacity =
            config->groups_capacity == 0 ? INITIAL_GROUPS_CAPACITY : config->groups_capacity * 2;
        char **new_groups = (char **)realloc(config->groups, sizeof(char *) * new_capacity);
        if (!new_groups) {
            return -1;
        }
        config->groups = new_groups;
        config->groups_capacity = new_capacity;
    }

    char *group_copy = duplicate_string(group_name);
    if (!group_copy) {
        return -1;
    }
    config->groups[config->group_count++] = group_copy;
    return 0;
}

/* Append a new menu item to the in-memory menu configuration. */
int add_item(MenuConfig *config, const char *label, const char *command, int launch_detached,
             const char *group_name) {
    if (add_group(config, (group_name && group_name[0] != '\0') ? group_name
                                                                : DEFAULT_GROUP_NAME) != 0) {
        return -1;
    }

    if (config->count == config->capacity) {
        size_t new_capacity =
            config->capacity == 0 ? INITIAL_ITEMS_CAPACITY : config->capacity * 2;
        MenuItem *new_items = (MenuItem *)realloc(config->items, sizeof(MenuItem) * new_capacity);
        if (!new_items) {
            return -1;
        }
        config->items = new_items;
        config->capacity = new_capacity;
    }

    config->items[config->count].label = duplicate_string(label);
    config->items[config->count].command = duplicate_string(command);
    config->items[config->count].group =
        duplicate_string((group_name && group_name[0] != '\0') ? group_name : DEFAULT_GROUP_NAME);
    config->items[config->count].launch_detached = launch_detached;
    if (!config->items[config->count].label || !config->items[config->count].command ||
        !config->items[config->count].group) {
        free(config->items[config->count].label);
        free(config->items[config->count].command);
        free(config->items[config->count].group);
        return -1;
    }

    config->count++;
    return 0;
}

/* Free all menu entries and reset menu configuration storage. */
void free_menu_config(MenuConfig *config) {
    if (!config || !config->items) {
        if (config && config->groups) {
            for (size_t i = 0; i < config->group_count; i++) {
                free(config->groups[i]);
            }
            free(config->groups);
            config->groups = NULL;
            config->group_count = 0;
            config->groups_capacity = 0;
        }
        return;
    }

    for (size_t i = 0; i < config->count; i++) {
        free(config->items[i].label);
        free(config->items[i].command);
        free(config->items[i].group);
    }

    free(config->items);
    config->items = NULL;
    config->count = 0;
    config->capacity = 0;
    if (config->groups) {
        for (size_t i = 0; i < config->group_count; i++) {
            free(config->groups[i]);
        }
        free(config->groups);
    }
    config->groups = NULL;
    config->group_count = 0;
    config->groups_capacity = 0;
}

/* Skip whitespace while parsing JSON text. */
static void json_skip_whitespace(const char **p) {
    while (**p && isspace((unsigned char)**p)) {
        (*p)++;
    }
}

/* Parse one escaped JSON string into a newly allocated C string. */
static char *json_parse_string(const char **p) {
    json_skip_whitespace(p);
    if (**p != '"') {
        return NULL;
    }
    (*p)++;

    size_t capacity = 64;
    size_t len = 0;
    char *out = (char *)malloc(capacity);
    if (!out) {
        return NULL;
    }

    while (**p) {
        char ch = **p;
        (*p)++;
        if (ch == '"') {
            out[len] = '\0';
            return out;
        }

        if (ch == '\\') {
            char esc = **p;
            if (esc == '\0') {
                free(out);
                return NULL;
            }
            (*p)++;
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    ch = esc;
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u':
                    ch = '?';
                    for (int i = 0; i < 4; i++) {
                        if (!isxdigit((unsigned char)**p)) {
                            free(out);
                            return NULL;
                        }
                        (*p)++;
                    }
                    break;
                default:
                    free(out);
                    return NULL;
            }
        }

        if (len + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            char *resized = (char *)realloc(out, new_capacity);
            if (!resized) {
                free(out);
                return NULL;
            }
            out = resized;
            capacity = new_capacity;
        }
        out[len++] = ch;
    }

    free(out);
    return NULL;
}

/* Skip any JSON value (object/array/string/number/bool/null). */
static int json_skip_value(const char **p) {
    json_skip_whitespace(p);
    if (**p == '"') {
        char *tmp = json_parse_string(p);
        if (!tmp) {
            return -1;
        }
        free(tmp);
        return 0;
    }

    if (**p == '{') {
        (*p)++;
        json_skip_whitespace(p);
        if (**p == '}') {
            (*p)++;
            return 0;
        }
        while (**p) {
            char *key = json_parse_string(p);
            if (!key) {
                return -1;
            }
            free(key);
            json_skip_whitespace(p);
            if (**p != ':') {
                return -1;
            }
            (*p)++;
            if (json_skip_value(p) != 0) {
                return -1;
            }
            json_skip_whitespace(p);
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == '}') {
                (*p)++;
                return 0;
            }
            return -1;
        }
        return -1;
    }

    if (**p == '[') {
        (*p)++;
        json_skip_whitespace(p);
        if (**p == ']') {
            (*p)++;
            return 0;
        }
        while (**p) {
            if (json_skip_value(p) != 0) {
                return -1;
            }
            json_skip_whitespace(p);
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == ']') {
                (*p)++;
                return 0;
            }
            return -1;
        }
        return -1;
    }

    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        return 0;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        return 0;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return 0;
    }

    if (**p == '-' || isdigit((unsigned char)**p)) {
        (*p)++;
        while (**p && (isdigit((unsigned char)**p) || **p == '.' || **p == 'e' ||
                       **p == 'E' || **p == '+' || **p == '-')) {
            (*p)++;
        }
        return 0;
    }

    return -1;
}

/* Parse a JSON boolean or 0/1 numeric value into integer form. */
static int json_parse_boolish(const char **p, int *out_value) {
    json_skip_whitespace(p);
    if (strncmp(*p, "true", 4) == 0) {
        *out_value = 1;
        *p += 4;
        return 0;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *out_value = 0;
        *p += 5;
        return 0;
    }
    if (**p == '0') {
        *out_value = 0;
        (*p)++;
        return 0;
    }
    if (**p == '1') {
        *out_value = 1;
        (*p)++;
        return 0;
    }
    return -1;
}

/* Parse a JSON integer value. */
static int json_parse_int(const char **p, int *out_value) {
    json_skip_whitespace(p);
    if (!(**p == '-' || isdigit((unsigned char)**p))) {
        return -1;
    }

    char *end_ptr = NULL;
    long value = strtol(*p, &end_ptr, 10);
    if (end_ptr == *p) {
        return -1;
    }
    *out_value = (int)value;
    *p = end_ptr;
    return 0;
}

/* Parse one menu item object and append it to config. */
static int json_parse_menu_item(const char **p, MenuConfig *config) {
    json_skip_whitespace(p);
    if (**p != '{') {
        return -1;
    }
    (*p)++;

    char *label = NULL;
    char *command = NULL;
    int launch_detached = 0;
    char *group = NULL;

    json_skip_whitespace(p);
    if (**p == '}') {
        (*p)++;
    } else {
        while (**p) {
            char *key = json_parse_string(p);
            if (!key) {
                free(label);
                free(command);
                return -1;
            }

            json_skip_whitespace(p);
            if (**p != ':') {
                free(key);
                free(label);
                free(command);
                return -1;
            }
            (*p)++;

            if (strcmp(key, "label") == 0) {
                free(label);
                label = json_parse_string(p);
                if (!label) {
                    free(key);
                    free(command);
                    free(group);
                    return -1;
                }
            } else if (strcmp(key, "command") == 0) {
                free(command);
                command = json_parse_string(p);
                if (!command) {
                    free(key);
                    free(label);
                    free(group);
                    return -1;
                }
            } else if (strcmp(key, "group") == 0) {
                free(group);
                group = json_parse_string(p);
                if (!group) {
                    free(key);
                    free(label);
                    free(command);
                    return -1;
                }
            } else if (strcmp(key, "launch_detached") == 0) {
                if (json_parse_boolish(p, &launch_detached) != 0) {
                    free(key);
                    free(label);
                    free(command);
                    free(group);
                    return -1;
                }
            } else {
                if (json_skip_value(p) != 0) {
                    free(key);
                    free(label);
                    free(command);
                    free(group);
                    return -1;
                }
            }
            free(key);

            json_skip_whitespace(p);
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == '}') {
                (*p)++;
                break;
            }
            free(label);
            free(command);
            free(group);
            return -1;
        }
    }

    if (!label || !command || label[0] == '\0' || command[0] == '\0') {
        free(label);
        free(command);
        free(group);
        return -1;
    }

    int result =
        add_item(config, label, command, launch_detached, (group && group[0] != '\0') ? group
                                                                                       : DEFAULT_GROUP_NAME);
    free(label);
    free(command);
    free(group);
    return result;
}

/* Escape and write a JSON string literal value. */
static void json_write_escaped_string(FILE *f, const char *text) {
    fputc('"', f);
    while (*text) {
        unsigned char ch = (unsigned char)*text++;
        switch (ch) {
            case '\\':
                fputs("\\\\", f);
                break;
            case '"':
                fputs("\\\"", f);
                break;
            case '\n':
                fputs("\\n", f);
                break;
            case '\r':
                fputs("\\r", f);
                break;
            case '\t':
                fputs("\\t", f);
                break;
            case '\b':
                fputs("\\b", f);
                break;
            case '\f':
                fputs("\\f", f);
                break;
            default:
                if (ch < 0x20) {
                    fprintf(f, "\\u%04x", ch);
                } else {
                    fputc((int)ch, f);
                }
                break;
        }
    }
    fputc('"', f);
}

/* Parse JSON config file into an in-memory menu configuration. */
int parse_config_file(const char *path, MenuConfig *config) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("Failed to open config file");
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return -1;
    }
    size_t bytes_read = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[bytes_read] = '\0';

    const char *p = data;
    json_skip_whitespace(&p);
    if (*p != '{') {
        free(data);
        fprintf(stderr, "Invalid config: expected root object\n");
        return -1;
    }
    p++;

    int found_items = 0;
    int parsed_theme_index = 0;
    json_skip_whitespace(&p);
    if (*p == '}') {
        p++;
    } else {
        while (*p) {
            char *key = json_parse_string(&p);
            if (!key) {
                free(data);
                fprintf(stderr, "Invalid config: expected object key\n");
                return -1;
            }

            json_skip_whitespace(&p);
            if (*p != ':') {
                free(key);
                free(data);
                fprintf(stderr, "Invalid config: expected ':' after key\n");
                return -1;
            }
            p++;

            if (strcmp(key, "groups") == 0) {
                json_skip_whitespace(&p);
                if (*p != '[') {
                    free(key);
                    free(data);
                    fprintf(stderr, "Invalid config: groups must be an array\n");
                    return -1;
                }
                p++;
                json_skip_whitespace(&p);
                if (*p == ']') {
                    p++;
                } else {
                    while (*p) {
                        char *group_name = json_parse_string(&p);
                        if (!group_name || group_name[0] == '\0' ||
                            add_group(config, group_name) != 0) {
                            free(group_name);
                            free(key);
                            free(data);
                            fprintf(stderr, "Invalid config: malformed group\n");
                            return -1;
                        }
                        free(group_name);
                        json_skip_whitespace(&p);
                        if (*p == ',') {
                            p++;
                            continue;
                        }
                        if (*p == ']') {
                            p++;
                            break;
                        }
                        free(key);
                        free(data);
                        fprintf(stderr, "Invalid config: expected ',' or ']'\n");
                        return -1;
                    }
                }
            } else if (strcmp(key, "theme_index") == 0) {
                if (json_parse_int(&p, &parsed_theme_index) != 0) {
                    free(key);
                    free(data);
                    fprintf(stderr, "Invalid config: theme_index must be an integer\n");
                    return -1;
                }
            } else if (strcmp(key, "items") == 0) {
                found_items = 1;
                json_skip_whitespace(&p);
                if (*p != '[') {
                    free(key);
                    free(data);
                    fprintf(stderr, "Invalid config: items must be an array\n");
                    return -1;
                }
                p++;
                json_skip_whitespace(&p);
                if (*p == ']') {
                    p++;
                } else {
                    while (*p) {
                        if (json_parse_menu_item(&p, config) != 0) {
                            free(key);
                            free(data);
                            fprintf(stderr, "Invalid config: malformed menu item\n");
                            return -1;
                        }
                        json_skip_whitespace(&p);
                        if (*p == ',') {
                            p++;
                            continue;
                        }
                        if (*p == ']') {
                            p++;
                            break;
                        }
                        free(key);
                        free(data);
                        fprintf(stderr, "Invalid config: expected ',' or ']'\n");
                        return -1;
                    }
                }
            } else {
                if (json_skip_value(&p) != 0) {
                    free(key);
                    free(data);
                    fprintf(stderr, "Invalid config: malformed value\n");
                    return -1;
                }
            }
            free(key);

            json_skip_whitespace(&p);
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == '}') {
                p++;
                break;
            }
            free(data);
            fprintf(stderr, "Invalid config: expected ',' or '}'\n");
            return -1;
        }
    }

    json_skip_whitespace(&p);
    if (*p != '\0') {
        free(data);
        fprintf(stderr, "Invalid config: unexpected trailing data\n");
        return -1;
    }
    if (!found_items) {
        free(data);
        fprintf(stderr, "Invalid config: missing items array\n");
        return -1;
    }
    if (add_group(config, DEFAULT_GROUP_NAME) != 0) {
        free(data);
        fprintf(stderr, "Invalid config: failed to ensure default group\n");
        return -1;
    }
    config->theme_index = parsed_theme_index;

    free(data);
    return 0;
}

/* Persist the current in-memory menu configuration to disk. */
int write_config_file(const char *path, const MenuConfig *config) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }

    fprintf(f, "{\n  \"theme_index\": %d,\n  \"groups\": [\n", config->theme_index);
    for (size_t i = 0; i < config->group_count; i++) {
        fprintf(f, "    ");
        json_write_escaped_string(f, config->groups[i]);
        fprintf(f, "%s\n", (i + 1 < config->group_count) ? "," : "");
    }
    fprintf(f, "  ],\n  \"items\": [\n");
    for (size_t i = 0; i < config->count; i++) {
        fprintf(f, "    {\n      \"label\": ");
        json_write_escaped_string(f, config->items[i].label);
        fprintf(f, ",\n      \"command\": ");
        json_write_escaped_string(f, config->items[i].command);
        fprintf(f, ",\n      \"group\": ");
        json_write_escaped_string(
            f, config->items[i].group ? config->items[i].group : DEFAULT_GROUP_NAME);
        fprintf(f, ",\n      \"launch_detached\": %s\n    }%s\n",
                config->items[i].launch_detached ? "true" : "false",
                (i + 1 < config->count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

/* Count how many items currently belong to the given group name. */
int count_items_in_group(const MenuConfig *config, const char *group_name) {
    if (!config || !group_name) {
        return 0;
    }
    int count = 0;
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->items[i].group, group_name) == 0) {
            count++;
        }
    }
    return count;
}
