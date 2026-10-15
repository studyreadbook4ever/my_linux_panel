#include "notification_center.h"
#include <gio/gio.h>

#define SERVICE "org.xfce.Notifyd"
#define OBJECT "/org/xfce/Notifyd"
#define IFACE "org.xfce.Notifyd.Log"
#define PAGE 100

struct NotificationCenter {
  gint refs;
  gboolean dead, available, list_busy, unread_busy, dirty, more;
  guint watch, subscription, refresh_source, generation, loaded_limit;
  gchar *scroll_anchor;
  gdouble scroll_offset, scroll_value;
  gboolean restore_scroll;
  GDBusConnection *bus;
  GCancellable *cancel;
  gchar *owner, *cursor;
  GHashTable *ids;
  GtkWindow *parent;
  GtkWidget *button, *badge, *window, *rows, *status, *next, *all, *scroll;
};
typedef struct {
  NotificationCenter *c;
  guint generation, count;
  gboolean append;
} Request;
static NotificationCenter *center_ref(NotificationCenter *c) {
  c->refs++;
  return c;
}
static void center_unref(gpointer data) {
  NotificationCenter *c = data;
  if (--c->refs)
    return;
  g_clear_object(&c->bus);
  g_clear_object(&c->cancel);
  g_free(c->owner);
  g_free(c->cursor);
  g_free(c->scroll_anchor);
  g_hash_table_unref(c->ids);
  g_free(c);
}
static Request *request_new(NotificationCenter *c, gboolean append) {
  Request *r = g_new0(Request, 1);
  r->c = center_ref(c);
  r->generation = c->generation;
  r->append = append;
  return r;
}
static void request_free(Request *r) {
  center_unref(r->c);
  g_free(r);
}
static gboolean current(Request *r) {
  return !r->c->dead && r->generation == r->c->generation;
}
static void schedule_refresh(NotificationCenter *c);
static void fetch_list(NotificationCenter *c, gboolean append);
static void set_status(NotificationCenter *c, const gchar *text) {
  if (c->status)
    gtk_label_set_text(GTK_LABEL(c->status), text);
}
static void clear_rows(NotificationCenter *c) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(c->rows));
  for (GList *l = children; l; l = l->next)
    gtk_widget_destroy(l->data);
  g_list_free(children);
  g_hash_table_remove_all(c->ids);
  g_clear_pointer(&c->cursor, g_free);
  c->more = FALSE;
}
static void remember_scroll(NotificationCenter *c) {
  GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(c->scroll));
  c->scroll_value = gtk_adjustment_get_value(adjustment);
  g_clear_pointer(&c->scroll_anchor, g_free);
  GList *children = gtk_container_get_children(GTK_CONTAINER(c->rows));
  for (GList *l = children; l; l = l->next) {
    const gchar *id = g_object_get_data(G_OBJECT(l->data), "notification-id");
    GtkAllocation allocation;
    gtk_widget_get_allocation(l->data, &allocation);
    if (id && allocation.y + allocation.height > c->scroll_value) {
      c->scroll_anchor = g_strdup(id);
      c->scroll_offset = c->scroll_value - allocation.y;
      break;
    }
  }
  g_list_free(children);
  c->restore_scroll = TRUE;
}
static void restore_scroll_position(GtkWidget *widget,
                                    GtkAllocation *allocation, gpointer data) {
  (void)widget;
  (void)allocation;
  NotificationCenter *c = data;
  if (!c->restore_scroll)
    return;
  c->restore_scroll = FALSE;
  gdouble value = c->scroll_value;
  GList *children = gtk_container_get_children(GTK_CONTAINER(c->rows));
  for (GList *l = children; l; l = l->next) {
    const gchar *id = g_object_get_data(G_OBJECT(l->data), "notification-id");
    if (id && g_strcmp0(id, c->scroll_anchor) == 0) {
      GtkAllocation row_allocation;
      gtk_widget_get_allocation(l->data, &row_allocation);
      value = row_allocation.y + c->scroll_offset;
      break;
    }
  }
  g_list_free(children);
  GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(c->scroll));
  gdouble maximum = MAX(gtk_adjustment_get_lower(adjustment),
                        gtk_adjustment_get_upper(adjustment) -
                            gtk_adjustment_get_page_size(adjustment));
  gtk_adjustment_set_value(
      adjustment, CLAMP(value, gtk_adjustment_get_lower(adjustment), maximum));
}
static gchar *limited(const gchar *text, glong count) {
  gchar *bytes = g_strndup(text ? text : "", (gsize)count * 4);
  gchar *valid = g_utf8_make_valid(bytes, -1);
  g_free(bytes);
  gchar *out = g_utf8_substring(valid, 0, MIN(g_utf8_strlen(valid, -1), count));
  g_free(valid);
  return out;
}
static GtkWidget *label(const gchar *text, glong limit) {
  GtkWidget *w = gtk_label_new(NULL);
  gchar *s = limited(text, limit);
  gtk_label_set_text(GTK_LABEL(w), s);
  g_free(s);
  gtk_label_set_xalign(GTK_LABEL(w), 0);
  gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(w), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(w), 65);
  return w;
}
static void action_done(GObject *source, GAsyncResult *result, gpointer data) {
  Request *r = data;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (current(r)) {
    if (error)
      set_status(r->c, "Could not update notification history. Try again.");
    else
      schedule_refresh(r->c);
  }
  g_clear_pointer(&reply, g_variant_unref);
  g_clear_error(&error);
  request_free(r);
}
static void action(GtkButton *button, gpointer data) {
  NotificationCenter *c = data;
  if (!c->available || c->dead)
    return;
  const gchar *method = g_object_get_data(G_OBJECT(button), "method");
  const gchar *id = g_object_get_data(G_OBJECT(button), "id");
  const gchar *ids[] = {id, NULL};
  g_dbus_connection_call(c->bus, c->owner, OBJECT, IFACE, method,
                         id ? g_variant_new("(^as)", ids) : NULL,
                         G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                         4000, c->cancel, action_done, request_new(c, FALSE));
}
static GtkWidget *action_button(NotificationCenter *c, const gchar *title,
                                const gchar *method, const gchar *id) {
  GtkWidget *w = gtk_button_new_with_label(title);
  g_object_set_data_full(G_OBJECT(w), "method", g_strdup(method), g_free);
  if (id)
    g_object_set_data_full(G_OBJECT(w), "id", g_strdup(id), g_free);
  g_signal_connect(w, "clicked", G_CALLBACK(action), c);
  return w;
}
static void add_entry(NotificationCenter *c, GVariant *entry) {
  /* Official xfce-notify-log-dbus.xml: id, timestamp(us), timezone,
     app id, app name, icon id, summary, body, actions, timeout, is_read. */
  const gchar *id, *zone, *app_id, *app, *icon, *summary, *body;
  gint64 timestamp;
  gint timeout;
  gboolean read;
  GVariant *actions;
  g_variant_get(entry, "(&sx&s&s&s&s&s&s@a(ss)ib)", &id, &timestamp, &zone,
                &app_id, &app, &icon, &summary, &body, &actions, &timeout,
                &read);
  g_variant_unref(actions);
  g_free(c->cursor);
  c->cursor = g_strdup(id);
  if (g_hash_table_contains(c->ids, id) || g_hash_table_size(c->ids) >= 1000)
    return;
  g_hash_table_add(c->ids, g_strdup(id));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  g_object_set_data_full(G_OBJECT(box), "notification-id", g_strdup(id),
                         g_free);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  GDateTime *dt = g_date_time_new_from_unix_local(timestamp / G_USEC_PER_SEC);
  gchar *when =
      dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : g_strdup("Unknown time");
  gchar *safe_app = limited(*app ? app : app_id, 120);
  gchar *header = g_strdup_printf("%s  ·  %s  ·  %s", safe_app, when,
                                  read ? "Read" : "Unread");
  gtk_box_pack_start(GTK_BOX(box), label(header, 200), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), label(summary, 300), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), label(body, 2000), FALSE, FALSE, 0);
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  if (!read)
    gtk_box_pack_start(GTK_BOX(buttons),
                       action_button(c, "Mark read", "MarkRead", id), FALSE,
                       FALSE, 0);
  gtk_box_pack_start(GTK_BOX(buttons), action_button(c, "Delete", "Delete", id),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(c->rows), box, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(c->rows),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 0);
  g_free(header);
  g_free(safe_app);
  g_free(when);
  if (dt)
    g_date_time_unref(dt);
}
static void list_done(GObject *source, GAsyncResult *result, gpointer data) {
  Request *r = data;
  NotificationCenter *c = r->c;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (current(r)) {
    c->list_busy = FALSE;
    if (error) {
      set_status(c, "Xfce notification history unavailable or unsupported. "
                    "Open Notification settings.");
      c->more = FALSE;
    } else if (!c->dirty) {
      GVariant *entries = g_variant_get_child_value(reply, 0);
      if (!r->append) {
        remember_scroll(c);
        clear_rows(c);
      }
      gsize n = g_variant_n_children(entries);
      for (gsize i = 0; i < n; i++) {
        GVariant *entry = g_variant_get_child_value(entries, i);
        add_entry(c, entry);
        g_variant_unref(entry);
      }
      gtk_widget_show_all(c->rows);
      c->more = n == r->count && c->loaded_limit < 1000;
      set_status(c,
                 g_hash_table_size(c->ids) >= 1000
                     ? "Showing the latest 1,000 notifications. Older history "
                       "is available in Notification settings."
                 : g_hash_table_size(c->ids)
                     ? "Opening this window does not mark notifications read."
                     : "No saved notifications. Check that logging is enabled "
                       "in Notification settings.");
      g_variant_unref(entries);
    }
    gtk_widget_set_sensitive(c->next, c->more && !c->dirty);
    if (c->dirty)
      schedule_refresh(c);
  }
  g_clear_pointer(&reply, g_variant_unref);
  g_clear_error(&error);
  request_free(r);
}
static void fetch_list(NotificationCenter *c, gboolean append) {
  if (!c->available || c->list_busy || !c->window ||
      !gtk_widget_get_visible(c->window))
    return;
  c->list_busy = TRUE;
  gtk_widget_set_sensitive(c->next, FALSE);
  if (append)
    c->loaded_limit = MIN(c->loaded_limit + PAGE, 1000);
  Request *request = request_new(c, append);
  request->count = append ? PAGE : c->loaded_limit;
  g_dbus_connection_call(
      c->bus, c->owner, OBJECT, IFACE, "List",
      g_variant_new("(sub)", append && c->cursor ? c->cursor : "",
                    request->count, FALSE),
      G_VARIANT_TYPE("(a(sxssssssa(ss)ib))"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      4000, c->cancel, list_done, request);
}
static void unread_done(GObject *source, GAsyncResult *result, gpointer data) {
  Request *r = data;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (current(r)) {
    r->c->unread_busy = FALSE;
    gboolean unread = FALSE;
    if (reply)
      g_variant_get(reply, "(b)", &unread);
    gtk_label_set_text(GTK_LABEL(r->c->badge), unread ? "•" : "");
    gtk_widget_set_tooltip_text(r->c->button,
                                error    ? "Notification history unavailable"
                                : unread ? "Notifications: unread items"
                                         : "Notifications");
    if (r->c->dirty)
      schedule_refresh(r->c);
  }
  g_clear_pointer(&reply, g_variant_unref);
  g_clear_error(&error);
  request_free(r);
}
static gboolean refresh(gpointer data) {
  NotificationCenter *c = data;
  c->refresh_source = 0;
  if (c->dead || !c->available)
    return G_SOURCE_REMOVE;
  /* In-flight requests will reschedule after the signal burst settles. */
  if (c->list_busy || c->unread_busy) {
    c->dirty = TRUE;
    return G_SOURCE_REMOVE;
  }
  c->dirty = FALSE;
  c->unread_busy = TRUE;
  g_dbus_connection_call(c->bus, c->owner, OBJECT, IFACE, "HasUnread", NULL,
                         G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                         4000, c->cancel, unread_done, request_new(c, FALSE));
  fetch_list(c, FALSE);
  return G_SOURCE_REMOVE;
}
static void schedule_refresh(NotificationCenter *c) {
  if (c->dead)
    return;
  c->dirty = TRUE;
  if (!c->refresh_source)
    c->refresh_source = g_timeout_add_full(G_PRIORITY_DEFAULT, 150, refresh,
                                           center_ref(c), center_unref);
}
static void changed(GDBusConnection *bus, const gchar *sender,
                    const gchar *path, const gchar *iface, const gchar *signal,
                    GVariant *parameters, gpointer data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)iface;
  (void)signal;
  (void)parameters;
  schedule_refresh(data);
}
static void disconnect_service(NotificationCenter *c) {
  c->generation++;
  c->available = FALSE;
  c->list_busy = c->unread_busy = FALSE;
  if (c->subscription) {
    g_dbus_connection_signal_unsubscribe(c->bus, c->subscription);
    c->subscription = 0;
  }
  g_clear_object(&c->bus);
  g_clear_pointer(&c->owner, g_free);
}
static void appeared(GDBusConnection *bus, const gchar *name,
                     const gchar *owner, gpointer data) {
  (void)name;
  NotificationCenter *c = data;
  if (c->dead)
    return;
  disconnect_service(c);
  c->bus = g_object_ref(bus);
  c->owner = g_strdup(owner);
  c->available = TRUE;
  c->subscription = g_dbus_connection_signal_subscribe(
      bus, owner, IFACE, NULL, OBJECT, NULL, G_DBUS_SIGNAL_FLAGS_NONE, changed,
      center_ref(c), center_unref);
  if (c->all)
    gtk_widget_set_sensitive(c->all, TRUE);
  schedule_refresh(c);
}
static void vanished(GDBusConnection *bus, const gchar *name, gpointer data) {
  (void)bus;
  (void)name;
  NotificationCenter *c = data;
  if (c->dead)
    return;
  disconnect_service(c);
  gtk_label_set_text(GTK_LABEL(c->badge), "");
  gtk_widget_set_tooltip_text(c->button,
                              "Xfce notification service unavailable");
  if (c->rows)
    clear_rows(c);
  if (c->all)
    gtk_widget_set_sensitive(c->all, FALSE);
  if (c->next)
    gtk_widget_set_sensitive(c->next, FALSE);
  set_status(
      c, "Xfce notification service unavailable. Open Notification settings.");
}
static gboolean hide_window(GtkWidget *window, GdkEvent *event, gpointer data) {
  (void)event;
  (void)data;
  gtk_widget_hide(window);
  return TRUE;
}
static void settings(GtkButton *button, gpointer data) {
  (void)button;
  NotificationCenter *c = data;
  GError *error = NULL;
  gchar *argv[] = {"xfce4-notifyd-config", NULL};
  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                     &error)) {
    set_status(c,
               "Could not open xfce4-notifyd-config. Install xfce4-notifyd.");
    g_clear_error(&error);
  }
}
static void next_page(GtkButton *button, gpointer data) {
  (void)button;
  NotificationCenter *c = data;
  if (c->more)
    fetch_list(c, TRUE);
}
static void open_window(GtkButton *button, gpointer data) {
  (void)button;
  NotificationCenter *c = data;
  if (!c->window) {
    c->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(c->window), "Notifications");
    /* The panel installs transparent window CSS on the whole screen. */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        css,
        "window { background-color: @theme_bg_color; color: @theme_fg_color; }",
        -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(c->window),
                                   GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(css);
    gtk_window_set_default_size(GTK_WINDOW(c->window), 540, 580);
    if (c->parent)
      gtk_window_set_transient_for(GTK_WINDOW(c->window), c->parent);
    g_signal_connect(c->window, "delete-event", G_CALLBACK(hide_window), c);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);
    gtk_container_add(GTK_CONTAINER(c->window), outer);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    c->all = action_button(c, "Mark all read", "MarkAllRead", NULL);
    gtk_widget_set_sensitive(c->all, c->available);
    GtkWidget *config = gtk_button_new_with_label("Notification settings");
    g_signal_connect(config, "clicked", G_CALLBACK(settings), c);
    gtk_box_pack_start(GTK_BOX(toolbar), c->all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), config, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), toolbar, FALSE, FALSE, 0);
    c->status = label("Loading notification history…", 300);
    gtk_box_pack_start(GTK_BOX(outer), c->status, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    c->scroll = scroll;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    c->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_signal_connect(c->rows, "size-allocate",
                     G_CALLBACK(restore_scroll_position), c);
    gtk_container_add(GTK_CONTAINER(scroll), c->rows);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    c->next = gtk_button_new_with_label("More (100)");
    gtk_widget_set_sensitive(c->next, FALSE);
    g_signal_connect(c->next, "clicked", G_CALLBACK(next_page), c);
    gtk_box_pack_start(GTK_BOX(outer), c->next, FALSE, FALSE, 0);
  }
  gtk_widget_show_all(c->window);
  gtk_window_present(GTK_WINDOW(c->window));
  if (c->available)
    schedule_refresh(c);
  else
    set_status(
        c,
        "Xfce notification service unavailable. Open Notification settings.");
}
NotificationCenter *notification_center_new(GtkWindow *parent) {
  NotificationCenter *c = g_new0(NotificationCenter, 1);
  c->refs = 1;
  c->loaded_limit = PAGE;
  c->parent = parent;
  if (parent)
    g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&c->parent);
  c->cancel = g_cancellable_new();
  c->ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  c->button = gtk_button_new();
  g_object_ref_sink(c->button);
  atk_object_set_name(gtk_widget_get_accessible(c->button), "Notifications");
  gtk_widget_set_name(c->button, "icon-button");
  gtk_widget_set_tooltip_text(c->button, "Notifications");
  GtkWidget *box = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(box), gtk_image_new_from_icon_name(
                                            "preferences-system-notifications",
                                            GTK_ICON_SIZE_LARGE_TOOLBAR));
  c->badge = gtk_label_new("");
  gtk_widget_set_halign(c->badge, GTK_ALIGN_END);
  gtk_widget_set_valign(c->badge, GTK_ALIGN_START);
  gtk_overlay_add_overlay(GTK_OVERLAY(box), c->badge);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(box), c->badge, TRUE);
  gtk_container_add(GTK_CONTAINER(c->button), box);
  gtk_widget_show_all(box);
  g_signal_connect(c->button, "clicked", G_CALLBACK(open_window), c);
  c->watch = g_bus_watch_name(G_BUS_TYPE_SESSION, SERVICE,
                              G_BUS_NAME_WATCHER_FLAGS_NONE, appeared,
                              vanished, center_ref(c), center_unref);
  return c;
}
GtkWidget *notification_center_get_button(NotificationCenter *c) {
  return c->button;
}
void notification_center_free(NotificationCenter *c) {
  if (!c || c->dead)
    return;
  c->dead = TRUE;
  g_cancellable_cancel(c->cancel);
  if (c->refresh_source) {
    g_source_remove(c->refresh_source);
    c->refresh_source = 0;
  }
  if (c->watch) {
    g_bus_unwatch_name(c->watch);
    c->watch = 0;
  }
  disconnect_service(c);
  if (c->parent)
    g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
  if (c->window)
    gtk_widget_destroy(c->window);
  gtk_widget_destroy(c->button);
  g_object_unref(c->button);
  center_unref(c);
}
