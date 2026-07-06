/* myPanel.c
 * Small translucent Xfce4/X11 state panel.
 *
 * Build:
 *   gcc -O2 -pipe myPanel.c -o myPanel $(pkg-config --cflags --libs gtk+-3.0)
 *
 * Run:
 *   ./myPanel
 */

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WINDOW_X 0
#define WINDOW_Y 0
#define ICON_SLOTS 4
#define WEATHER_INTERVAL_SECONDS 600

typedef struct {
    GtkWidget *window;
    GtkWidget *root_box;
    GtkWidget *menu_button;
    GtkWidget *details_box;
    GtkWidget *info_label;
    GtkWidget *weather_icon;
    GtkWidget *weather_label;
    GtkWidget *icon_buttons[ICON_SLOTS];
    GtkWidget *icon_images[ICON_SLOTS];
    GtkWidget *weather_reset_button;
    GtkWidget *weather_reset_image;

    char config_path[PATH_MAX];
    char time_line[64];
    char uptime_line[64];
    char battery_line[128];
    char cpu_line[128];
    char net_line[64];
    char net_host[64];
    gboolean net_inflight;
    char battery_dir[PATH_MAX];
    gboolean has_battery;

    char weather_line[160];
    char weather_icon_name[64];
    char weather_place[96];
    char weather_url[1024];
    gboolean weather_inflight;

    char icon_names[ICON_SLOTS][PATH_MAX];
    char icon_commands[ICON_SLOTS][PATH_MAX];
    char icon_tooltips[ICON_SLOTS][128];

    gboolean expanded;
    guint shape_update_id;
} AppState;

static GtkWidget *make_label(const char *name, const char *text);
static GtkWidget *make_entry(const char *name, const char *placeholder,
                             const char *value);

static gboolean read_first_line(const char *path, char *buf, size_t len) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return FALSE;

    if (!fgets(buf, (int)len, fp)) {
        fclose(fp);
        return FALSE;
    }

    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
    return TRUE;
}

static void build_config_path(AppState *st) {
    const char *config_home = g_get_user_config_dir();

    g_snprintf(st->config_path, sizeof(st->config_path),
               "%s/myPanel/panel.ini", config_home);
}

static void set_default_config(AppState *st) {
    st->weather_place[0] = '\0';
    st->weather_url[0] = '\0';
    g_strlcpy(st->weather_line, "weather: set Open-Meteo or OpenWeather URL",
              sizeof(st->weather_line));
    g_strlcpy(st->weather_icon_name, "weather-clear",
              sizeof(st->weather_icon_name));

    g_strlcpy(st->icon_names[0], "utilities-terminal",
              sizeof(st->icon_names[0]));
    g_strlcpy(st->icon_commands[0], "exo-open --launch TerminalEmulator",
              sizeof(st->icon_commands[0]));
    g_strlcpy(st->icon_tooltips[0], "Terminal", sizeof(st->icon_tooltips[0]));

    g_strlcpy(st->icon_names[1], "web-browser", sizeof(st->icon_names[1]));
    g_strlcpy(st->icon_commands[1], "exo-open --launch WebBrowser",
              sizeof(st->icon_commands[1]));
    g_strlcpy(st->icon_tooltips[1], "Browser", sizeof(st->icon_tooltips[1]));

    g_strlcpy(st->icon_names[2], "system-file-manager",
              sizeof(st->icon_names[2]));
    g_strlcpy(st->icon_commands[2], "exo-open --launch FileManager",
              sizeof(st->icon_commands[2]));
    g_strlcpy(st->icon_tooltips[2], "Files", sizeof(st->icon_tooltips[2]));

    g_strlcpy(st->icon_names[3], "preferences-system",
              sizeof(st->icon_names[3]));
    g_strlcpy(st->icon_commands[3], "xfce4-settings-manager",
              sizeof(st->icon_commands[3]));
    g_strlcpy(st->icon_tooltips[3], "Settings", sizeof(st->icon_tooltips[3]));
}

static gboolean ensure_config_dir(AppState *st) {
    gchar *dir = g_path_get_dirname(st->config_path);
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0;

    if (!ok) {
        g_printerr("myPanel: could not create %s: %s\n",
                   dir, g_strerror(errno));
    }

    g_free(dir);
    return ok;
}

static void save_config(AppState *st) {
    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;

    ensure_config_dir(st);

    g_key_file_set_string(kf, "weather", "place", st->weather_place);
    g_key_file_set_string(kf, "weather", "url", st->weather_url);

    for (int i = 0; i < ICON_SLOTS; i++) {
        char key[32];

        g_snprintf(key, sizeof(key), "%d_name", i + 1);
        g_key_file_set_string(kf, "icons", key, st->icon_names[i]);

        g_snprintf(key, sizeof(key), "%d_command", i + 1);
        g_key_file_set_string(kf, "icons", key, st->icon_commands[i]);

        g_snprintf(key, sizeof(key), "%d_tooltip", i + 1);
        g_key_file_set_string(kf, "icons", key, st->icon_tooltips[i]);
    }

    if (!g_key_file_save_to_file(kf, st->config_path, &error)) {
        g_printerr("myPanel: could not save %s: %s\n",
                   st->config_path, error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    g_key_file_unref(kf);
}

static void load_config(AppState *st) {
    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;

    build_config_path(st);
    set_default_config(st);

    if (!g_key_file_load_from_file(kf, st->config_path, G_KEY_FILE_NONE,
                                   &error)) {
        g_clear_error(&error);
        save_config(st);
        g_key_file_unref(kf);
        return;
    }

    gchar *place = g_key_file_get_string(kf, "weather", "place", NULL);
    gchar *url = g_key_file_get_string(kf, "weather", "url", NULL);

    if (place) {
        g_strlcpy(st->weather_place, place, sizeof(st->weather_place));
        g_free(place);
    }

    if (url) {
        g_strlcpy(st->weather_url, url, sizeof(st->weather_url));
        g_free(url);
    }

    for (int i = 0; i < ICON_SLOTS; i++) {
        char key[32];
        gchar *value;

        g_snprintf(key, sizeof(key), "%d_name", i + 1);
        value = g_key_file_get_string(kf, "icons", key, NULL);
        if (value) {
            g_strlcpy(st->icon_names[i], value, sizeof(st->icon_names[i]));
            g_free(value);
        }

        g_snprintf(key, sizeof(key), "%d_command", i + 1);
        value = g_key_file_get_string(kf, "icons", key, NULL);
        if (value) {
            g_strlcpy(st->icon_commands[i], value,
                      sizeof(st->icon_commands[i]));
            g_free(value);
        }

        g_snprintf(key, sizeof(key), "%d_tooltip", i + 1);
        value = g_key_file_get_string(kf, "icons", key, NULL);
        if (value) {
            g_strlcpy(st->icon_tooltips[i], value,
                      sizeof(st->icon_tooltips[i]));
            g_free(value);
        }
    }

    g_key_file_unref(kf);
}

static gboolean detect_battery(AppState *st) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return FALSE;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char type_path[PATH_MAX];
        char present_path[PATH_MAX];
        char type[64];
        char present[16];

        snprintf(type_path, sizeof(type_path),
                 "/sys/class/power_supply/%s/type", ent->d_name);

        if (!read_first_line(type_path, type, sizeof(type)))
            continue;

        if (strcmp(type, "Battery") != 0)
            continue;

        snprintf(present_path, sizeof(present_path),
                 "/sys/class/power_supply/%s/present", ent->d_name);

        if (read_first_line(present_path, present, sizeof(present))) {
            if (strcmp(present, "1") != 0)
                continue;
        }

        snprintf(st->battery_dir, sizeof(st->battery_dir),
                 "/sys/class/power_supply/%s", ent->d_name);

        st->has_battery = TRUE;
        closedir(d);
        return TRUE;
    }

    closedir(d);
    st->has_battery = FALSE;
    st->battery_dir[0] = '\0';
    return FALSE;
}

static void format_current_time(AppState *st) {
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(st->time_line, sizeof(st->time_line),
             "time: %Y-%m-%d %H:%M:%S", &tm_now);
}

static void format_uptime(AppState *st) {
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) {
        g_snprintf(st->uptime_line, sizeof(st->uptime_line), "uptime: N/A");
        return;
    }

    double up = 0.0;
    if (fscanf(fp, "%lf", &up) != 1) {
        fclose(fp);
        g_snprintf(st->uptime_line, sizeof(st->uptime_line), "uptime: N/A");
        return;
    }
    fclose(fp);

    long sec = (long)up;
    long days = sec / 86400;
    long hours = (sec % 86400) / 3600;
    long mins = (sec % 3600) / 60;

    if (days > 0) {
        g_snprintf(st->uptime_line, sizeof(st->uptime_line),
                   "uptime: %ldd %02ldh %02ldm", days, hours, mins);
    } else {
        g_snprintf(st->uptime_line, sizeof(st->uptime_line),
                   "uptime: %02ldh %02ldm", hours, mins);
    }
}

static void format_battery(AppState *st) {
    if (!st->has_battery) {
        st->battery_line[0] = '\0';
        return;
    }

    char cap_path[PATH_MAX];
    char stat_path[PATH_MAX];
    char capacity[32];
    char status[64];

    snprintf(cap_path, sizeof(cap_path), "%s/capacity", st->battery_dir);
    snprintf(stat_path, sizeof(stat_path), "%s/status", st->battery_dir);

    if (!read_first_line(cap_path, capacity, sizeof(capacity))) {
        g_snprintf(st->battery_line, sizeof(st->battery_line), "battery: N/A");
        return;
    }

    if (!read_first_line(stat_path, status, sizeof(status)))
        g_strlcpy(status, "Unknown", sizeof(status));

    g_snprintf(st->battery_line, sizeof(st->battery_line),
               "battery: %s%% (%s)", capacity, status);
}

static void refresh_info_label(AppState *st) {
    if (!st->info_label)
        return;

    if (st->has_battery && st->battery_line[0] != '\0') {
        char text[512];
        g_snprintf(text, sizeof(text), "%s\n%s\n%s\n%s\n%s",
                   st->time_line,
                   st->cpu_line,
                   st->net_line,
                   st->uptime_line,
                   st->battery_line);
        gtk_label_set_text(GTK_LABEL(st->info_label), text);
    } else {
        char text[384];
        g_snprintf(text, sizeof(text), "%s\n%s\n%s\n%s",
                   st->time_line,
                   st->cpu_line,
                   st->net_line,
                   st->uptime_line);
        gtk_label_set_text(GTK_LABEL(st->info_label), text);
    }
}

static void format_cpu(AppState *st) {
    char raw[128];

    if (!read_first_line("/proc/loadavg", raw, sizeof(raw))) {
        g_snprintf(st->cpu_line, sizeof(st->cpu_line), "cpu: N/A");
        return;
    }

    g_snprintf(st->cpu_line, sizeof(st->cpu_line), "cpu: %s", raw);
}

static void update_cpu(AppState *st) {
    format_cpu(st);
}

static void format_net(AppState *st, const char *stdout_buf) {
    const char *p;

    if (!stdout_buf) {
        g_snprintf(st->net_line, sizeof(st->net_line), "net: parse fail");
        return;
    }

    p = strstr(stdout_buf, "time=");
    if (!p) {
        g_snprintf(st->net_line, sizeof(st->net_line), "net: parse fail");
        return;
    }

    p += 5;

    char *end = NULL;
    double ms = g_ascii_strtod(p, &end);
    if (end == p) {
        g_snprintf(st->net_line, sizeof(st->net_line), "net: parse fail");
        return;
    }

    g_snprintf(st->net_line, sizeof(st->net_line), "net: %.1f ms", ms);
}

static void update_net_done(GObject *source, GAsyncResult *res,
                            gpointer user_data) {
    AppState *st = (AppState *)user_data;
    GSubprocess *proc = G_SUBPROCESS(source);
    GError *error = NULL;
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;

    st->net_inflight = FALSE;

    if (!g_subprocess_communicate_utf8_finish(proc, res,
                                              &stdout_buf, &stderr_buf,
                                              &error)) {
        g_snprintf(st->net_line, sizeof(st->net_line), "net: I/O fail");
        g_clear_error(&error);
        g_free(stdout_buf);
        g_free(stderr_buf);
        refresh_info_label(st);
        return;
    }

    if (!g_subprocess_get_successful(proc)) {
        g_snprintf(st->net_line, sizeof(st->net_line), "net: timeout");
        g_free(stdout_buf);
        g_free(stderr_buf);
        refresh_info_label(st);
        return;
    }

    format_net(st, stdout_buf);

    g_free(stdout_buf);
    g_free(stderr_buf);
    refresh_info_label(st);
}

static gboolean update_net(gpointer data) {
    AppState *st = (AppState *)data;
    GError *error = NULL;

    if (st->net_inflight)
        return G_SOURCE_CONTINUE;

    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                  G_SUBPROCESS_FLAGS_STDERR_PIPE);

    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);

    const gchar *argv[] = {
        "ping", "-n", "-c", "1", "-W", "1", st->net_host, NULL
    };

    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher);

    if (!proc) {
        g_snprintf(st->net_line, sizeof(st->net_line), "net: spawn fail");
        g_clear_error(&error);
        refresh_info_label(st);
        return G_SOURCE_CONTINUE;
    }

    st->net_inflight = TRUE;

    g_subprocess_communicate_utf8_async(proc, NULL, NULL, update_net_done, st);

    g_object_unref(proc);
    return G_SOURCE_CONTINUE;
}

static gboolean find_json_number(const char *json, const char *key,
                                 double *out) {
    const char *p = json;
    size_t key_len = strlen(key);

    while ((p = strstr(p, key)) != NULL) {
        const char *colon = strchr(p, ':');
        if (!colon)
            return FALSE;

        colon++;
        while (*colon == ' ' || *colon == '\t')
            colon++;

        char *end = NULL;
        double value = g_ascii_strtod(colon, &end);
        if (end != colon) {
            *out = value;
            return TRUE;
        }

        p += key_len;
    }

    return FALSE;
}

static gboolean find_json_string(const char *json, const char *key,
                                 char *out, size_t len) {
    const char *p = strstr(json, key);
    if (!p)
        return FALSE;

    p = strchr(p, ':');
    if (!p)
        return FALSE;

    p++;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p != '"')
        return FALSE;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < len) {
        if (*p == '\\' && p[1] != '\0')
            p++;
        out[i++] = *p++;
    }
    out[i] = '\0';

    return i > 0;
}

static const char *weather_icon_for_code(int code, gboolean openweather_id) {
    if (openweather_id) {
        if (code >= 200 && code < 300)
            return "weather-storm";
        if (code >= 300 && code < 600)
            return "weather-showers";
        if (code >= 600 && code < 700)
            return "weather-snow";
        if (code >= 700 && code < 800)
            return "weather-fog";
        if (code == 800)
            return "weather-clear";
        if (code == 801)
            return "weather-few-clouds";
        return "weather-overcast";
    }

    if (code == 0)
        return "weather-clear";
    if (code == 1 || code == 2)
        return "weather-few-clouds";
    if (code == 3)
        return "weather-overcast";
    if (code == 45 || code == 48)
        return "weather-fog";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
        return "weather-showers";
    if (code >= 71 && code <= 77)
        return "weather-snow";
    if (code >= 95)
        return "weather-storm";
    return "weather-clear";
}

static void refresh_weather_widgets(AppState *st) {
    if (st->weather_label)
        gtk_label_set_text(GTK_LABEL(st->weather_label), st->weather_line);

    if (st->weather_icon) {
        gtk_image_set_from_icon_name(GTK_IMAGE(st->weather_icon),
                                     st->weather_icon_name,
                                     GTK_ICON_SIZE_DND);
    }
}

static void parse_weather_response(AppState *st, const char *json) {
    double temp = 0.0;
    double code_value = 0.0;
    char desc[80] = "";
    gboolean has_temp;
    gboolean has_code = FALSE;
    gboolean openweather_code = FALSE;

    has_temp = find_json_number(json, "\"temperature_2m\"", &temp);
    if (find_json_number(json, "\"weather_code\"", &code_value) ||
        find_json_number(json, "\"weathercode\"", &code_value)) {
        has_code = TRUE;
    }

    if (!has_temp) {
        has_temp = find_json_number(json, "\"temp\"", &temp);
        if (find_json_number(json, "\"id\"", &code_value)) {
            has_code = TRUE;
            openweather_code = TRUE;
        }
    }

    find_json_string(json, "\"description\"", desc, sizeof(desc));

    if (!has_temp) {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: connected, parse fail");
        g_strlcpy(st->weather_icon_name, "weather-severe-alert",
                  sizeof(st->weather_icon_name));
        return;
    }

    if (has_code) {
        int code = (int)(code_value >= 0.0 ? code_value + 0.5
                                           : code_value - 0.5);
        g_strlcpy(st->weather_icon_name,
                  weather_icon_for_code(code, openweather_code),
                  sizeof(st->weather_icon_name));
    } else {
        g_strlcpy(st->weather_icon_name, "weather-clear",
                  sizeof(st->weather_icon_name));
    }

    if (desc[0] != '\0') {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: %s %.1f C, %s",
                   st->weather_place[0] ? st->weather_place : "local",
                   temp, desc);
    } else {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: %s %.1f C",
                   st->weather_place[0] ? st->weather_place : "local",
                   temp);
    }
}

static void update_weather_done(GObject *source, GAsyncResult *res,
                                gpointer user_data) {
    AppState *st = (AppState *)user_data;
    GSubprocess *proc = G_SUBPROCESS(source);
    GError *error = NULL;
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;

    st->weather_inflight = FALSE;

    if (!g_subprocess_communicate_utf8_finish(proc, res,
                                              &stdout_buf, &stderr_buf,
                                              &error)) {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: network I/O fail");
        g_strlcpy(st->weather_icon_name, "weather-severe-alert",
                  sizeof(st->weather_icon_name));
        g_clear_error(&error);
        g_free(stdout_buf);
        g_free(stderr_buf);
        refresh_weather_widgets(st);
        return;
    }

    if (!g_subprocess_get_successful(proc)) {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: API timeout or HTTP error");
        g_strlcpy(st->weather_icon_name, "weather-severe-alert",
                  sizeof(st->weather_icon_name));
        g_free(stdout_buf);
        g_free(stderr_buf);
        refresh_weather_widgets(st);
        return;
    }

    parse_weather_response(st, stdout_buf);
    g_free(stdout_buf);
    g_free(stderr_buf);
    refresh_weather_widgets(st);
}

static gboolean update_weather(gpointer data) {
    AppState *st = (AppState *)data;
    GError *error = NULL;

    if (st->weather_inflight)
        return G_SOURCE_CONTINUE;

    if (st->weather_url[0] == '\0') {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: paste Open-Meteo or OpenWeather URL");
        g_strlcpy(st->weather_icon_name, "weather-clear",
                  sizeof(st->weather_icon_name));
        refresh_weather_widgets(st);
        return G_SOURCE_CONTINUE;
    }

    gchar *curl = g_find_program_in_path("curl");
    if (!curl) {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: install curl");
        g_strlcpy(st->weather_icon_name, "weather-severe-alert",
                  sizeof(st->weather_icon_name));
        refresh_weather_widgets(st);
        return G_SOURCE_CONTINUE;
    }

    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                  G_SUBPROCESS_FLAGS_STDERR_PIPE);

    const gchar *argv[] = {
        curl, "-fsSL", "--max-time", "5", st->weather_url, NULL
    };

    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher);
    g_free(curl);

    if (!proc) {
        g_snprintf(st->weather_line, sizeof(st->weather_line),
                   "weather: curl spawn fail");
        g_strlcpy(st->weather_icon_name, "weather-severe-alert",
                  sizeof(st->weather_icon_name));
        g_clear_error(&error);
        refresh_weather_widgets(st);
        return G_SOURCE_CONTINUE;
    }

    st->weather_inflight = TRUE;
    g_subprocess_communicate_utf8_async(proc, NULL, NULL,
                                        update_weather_done, st);
    g_object_unref(proc);
    return G_SOURCE_CONTINUE;
}

static gboolean update_time_cb(gpointer data) {
    AppState *st = (AppState *)data;
    format_current_time(st);
    refresh_info_label(st);
    return G_SOURCE_CONTINUE;
}

static gboolean update_slow_cb(gpointer data) {
    AppState *st = (AppState *)data;
    format_uptime(st);
    format_battery(st);
    update_cpu(st);
    refresh_info_label(st);
    return G_SOURCE_CONTINUE;
}

static void add_widget_region(GtkWidget *root, GtkWidget *widget,
                              cairo_region_t *region, int pad) {
    if (!widget || !gtk_widget_get_visible(widget) ||
        !gtk_widget_get_mapped(widget) || !gtk_widget_get_sensitive(widget))
        return;

    int x = 0;
    int y = 0;
    GtkAllocation alloc;

    if (!gtk_widget_translate_coordinates(widget, root, 0, 0, &x, &y))
        return;

    gtk_widget_get_allocation(widget, &alloc);

    cairo_rectangle_int_t rect;
    rect.x = x - pad;
    rect.y = y - pad;
    rect.width = alloc.width + (pad * 2);
    rect.height = alloc.height + (pad * 2);
    cairo_region_union_rectangle(region, &rect);
}

static gboolean apply_input_region_cb(gpointer data) {
    AppState *st = (AppState *)data;
    cairo_region_t *region;

    st->shape_update_id = 0;

    if (!gtk_widget_get_realized(st->window))
        return G_SOURCE_REMOVE;

    region = cairo_region_create();
    add_widget_region(st->window, st->menu_button, region, 4);

    if (st->expanded) {
        for (int i = 0; i < ICON_SLOTS; i++)
            add_widget_region(st->window, st->icon_buttons[i], region, 2);

        add_widget_region(st->window, st->weather_reset_button, region, 2);
    }

    gtk_widget_input_shape_combine_region(st->window, region);
    cairo_region_destroy(region);
    return G_SOURCE_REMOVE;
}

static void queue_input_region_update(AppState *st) {
    if (st->shape_update_id)
        return;

    st->shape_update_id = g_idle_add(apply_input_region_cb, st);
}

static void on_size_allocate(GtkWidget *widget, GdkRectangle *allocation,
                             gpointer user_data) {
    (void)widget;
    (void)allocation;
    queue_input_region_update((AppState *)user_data);
}

static void set_expanded(AppState *st, gboolean expanded) {
    st->expanded = expanded;

    if (expanded) {
        gtk_widget_set_no_show_all(st->details_box, FALSE);
        gtk_widget_show_all(st->details_box);
        gtk_button_set_label(GTK_BUTTON(st->menu_button), "close");
    } else {
        gtk_widget_hide(st->details_box);
        gtk_widget_set_no_show_all(st->details_box, TRUE);
        gtk_button_set_label(GTK_BUTTON(st->menu_button), "menu");
    }

    gtk_window_resize(GTK_WINDOW(st->window), 1, 1);
    queue_input_region_update(st);
}

static void on_menu_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *st = (AppState *)user_data;
    set_expanded(st, !st->expanded);
}

static void set_weather_config(AppState *st, const char *place,
                               const char *url) {
    g_strlcpy(st->weather_place, place, sizeof(st->weather_place));
    g_strlcpy(st->weather_url, url, sizeof(st->weather_url));

    save_config(st);
    update_weather(st);
}

static void on_weather_reset_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *st = (AppState *)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Weather API setup",
        GTK_WINDOW(st->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *place_label =
        make_label("section-label", "city or town label");
    GtkWidget *url_label =
        make_label("section-label", "Open-Meteo or OpenWeather API URL");
    GtkWidget *place_entry =
        make_entry("glass-entry", "city or town label", st->weather_place);
    GtkWidget *url_entry =
        make_entry("glass-entry", "paste HTTPS weather API URL",
                   st->weather_url);

    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_entry_set_activates_default(GTK_ENTRY(place_entry), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(url_entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    gtk_box_pack_start(GTK_BOX(box), place_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), place_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), url_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), url_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);

    gtk_widget_set_size_request(url_entry, 420, -1);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        set_weather_config(st,
                           gtk_entry_get_text(GTK_ENTRY(place_entry)),
                           gtk_entry_get_text(GTK_ENTRY(url_entry)));
    }

    gtk_widget_destroy(dialog);
}

static void on_icon_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *st = NULL;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "slot"));

    st = (AppState *)user_data;
    if (slot < 0 || slot >= ICON_SLOTS)
        return;

    if (st->icon_commands[slot][0] == '\0')
        return;

    GError *error = NULL;
    if (!g_spawn_command_line_async(st->icon_commands[slot], &error)) {
        g_printerr("myPanel: icon command failed: %s\n",
                   error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static gboolean icon_name_looks_like_path(const char *name) {
    return name[0] == '/' || g_str_has_prefix(name, "~/") ||
           strchr(name, '/') != NULL;
}

static gchar *expand_tilde_path(const char *path) {
    if (!g_str_has_prefix(path, "~/"))
        return g_strdup(path);

    return g_build_filename(g_get_home_dir(), path + 2, NULL);
}

static void refresh_icon_buttons(AppState *st) {
    for (int i = 0; i < ICON_SLOTS; i++) {
        if (icon_name_looks_like_path(st->icon_names[i])) {
            gchar *path = expand_tilde_path(st->icon_names[i]);
            gtk_image_set_from_file(GTK_IMAGE(st->icon_images[i]), path);
            g_free(path);
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(st->icon_images[i]),
                                         st->icon_names[i],
                                         GTK_ICON_SIZE_LARGE_TOOLBAR);
        }

        gtk_widget_set_tooltip_text(st->icon_buttons[i],
                                    st->icon_tooltips[i][0]
                                        ? st->icon_tooltips[i]
                                        : st->icon_commands[i]);
        gtk_widget_set_sensitive(st->icon_buttons[i],
                                 st->icon_commands[i][0] != '\0');
    }
}

static GtkWidget *make_label(const char *name, const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_name(label, name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
    return label;
}

static GtkWidget *make_entry(const char *name, const char *placeholder,
                             const char *value) {
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_name(entry, name);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_tooltip_text(entry, placeholder);
    gtk_entry_set_text(GTK_ENTRY(entry), value ? value : "");
    return entry;
}

static void apply_css(void) {
    static const char *css =
        "window, #rootbox {"
        "  background-color: transparent;"
        "}"
        "#rootbox {"
        "  padding: 6px;"
        "}"
        "#menu-button {"
        "  min-width: 66px;"
        "  min-height: 28px;"
        "  padding: 2px 12px;"
        "  border-radius: 999px;"
        "  border: 1px solid rgba(255,255,255,0.72);"
        "  color: #ffffff;"
        "  font: 700 11px Sans;"
        "  background-image: linear-gradient(to bottom,"
        "    rgba(255,255,255,0.42), rgba(86,143,200,0.30) 42%,"
        "    rgba(24,44,64,0.54));"
        "  box-shadow: inset 0 1px rgba(255,255,255,0.62),"
        "    0 2px 12px rgba(0,0,0,0.36);"
        "}"
        "#glass-panel {"
        "  margin-top: 6px;"
        "  padding: 10px;"
        "  border-radius: 8px;"
        "  border: 1px solid rgba(255,255,255,0.44);"
        "  background-image: linear-gradient(to bottom,"
        "    rgba(255,255,255,0.20), rgba(38,55,70,0.42) 46%,"
        "    rgba(10,16,24,0.54));"
        "  box-shadow: inset 0 1px rgba(255,255,255,0.36),"
        "    0 8px 30px rgba(0,0,0,0.34);"
        "}"
        "#info-label {"
        "  color: #f2f6f8;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  text-shadow: 0 1px rgba(0,0,0,0.72);"
        "}"
        "#section-label {"
        "  color: rgba(255,255,255,0.86);"
        "  font: 700 10px Sans;"
        "}"
        "#weather-label {"
        "  color: #ffffff;"
        "  font: 11px Sans;"
        "  text-shadow: 0 1px rgba(0,0,0,0.70);"
        "}"
        "#glass-entry {"
        "  min-height: 22px;"
        "  border-radius: 6px;"
        "  border: 1px solid rgba(255,255,255,0.34);"
        "  color: #ffffff;"
        "  background-color: rgba(4,8,12,0.34);"
        "  caret-color: #ffffff;"
        "  padding: 2px 7px;"
        "  font: 10px Sans;"
        "}"
        "#small-glass-button {"
        "  min-height: 24px;"
        "  border-radius: 6px;"
        "  border: 1px solid rgba(255,255,255,0.42);"
        "  color: #ffffff;"
        "  background-color: rgba(72,114,150,0.38);"
        "  font: 700 10px Sans;"
        "}"
        "#icon-button {"
        "  min-width: 34px;"
        "  min-height: 34px;"
        "  border-radius: 8px;"
        "  border: 1px solid rgba(255,255,255,0.38);"
        "  background-color: rgba(255,255,255,0.12);"
        "  box-shadow: inset 0 1px rgba(255,255,255,0.26);"
        "}";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(provider);
}

static GtkWidget *build_weather_box(AppState *st) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_box_pack_start(GTK_BOX(outer), make_label("section-label", "weather api"),
                       FALSE, FALSE, 0);

    st->weather_icon = gtk_image_new_from_icon_name(st->weather_icon_name,
                                                    GTK_ICON_SIZE_DND);
    st->weather_label = make_label("weather-label", st->weather_line);
    gtk_label_set_ellipsize(GTK_LABEL(st->weather_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(st->weather_label, 260, -1);

    gtk_box_pack_start(GTK_BOX(top), st->weather_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), st->weather_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), top, FALSE, FALSE, 0);

    return outer;
}

static GtkWidget *build_icon_box(AppState *st) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_box_pack_start(GTK_BOX(outer), make_label("section-label", "shortcut icons"),
                       FALSE, FALSE, 0);

    for (int i = 0; i < ICON_SLOTS; i++) {
        st->icon_images[i] = gtk_image_new();
        st->icon_buttons[i] = gtk_button_new();
        gtk_widget_set_name(st->icon_buttons[i], "icon-button");
        gtk_button_set_image(GTK_BUTTON(st->icon_buttons[i]),
                             st->icon_images[i]);
        gtk_button_set_always_show_image(GTK_BUTTON(st->icon_buttons[i]), TRUE);
        g_object_set_data(G_OBJECT(st->icon_buttons[i]), "slot",
                          GINT_TO_POINTER(i));
        g_signal_connect(st->icon_buttons[i], "clicked",
                         G_CALLBACK(on_icon_clicked), st);
        gtk_box_pack_start(GTK_BOX(row), st->icon_buttons[i],
                           FALSE, FALSE, 0);
    }

    st->weather_reset_image =
        gtk_image_new_from_icon_name("weather-clear",
                                     GTK_ICON_SIZE_LARGE_TOOLBAR);
    st->weather_reset_button = gtk_button_new();
    gtk_widget_set_name(st->weather_reset_button, "icon-button");
    gtk_button_set_image(GTK_BUTTON(st->weather_reset_button),
                         st->weather_reset_image);
    gtk_button_set_always_show_image(GTK_BUTTON(st->weather_reset_button),
                                     TRUE);
    gtk_widget_set_tooltip_text(st->weather_reset_button,
                                "Reset weather API URL");
    g_signal_connect(st->weather_reset_button, "clicked",
                     G_CALLBACK(on_weather_reset_clicked), st);
    gtk_box_pack_start(GTK_BOX(row), st->weather_reset_button,
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);
    return outer;
}

static GtkWidget *build_ui(AppState *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(root, "rootbox");

    st->menu_button = gtk_button_new_with_label("menu");
    gtk_widget_set_name(st->menu_button, "menu-button");
    gtk_widget_set_tooltip_text(st->menu_button,
                                "Open or close the translucent panel");
    gtk_widget_set_halign(st->menu_button, GTK_ALIGN_START);
    g_signal_connect(st->menu_button, "clicked", G_CALLBACK(on_menu_clicked),
                     st);
    gtk_box_pack_start(GTK_BOX(root), st->menu_button, FALSE, FALSE, 0);

    st->details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_name(st->details_box, "glass-panel");
    gtk_widget_set_no_show_all(st->details_box, TRUE);

    st->info_label = gtk_label_new("");
    gtk_widget_set_name(st->info_label, "info-label");
    gtk_label_set_xalign(GTK_LABEL(st->info_label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(st->info_label), 0.0f);
    gtk_label_set_justify(GTK_LABEL(st->info_label), GTK_JUSTIFY_LEFT);
    gtk_label_set_selectable(GTK_LABEL(st->info_label), FALSE);

    gtk_box_pack_start(GTK_BOX(st->details_box), st->info_label,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st->details_box), build_weather_box(st),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st->details_box), build_icon_box(st),
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), st->details_box, FALSE, FALSE, 0);
    gtk_widget_hide(st->details_box);

    refresh_icon_buttons(st);
    return root;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    AppState st = {0};
    load_config(&st);
    detect_battery(&st);
    g_strlcpy(st.net_host, "1.1.1.1", sizeof(st.net_host));
    g_strlcpy(st.net_line, "net: warming up...", sizeof(st.net_line));
    update_cpu(&st);

    st.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(st.window), "myPanel");
    gtk_window_set_decorated(GTK_WINDOW(st.window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(st.window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(st.window), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(st.window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(st.window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(st.window), TRUE);
    gtk_window_set_focus_on_map(GTK_WINDOW(st.window), FALSE);
    gtk_window_stick(GTK_WINDOW(st.window));
    gtk_window_move(GTK_WINDOW(st.window), WINDOW_X, WINDOW_Y);
    gtk_window_set_type_hint(GTK_WINDOW(st.window),
                             GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_widget_set_app_paintable(st.window, TRUE);

    GdkScreen *screen = gtk_widget_get_screen(st.window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual)
        gtk_widget_set_visual(st.window, visual);

    g_signal_connect(st.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(st.window, "size-allocate", G_CALLBACK(on_size_allocate),
                     &st);

    apply_css();

    st.root_box = build_ui(&st);
    gtk_container_add(GTK_CONTAINER(st.window), st.root_box);

    format_current_time(&st);
    format_uptime(&st);
    format_battery(&st);
    refresh_info_label(&st);
    refresh_weather_widgets(&st);

    g_timeout_add(1000, update_time_cb, &st);
    g_timeout_add_seconds(10, update_slow_cb, &st);
    g_timeout_add_seconds(5, update_net, &st);
    g_timeout_add_seconds(WEATHER_INTERVAL_SECONDS, update_weather, &st);

    gtk_widget_show_all(st.window);
    set_expanded(&st, FALSE);
    update_net(&st);
    update_weather(&st);

    gtk_main();
    return 0;
}
