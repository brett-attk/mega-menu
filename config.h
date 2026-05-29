#ifndef MEGAMENU_CONFIG_H
#define MEGAMENU_CONFIG_H

#include <stddef.h>

#define DEFAULT_GROUP_NAME "Ungrouped"

typedef struct {
    char *label;
    char *command;
    char *group;
    int launch_detached;
} MenuItem;

typedef struct {
    MenuItem *items;
    size_t count;
    size_t capacity;
    char **groups;
    size_t group_count;
    size_t groups_capacity;
    int theme_index;
} MenuConfig;

void free_menu_config(MenuConfig *config);
int find_group_index(const MenuConfig *config, const char *group_name);
int add_group(MenuConfig *config, const char *group_name);
int add_item(MenuConfig *config, const char *label, const char *command, int launch_detached,
             const char *group_name);
int parse_config_file(const char *path, MenuConfig *config);
int write_config_file(const char *path, const MenuConfig *config);
int count_items_in_group(const MenuConfig *config, const char *group_name);

#endif
