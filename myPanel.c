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
#define SPOTIFY_INTERVAL_SECONDS 2

typedef struct {
    GtkWidget *window;
    GtkWidget *root_box;
    GtkWidget *menu_button;
    GtkWidget *details_box;
    GtkWidget *info_label;
    GtkWidget *weather_icon;
    GtkWidget *weather_label;
    GtkWidget *spotify_player_box;
    GtkWidget *spotify_art_image;
    GtkWidget *spotify_player_title_label;
    GtkWidget *spotify_player_subtitle_label;
    GtkWidget *spotify_progress;
    GtkWidget *spotify_prev_button;
    GtkWidget *spotify_play_button;
    GtkWidget *spotify_play_image;
    GtkWidget *spotify_next_button;
    GtkWidget *icon_buttons[ICON_SLOTS];
    GtkWidget *icon_images[ICON_SLOTS];
    GtkWidget *spotify_button;
    GtkWidget *spotify_image;
    GtkWidget *weather_reset_button;
    GtkWidget *weather_reset_image;
    GtkWidget *power_button;
    GtkWidget *power_image;

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

    GDBusConnection *session_bus;
    char spotify_status[32];
    char spotify_title[160];
    char spotify_artist[160];
    char spotify_album[160];
    char spotify_art_url[1024];
    char spotify_loaded_art_url[1024];
    gint64 spotify_length_us;
    gint64 spotify_position_us;
    gboolean spotify_available;
    gboolean spotify_art_inflight;

    char icon_names[ICON_SLOTS][PATH_MAX];
    char icon_commands[ICON_SLOTS][PATH_MAX];
    char icon_tooltips[ICON_SLOTS][128];

    gboolean expanded;
    guint shape_update_id;
} AppState;

static GtkWidget *make_label(const char *name, const char *text);
static GtkWidget *make_entry(const char *name, const char *placeholder,
                             const char *value);
static void image_set_best_icon(GtkWidget *image, const char *const *icons,
                                const char *fallback);
static void queue_input_region_update(AppState *st);

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

static gboolean ensure_session_bus(AppState *st) {
    if (st->session_bus)
        return TRUE;

    GError *error = NULL;
    st->session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!st->session_bus) {
        g_clear_error(&error);
        return FALSE;
    }

    return TRUE;
}

static gboolean dbus_name_has_owner(AppState *st, const char *name) {
    if (!ensure_session_bus(st))
        return FALSE;

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        st->session_bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        g_variant_new("(s)", name),
        G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE,
        300,
        NULL,
        &error);

    if (!result) {
        g_clear_error(&error);
        return FALSE;
    }

    gboolean has_owner = FALSE;
    g_variant_get(result, "(b)", &has_owner);
    g_variant_unref(result);
    return has_owner;
}

static GVariant *dbus_get_property(AppState *st, const char *bus_name,
                                   const char *path,
                                   const char *interface,
                                   const char *property) {
    if (!ensure_session_bus(st))
        return NULL;

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        st->session_bus,
        bus_name,
        path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", interface, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        300,
        NULL,
        &error);

    if (!result) {
        g_clear_error(&error);
        return NULL;
    }

    GVariant *boxed = g_variant_get_child_value(result, 0);
    GVariant *value = g_variant_get_variant(boxed);
    g_variant_unref(boxed);
    g_variant_unref(result);
    return value;
}

static void copy_metadata_string(GVariant *metadata, const char *key,
                                 char *dest, size_t len) {
    dest[0] = '\0';
    if (!metadata)
        return;

    GVariant *value =
        g_variant_lookup_value(metadata, key, G_VARIANT_TYPE_STRING);
    if (!value)
        return;

    g_strlcpy(dest, g_variant_get_string(value, NULL), len);
    g_variant_unref(value);
}

static void copy_metadata_first_string(GVariant *metadata, const char *key,
                                       char *dest, size_t len) {
    dest[0] = '\0';
    if (!metadata)
        return;

    GVariant *value =
        g_variant_lookup_value(metadata, key, G_VARIANT_TYPE("as"));
    if (!value)
        return;

    if (g_variant_n_children(value) > 0) {
        GVariant *child = g_variant_get_child_value(value, 0);
        g_strlcpy(dest, g_variant_get_string(child, NULL), len);
        g_variant_unref(child);
    }

    g_variant_unref(value);
}

static gint64 metadata_int64(GVariant *metadata, const char *key) {
    if (!metadata)
        return 0;

    GVariant *value = g_variant_lookup_value(metadata, key, NULL);
    if (!value)
        return 0;

    gint64 result = 0;
    const GVariantType *type = g_variant_get_type(value);
    if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)) {
        result = g_variant_get_int64(value);
    } else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32)) {
        result = g_variant_get_int32(value);
    } else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64)) {
        result = (gint64)g_variant_get_uint64(value);
    } else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32)) {
        result = g_variant_get_uint32(value);
    }

    g_variant_unref(value);
    return result;
}

static void set_spotify_art_default(AppState *st) {
    if (!st->spotify_art_image)
        return;

    const char *spotify_icons[] = {
        "spotify",
        "spotify-client",
        "com.spotify.Client",
        "spotify-launcher",
        NULL
    };

    image_set_best_icon(st->spotify_art_image, spotify_icons,
                        "multimedia-player");
    gtk_image_set_pixel_size(GTK_IMAGE(st->spotify_art_image), 42);
    st->spotify_loaded_art_url[0] = '\0';
}

static gboolean load_spotify_art_file(AppState *st, const char *path) {
    if (!st->spotify_art_image || !path || path[0] == '\0')
        return FALSE;

    GError *error = NULL;
    GdkPixbuf *pixbuf =
        gdk_pixbuf_new_from_file_at_scale(path, 54, 54, TRUE, &error);
    if (!pixbuf) {
        g_clear_error(&error);
        return FALSE;
    }

    gtk_image_set_from_pixbuf(GTK_IMAGE(st->spotify_art_image), pixbuf);
    g_object_unref(pixbuf);
    return TRUE;
}

typedef struct {
    AppState *st;
    char url[1024];
    char path[PATH_MAX];
} SpotifyArtJob;

static void spotify_art_download_done(GObject *source, GAsyncResult *result,
                                      gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source);
    SpotifyArtJob *job = (SpotifyArtJob *)user_data;
    GError *error = NULL;

    job->st->spotify_art_inflight = FALSE;
    if (g_subprocess_wait_check_finish(proc, result, &error) &&
        strcmp(job->st->spotify_art_url, job->url) == 0 &&
        load_spotify_art_file(job->st, job->path)) {
        g_strlcpy(job->st->spotify_loaded_art_url, job->url,
                  sizeof(job->st->spotify_loaded_art_url));
    } else {
        g_clear_error(&error);
    }

    g_free(job);
    g_object_unref(proc);
}

static void cache_spotify_art_url(AppState *st, const char *url) {
    if (st->spotify_art_inflight || !url || url[0] == '\0')
        return;

    gchar *curl = g_find_program_in_path("curl");
    if (!curl)
        return;

    gchar *cache_dir =
        g_build_filename(g_get_user_cache_dir(), "myPanel", "spotify-art",
                         NULL);
    if (g_mkdir_with_parents(cache_dir, 0700) != 0) {
        g_free(cache_dir);
        g_free(curl);
        return;
    }

    gchar *checksum =
        g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
    gchar *filename = g_strdup_printf("%s.jpg", checksum);
    gchar *path = g_build_filename(cache_dir, filename, NULL);

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        if (load_spotify_art_file(st, path))
            g_strlcpy(st->spotify_loaded_art_url, url,
                      sizeof(st->spotify_loaded_art_url));
        g_free(path);
        g_free(filename);
        g_free(checksum);
        g_free(cache_dir);
        g_free(curl);
        return;
    }

    SpotifyArtJob *job = g_new0(SpotifyArtJob, 1);
    job->st = st;
    g_strlcpy(job->url, url, sizeof(job->url));
    g_strlcpy(job->path, path, sizeof(job->path));

    GError *error = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error, curl, "-fsSL", "--max-time", "8", "-o", path, url, NULL);

    if (proc) {
        st->spotify_art_inflight = TRUE;
        g_subprocess_wait_check_async(proc, NULL, spotify_art_download_done,
                                      job);
    } else {
        g_clear_error(&error);
        g_free(job);
    }

    g_free(path);
    g_free(filename);
    g_free(checksum);
    g_free(cache_dir);
    g_free(curl);
}

static void refresh_spotify_art(AppState *st) {
    if (!st->spotify_available || st->spotify_art_url[0] == '\0') {
        set_spotify_art_default(st);
        return;
    }

    if (strcmp(st->spotify_loaded_art_url, st->spotify_art_url) == 0)
        return;

    if (g_str_has_prefix(st->spotify_art_url, "file://")) {
        GError *error = NULL;
        gchar *path = g_filename_from_uri(st->spotify_art_url, NULL, &error);
        if (path) {
            if (load_spotify_art_file(st, path))
                g_strlcpy(st->spotify_loaded_art_url, st->spotify_art_url,
                          sizeof(st->spotify_loaded_art_url));
            else
                set_spotify_art_default(st);
            g_free(path);
        } else {
            g_clear_error(&error);
            set_spotify_art_default(st);
        }
    } else if (g_str_has_prefix(st->spotify_art_url, "http://") ||
               g_str_has_prefix(st->spotify_art_url, "https://")) {
        cache_spotify_art_url(st, st->spotify_art_url);
    } else if (load_spotify_art_file(st, st->spotify_art_url)) {
        g_strlcpy(st->spotify_loaded_art_url, st->spotify_art_url,
                  sizeof(st->spotify_loaded_art_url));
    } else {
        set_spotify_art_default(st);
    }
}

static void format_player_time(gint64 us, char *dest, size_t len) {
    if (us < 0)
        us = 0;

    gint64 total_seconds = us / G_USEC_PER_SEC;
    g_snprintf(dest, len, "%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
               total_seconds / 60, total_seconds % 60);
}

static void refresh_spotify_player_widgets(AppState *st) {
    if (!st->spotify_player_title_label ||
        !st->spotify_player_subtitle_label ||
        !st->spotify_progress)
        return;

    char title[224];
    char subtitle[256];
    char time_left[32];
    char time_right[32];
    const char *play_icon = "media-playback-start";

    if (!st->spotify_available) {
        gtk_label_set_text(GTK_LABEL(st->spotify_player_title_label),
                           "Spotify");
        gtk_label_set_text(GTK_LABEL(st->spotify_player_subtitle_label),
                           "not running");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->spotify_progress),
                                      0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(st->spotify_progress),
                                  "");
        if (st->spotify_play_image)
            gtk_image_set_from_icon_name(GTK_IMAGE(st->spotify_play_image),
                                         play_icon,
                                         GTK_ICON_SIZE_MENU);
        set_spotify_art_default(st);
        return;
    }

    if (strcmp(st->spotify_status, "Playing") == 0)
        play_icon = "media-playback-pause";

    if (st->spotify_title[0] != '\0')
        g_strlcpy(title, st->spotify_title, sizeof(title));
    else
        g_strlcpy(title, "Spotify", sizeof(title));

    if (st->spotify_artist[0] != '\0' && st->spotify_album[0] != '\0') {
        g_snprintf(subtitle, sizeof(subtitle), "%s - %s",
                   st->spotify_artist, st->spotify_album);
    } else if (st->spotify_artist[0] != '\0') {
        g_strlcpy(subtitle, st->spotify_artist, sizeof(subtitle));
    } else if (st->spotify_album[0] != '\0') {
        g_strlcpy(subtitle, st->spotify_album, sizeof(subtitle));
    } else {
        g_strlcpy(subtitle, "Spotify", sizeof(subtitle));
    }

    gtk_label_set_text(GTK_LABEL(st->spotify_player_title_label), title);
    gtk_label_set_text(GTK_LABEL(st->spotify_player_subtitle_label),
                       subtitle);

    if (st->spotify_length_us > 0 && st->spotify_position_us >= 0) {
        double fraction =
            (double)st->spotify_position_us / (double)st->spotify_length_us;
        fraction = CLAMP(fraction, 0.0, 1.0);
        format_player_time(st->spotify_position_us, time_left,
                           sizeof(time_left));
        format_player_time(st->spotify_length_us, time_right,
                           sizeof(time_right));
        g_snprintf(subtitle, sizeof(subtitle), "%s / %s",
                   time_left, time_right);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->spotify_progress),
                                      fraction);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(st->spotify_progress),
                                  subtitle);
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->spotify_progress),
                                      0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(st->spotify_progress),
                                  "");
    }

    if (st->spotify_play_image)
        gtk_image_set_from_icon_name(GTK_IMAGE(st->spotify_play_image),
                                     play_icon,
                                     GTK_ICON_SIZE_MENU);

    refresh_spotify_art(st);
}

static gboolean update_spotify_now(gpointer data) {
    AppState *st = (AppState *)data;

    if (!ensure_session_bus(st)) {
        st->spotify_available = FALSE;
        refresh_spotify_player_widgets(st);
        return G_SOURCE_CONTINUE;
    }

    if (!dbus_name_has_owner(st, "org.mpris.MediaPlayer2.spotify")) {
        st->spotify_available = FALSE;
        st->spotify_status[0] = '\0';
        st->spotify_title[0] = '\0';
        st->spotify_artist[0] = '\0';
        st->spotify_album[0] = '\0';
        st->spotify_art_url[0] = '\0';
        st->spotify_length_us = 0;
        st->spotify_position_us = 0;
        refresh_spotify_player_widgets(st);
        return G_SOURCE_CONTINUE;
    }

    GVariant *status = dbus_get_property(
        st,
        "org.mpris.MediaPlayer2.spotify",
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        "PlaybackStatus");
    GVariant *metadata = dbus_get_property(
        st,
        "org.mpris.MediaPlayer2.spotify",
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        "Metadata");
    GVariant *position = dbus_get_property(
        st,
        "org.mpris.MediaPlayer2.spotify",
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        "Position");

    st->spotify_available = TRUE;
    st->spotify_status[0] = '\0';
    st->spotify_title[0] = '\0';
    st->spotify_artist[0] = '\0';
    st->spotify_album[0] = '\0';
    st->spotify_art_url[0] = '\0';
    st->spotify_length_us = 0;
    st->spotify_position_us = 0;

    if (status) {
        g_strlcpy(st->spotify_status,
                  g_variant_get_string(status, NULL),
                  sizeof(st->spotify_status));
        g_variant_unref(status);
    }

    if (metadata) {
        copy_metadata_string(metadata, "xesam:title",
                             st->spotify_title,
                             sizeof(st->spotify_title));
        copy_metadata_first_string(metadata, "xesam:artist",
                                   st->spotify_artist,
                                   sizeof(st->spotify_artist));
        copy_metadata_string(metadata, "xesam:album",
                             st->spotify_album,
                             sizeof(st->spotify_album));
        copy_metadata_string(metadata, "mpris:artUrl",
                             st->spotify_art_url,
                             sizeof(st->spotify_art_url));
        st->spotify_length_us = metadata_int64(metadata, "mpris:length");
        g_variant_unref(metadata);
    }

    if (position) {
        st->spotify_position_us = g_variant_get_int64(position);
        g_variant_unref(position);
    }

    refresh_spotify_player_widgets(st);
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
        add_widget_region(st->window, st->power_button, region, 2);
        add_widget_region(st->window, st->icon_buttons[0], region, 2);
        add_widget_region(st->window, st->icon_buttons[1], region, 2);
        add_widget_region(st->window, st->spotify_button, region, 2);
        add_widget_region(st->window, st->icon_buttons[2], region, 2);
        add_widget_region(st->window, st->weather_reset_button, region, 2);
        add_widget_region(st->window, st->icon_buttons[3], region, 2);
        add_widget_region(st->window, st->spotify_prev_button, region, 2);
        add_widget_region(st->window, st->spotify_play_button, region, 2);
        add_widget_region(st->window, st->spotify_next_button, region, 2);
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
    g_timeout_add(150, apply_input_region_cb, st);
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

static gboolean spawn_argv_async(const gchar *const *argv,
                                 const char *error_context) {
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL,
                                (gchar **)argv,
                                NULL,
                                G_SPAWN_SEARCH_PATH,
                                NULL,
                                NULL,
                                NULL,
                                &error);

    if (!ok) {
        g_printerr("myPanel: %s failed: %s\n",
                   error_context,
                   error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    return ok;
}

static void notify_user(const char *summary, const char *body) {
    gchar *notify = g_find_program_in_path("notify-send");
    if (!notify)
        return;

    const gchar *argv[] = {
        notify, summary, body, NULL
    };
    spawn_argv_async(argv, "notification");
    g_free(notify);
}

static void launch_spotify(void) {
    gchar *local_spotify = g_build_filename(g_get_home_dir(),
                                            ".local/bin/spotify",
                                            NULL);
    gchar *spotify = NULL;
    gchar *spotify_launcher = g_find_program_in_path("spotify-launcher");

    if (g_file_test(local_spotify, G_FILE_TEST_IS_EXECUTABLE))
        spotify = g_strdup(local_spotify);
    else
        spotify = g_find_program_in_path("spotify");

    g_free(local_spotify);

    if (spotify) {
        const gchar *argv[] = { spotify, NULL };
        spawn_argv_async(argv, "spotify");
        g_free(spotify_launcher);
        g_free(spotify);
        return;
    }

    if (spotify_launcher) {
        const gchar *argv[] = { spotify_launcher, NULL };
        spawn_argv_async(argv, "spotify-launcher");
        g_free(spotify);
        g_free(spotify_launcher);
        return;
    }

    g_free(spotify_launcher);
    g_free(spotify);

    gchar *pkexec = g_find_program_in_path("pkexec");
    gchar *pacman = g_find_program_in_path("pacman");
    if (pkexec && pacman) {
        notify_user("myPanel",
                    "Spotify is not installed. Requesting spotify-launcher installation.");
        const gchar *argv[] = {
            "sh", "-lc",
            "pkexec pacman -S --needed spotify-launcher playerctl && setsid -f spotify-launcher",
            NULL
        };
        spawn_argv_async(argv, "spotify installer");
    } else {
        notify_user("myPanel",
                    "Install spotify-launcher and playerctl, then press Spotify again.");
    }

    g_free(pkexec);
    g_free(pacman);
}

static void on_spotify_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    launch_spotify();
}

static void call_spotify_player_method(AppState *st, const char *method) {
    if (!method || method[0] == '\0')
        return;

    if (!ensure_session_bus(st))
        return;

    g_dbus_connection_call(st->session_bus,
                           "org.mpris.MediaPlayer2.spotify",
                           "/org/mpris/MediaPlayer2",
                           "org.mpris.MediaPlayer2.Player",
                           method,
                           NULL,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           500,
                           NULL,
                           NULL,
                           NULL);
}

static gboolean refresh_spotify_soon(gpointer data) {
    update_spotify_now(data);
    return G_SOURCE_REMOVE;
}

static void on_spotify_player_clicked(GtkButton *button, gpointer user_data) {
    AppState *st = (AppState *)user_data;
    const char *method =
        (const char *)g_object_get_data(G_OBJECT(button), "spotify-method");

    if (!st->spotify_available) {
        launch_spotify();
        return;
    }

    call_spotify_player_method(st, method);
    g_timeout_add(250, refresh_spotify_soon, st);
}

static void on_power_action_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    const char *cmd =
        (const char *)g_object_get_data(G_OBJECT(button), "power-command");
    gpointer confirm_data =
        g_object_get_data(G_OBJECT(button), "confirm-switch");
    gpointer dialog_data =
        g_object_get_data(G_OBJECT(button), "power-dialog");
    GtkWidget *confirm_switch =
        confirm_data ? GTK_WIDGET(confirm_data) : NULL;
    GtkWidget *dialog =
        dialog_data ? GTK_WIDGET(dialog_data) : NULL;

    if (!cmd || !confirm_switch)
        return;

    if (!gtk_switch_get_active(GTK_SWITCH(confirm_switch))) {
        notify_user("myPanel", "Turn on the power confirmation switch first.");
        return;
    }

    GError *error = NULL;
    if (!g_spawn_command_line_async(cmd, &error)) {
        g_printerr("myPanel: power command failed: %s\n",
                   error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    if (dialog)
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
}

static void on_power_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *st = (AppState *)user_data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Power",
        GTK_WINDOW(st->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *confirm_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *confirm_label = gtk_label_new("confirm");
    GtkWidget *confirm_switch = gtk_switch_new();
    const char *labels[] = {
        "poweroff", "suspend", "hibernate", "shutdown"
    };
    const char *commands[] = {
        "systemctl poweroff",
        "systemctl suspend",
        "systemctl hibernate",
        "shutdown now"
    };

    gtk_widget_set_name(outer, "power-menu");
    gtk_widget_set_name(confirm_label, "confirm-label");
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);
    gtk_label_set_xalign(GTK_LABEL(confirm_label), 0.0f);
    gtk_widget_set_tooltip_text(confirm_switch,
                                "Enable before running a power action");

    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++) {
        GtkWidget *action_button = gtk_button_new_with_label(labels[i]);
        g_object_set_data(G_OBJECT(action_button), "power-command",
                          (gpointer)commands[i]);
        g_object_set_data(G_OBJECT(action_button), "confirm-switch",
                          confirm_switch);
        g_object_set_data(G_OBJECT(action_button), "power-dialog", dialog);
        g_signal_connect(action_button, "clicked",
                         G_CALLBACK(on_power_action_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(button_row), action_button,
                           FALSE, FALSE, 0);
    }

    gtk_box_pack_end(GTK_BOX(confirm_row), confirm_switch, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(confirm_row), confirm_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), button_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), confirm_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), outer, TRUE, TRUE, 0);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
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

static void image_set_best_icon(GtkWidget *image,
                                const char *const *icons,
                                const char *fallback) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    const char *icon_name = fallback;

    for (int i = 0; icons[i] != NULL; i++) {
        if (gtk_icon_theme_has_icon(theme, icons[i])) {
            icon_name = icons[i];
            break;
        }
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(image),
                                 icon_name,
                                 GTK_ICON_SIZE_LARGE_TOOLBAR);
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
        "#spotify-player {"
        "  min-width: 300px;"
        "  min-height: 58px;"
        "  padding: 7px;"
        "  border-radius: 8px;"
        "  border: 1px solid rgba(255,255,255,0.34);"
        "  background-image: linear-gradient(to bottom,"
        "    rgba(255,255,255,0.17), rgba(18,24,32,0.32));"
        "  box-shadow: inset 0 1px rgba(255,255,255,0.25);"
        "}"
        "#spotify-art {"
        "  min-width: 54px;"
        "  min-height: 54px;"
        "}"
        "#player-title {"
        "  color: #ffffff;"
        "  font: 700 11px Sans;"
        "  text-shadow: 0 1px rgba(0,0,0,0.70);"
        "}"
        "#player-subtitle {"
        "  color: rgba(255,255,255,0.82);"
        "  font: 10px Sans;"
        "  text-shadow: 0 1px rgba(0,0,0,0.64);"
        "}"
        "#player-progress {"
        "  min-height: 7px;"
        "  margin-top: 2px;"
        "  font: 8px Sans;"
        "  color: rgba(255,255,255,0.74);"
        "}"
        "#player-progress trough {"
        "  min-height: 5px;"
        "  border-radius: 999px;"
        "  background-color: rgba(255,255,255,0.18);"
        "}"
        "#player-progress progress {"
        "  min-height: 5px;"
        "  border-radius: 999px;"
        "  background-image: linear-gradient(to right,"
        "    rgba(30,215,96,0.92), rgba(153,235,183,0.90));"
        "}"
        "#player-control {"
        "  min-width: 24px;"
        "  min-height: 24px;"
        "  padding: 0;"
        "  border-radius: 999px;"
        "  border: 1px solid rgba(255,255,255,0.36);"
        "  background-color: rgba(255,255,255,0.18);"
        "  box-shadow: inset 0 1px rgba(255,255,255,0.24);"
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
        "}"
        "#power-menu {"
        "  margin-top: 4px;"
        "  padding: 6px;"
        "  border-radius: 8px;"
        "  border: 1px solid rgba(255,255,255,0.34);"
        "  background-color: rgba(8,12,16,0.30);"
        "}"
        "#power-menu button {"
        "  min-height: 24px;"
        "  border-radius: 6px;"
        "  border: 1px solid rgba(255,255,255,0.34);"
        "  color: #16202b;"
        "  text-shadow: none;"
        "  background-image: linear-gradient(to bottom,"
        "    rgba(255,255,255,0.94), rgba(220,228,238,0.84));"
        "  font: 700 10px Sans;"
        "}"
        "#confirm-label {"
        "  color: rgba(255,255,255,0.88);"
        "  font: 10px Sans;"
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

    gtk_box_pack_start(GTK_BOX(outer), make_label("section-label", "weather"),
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

static GtkWidget *make_player_button(AppState *st, GtkWidget **image_out,
                                     const char *icon_name,
                                     const char *tooltip,
                                     const char *method) {
    GtkWidget *button = gtk_button_new();
    GtkWidget *image = gtk_image_new_from_icon_name(icon_name,
                                                    GTK_ICON_SIZE_MENU);

    gtk_widget_set_name(button, "player-control");
    gtk_image_set_pixel_size(GTK_IMAGE(image), 13);
    gtk_button_set_image(GTK_BUTTON(button), image);
    gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_object_set_data(G_OBJECT(button), "spotify-method", (gpointer)method);
    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_spotify_player_clicked), st);

    if (image_out)
        *image_out = image;

    return button;
}

static GtkWidget *build_spotify_player_box(AppState *st) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    const char *spotify_icons[] = {
        "spotify",
        "spotify-client",
        "com.spotify.Client",
        "spotify-launcher",
        NULL
    };

    gtk_widget_set_name(outer, "spotify-player");
    gtk_widget_set_size_request(outer, 300, 66);

    st->spotify_art_image = gtk_image_new();
    gtk_widget_set_name(st->spotify_art_image, "spotify-art");
    image_set_best_icon(st->spotify_art_image, spotify_icons,
                        "multimedia-player");
    gtk_image_set_pixel_size(GTK_IMAGE(st->spotify_art_image), 42);

    st->spotify_player_title_label =
        make_label("player-title", "Spotify");
    st->spotify_player_subtitle_label =
        make_label("player-subtitle", "not running");
    st->spotify_progress = gtk_progress_bar_new();
    gtk_widget_set_name(st->spotify_progress, "player-progress");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(st->spotify_progress),
                                   TRUE);

    gtk_label_set_ellipsize(GTK_LABEL(st->spotify_player_title_label),
                            PANGO_ELLIPSIZE_END);
    gtk_label_set_ellipsize(GTK_LABEL(st->spotify_player_subtitle_label),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(st->spotify_player_title_label, 138, -1);
    gtk_widget_set_size_request(st->spotify_player_subtitle_label, 138, -1);
    gtk_widget_set_size_request(st->spotify_progress, 138, -1);

    st->spotify_prev_button =
        make_player_button(st, NULL, "media-skip-backward",
                           "Previous track", "Previous");
    st->spotify_play_button =
        make_player_button(st, &st->spotify_play_image,
                           "media-playback-start",
                           "Play or pause", "PlayPause");
    st->spotify_next_button =
        make_player_button(st, NULL, "media-skip-forward",
                           "Next track", "Next");

    gtk_box_pack_start(GTK_BOX(text_box), st->spotify_player_title_label,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text_box), st->spotify_player_subtitle_label,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text_box), st->spotify_progress,
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(controls), st->spotify_prev_button,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), st->spotify_play_button,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), st->spotify_next_button,
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(right_box), text_box, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(right_box), controls, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), st->spotify_art_image,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), right_box, TRUE, TRUE, 0);

    st->spotify_player_box = outer;
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
    }

    st->spotify_image = gtk_image_new();
    const char *spotify_icons[] = {
        "spotify",
        "spotify-client",
        "com.spotify.Client",
        "spotify-launcher",
        NULL
    };
    image_set_best_icon(st->spotify_image, spotify_icons,
                        "multimedia-player");
    st->spotify_button = gtk_button_new();
    gtk_widget_set_name(st->spotify_button, "icon-button");
    gtk_button_set_image(GTK_BUTTON(st->spotify_button), st->spotify_image);
    gtk_button_set_always_show_image(GTK_BUTTON(st->spotify_button), TRUE);
    gtk_widget_set_tooltip_text(st->spotify_button,
                                "Spotify native app");
    g_signal_connect(st->spotify_button, "clicked",
                     G_CALLBACK(on_spotify_clicked), st);

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

    gtk_box_pack_start(GTK_BOX(row), st->icon_buttons[0],
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->icon_buttons[1],
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->spotify_button,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->icon_buttons[2],
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->weather_reset_button,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->icon_buttons[3],
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

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    st->info_label = gtk_label_new("");
    gtk_widget_set_name(st->info_label, "info-label");
    gtk_label_set_xalign(GTK_LABEL(st->info_label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(st->info_label), 0.0f);
    gtk_label_set_justify(GTK_LABEL(st->info_label), GTK_JUSTIFY_LEFT);
    gtk_label_set_selectable(GTK_LABEL(st->info_label), FALSE);

    st->power_image = gtk_image_new();
    const char *power_icons[] = {
        "system-shutdown",
        "application-exit",
        NULL
    };
    image_set_best_icon(st->power_image, power_icons, "application-exit");
    st->power_button = gtk_button_new();
    gtk_widget_set_name(st->power_button, "icon-button");
    gtk_button_set_image(GTK_BUTTON(st->power_button), st->power_image);
    gtk_button_set_always_show_image(GTK_BUTTON(st->power_button), TRUE);
    gtk_widget_set_tooltip_text(st->power_button, "Power menu");
    g_signal_connect(st->power_button, "clicked",
                     G_CALLBACK(on_power_clicked), st);

    gtk_box_pack_start(GTK_BOX(top_row), st->info_label,
                       TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top_row), st->power_button,
                     FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(st->details_box), top_row,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st->details_box), build_weather_box(st),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st->details_box), build_icon_box(st),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st->details_box), build_spotify_player_box(st),
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
    refresh_spotify_player_widgets(&st);

    g_timeout_add(1000, update_time_cb, &st);
    g_timeout_add_seconds(10, update_slow_cb, &st);
    g_timeout_add_seconds(5, update_net, &st);
    g_timeout_add_seconds(WEATHER_INTERVAL_SECONDS, update_weather, &st);
    g_timeout_add_seconds(SPOTIFY_INTERVAL_SECONDS, update_spotify_now, &st);

    gtk_widget_show_all(st.window);
    set_expanded(&st, FALSE);
    update_net(&st);
    update_weather(&st);
    update_spotify_now(&st);

    gtk_main();
    if (st.session_bus)
        g_object_unref(st.session_bus);
    return 0;
}
