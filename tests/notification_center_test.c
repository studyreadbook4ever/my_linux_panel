/* Integration tests use the public module API and real GTK widgets against a
 * private, in-process implementation of the official Xfce Log DBus contract.
 * All rows are synthetic. Never run on the desktop's session bus. */
#include "notification_center.h"
#include <gio/gio.h>
#include <string.h>

#define NAME "org.xfce.Notifyd"
#define PATH "/org/xfce/Notifyd"
#define IFACE "org.xfce.Notifyd.Log"
static const char xml[] =
 "<node><interface name='" IFACE "'>"
 "<method name='List'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
 "<arg type='b' direction='in'/><arg type='a(sxssssssa(ss)ib)' direction='out'/></method>"
 "<method name='HasUnread'><arg type='b' direction='out'/></method>"
 "<method name='MarkRead'><arg type='as' direction='in'/></method>"
 "<method name='MarkAllRead'/><method name='Delete'><arg type='as' direction='in'/></method>"
 "<signal name='RowAdded'><arg type='s'/></signal>"
 "<signal name='RowChanged'><arg type='s'/></signal>"
 "<signal name='RowDeleted'><arg type='s'/></signal>"
 "<signal name='Cleared'/><signal name='Truncated'><arg type='u'/></signal>"
 "</interface></node>";
static GDBusConnection *bus;
static guint registration;
static guint rows = 3, list_calls, unread_calls, mark_calls, delete_calls, all_calls, last_count;
static gboolean fail_list, duplicate, hold_list, prepend;
static gint unread_override = -1;
static gchar *edge_body;
static gchar *last_id, *last_cursor;
static GDBusMethodInvocation *pending;

static void spin(guint milliseconds) {
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}
static void reply_list(GDBusMethodInvocation *invocation, guint start, guint count) {
    GVariantBuilder entries;
    g_variant_builder_init(&entries, G_VARIANT_TYPE("a(sxssssssa(ss)ib)"));
    for (guint i = start; i < MIN(rows, start + count); i++) {
        guint index = duplicate && i == start + 1 ? start : i;
        if (prepend) index = i == 0 ? 99999 : i - 1;
        gchar *id = g_strdup_printf("synthetic-%u", index);
        gchar *summary = g_strdup_printf(edge_body && index == 0 ?
            "TEST SUMMARY %u <b>literal</b> 한글" : "TEST SUMMARY %u", index);
        GVariantBuilder actions;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("a(ss)"));
        g_variant_builder_add(&entries, "(sxssssss@a(ss)ib)", id,
            (gint64)1700000000000000 + index, "UTC", "test-app", "Test app",
            "dialog-information", summary, edge_body && index == 0 ? edge_body : "Synthetic test body only",
            g_variant_builder_end(&actions), 1000, FALSE);
        g_free(summary); g_free(id);
    }
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(@a(sxssssssa(ss)ib))", g_variant_builder_end(&entries)));
}
static void method_call(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *iface, const gchar *method,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
    (void)connection; (void)sender; (void)path; (void)iface; (void)data;
    if (!strcmp(method, "List")) {
        const gchar *cursor; guint count; gboolean only_unread;
        g_variant_get(parameters, "(&sub)", &cursor, &count, &only_unread);
        list_calls++; last_count = count;
        g_assert_cmpuint(count, >=, 100); g_assert_cmpuint(count, <=, 1000);
        g_assert_cmpuint(count % 100, ==, 0); g_assert_false(only_unread);
        if (*cursor) g_assert_cmpuint(count, ==, 100);
        g_free(last_cursor); last_cursor = g_strdup(cursor);
        if (hold_list) { g_assert_null(pending); pending = g_object_ref(invocation); return; }
        if (fail_list) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.xfce.Notifyd.TestFailure", "Synthetic failure"); return;
        }
        guint start = 0;
        if (*cursor) { g_assert_true(g_str_has_prefix(cursor, "synthetic-")); start = (guint)g_ascii_strtoull(cursor + 10, NULL, 10) + 1; }
        reply_list(invocation, start, count); return;
    }
    if (!strcmp(method, "HasUnread")) {
        unread_calls++; g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", unread_override >= 0 ? unread_override != 0 : rows > 0)); return;
    }
    if (!strcmp(method, "MarkAllRead")) all_calls++;
    else {
        gchar **ids; g_variant_get(parameters, "(^as)", &ids);
        g_assert_cmpuint(g_strv_length(ids), ==, 1);
        g_assert_true(g_str_has_prefix(ids[0], "synthetic-"));
        g_free(last_id); last_id = g_strdup(ids[0]); g_strfreev(ids);
        if (!strcmp(method, "MarkRead")) mark_calls++;
        else if (!strcmp(method, "Delete")) delete_calls++;
        else g_assert_not_reached();
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
}
static const GDBusInterfaceVTable vtable = {method_call, NULL, NULL, {0}};
static void own(gboolean acquire) {
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", acquire ? "RequestName" : "ReleaseName",
        acquire ? g_variant_new("(su)", NAME, 4u) : g_variant_new("(s)", NAME),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(reply);
    guint result; g_variant_get(reply, "(u)", &result); g_variant_unref(reply);
    g_assert_cmpuint(result, ==, 1); spin(80);
}
static GtkWidget *history(void) {
    GList *windows = gtk_window_list_toplevels(); GtkWidget *found = NULL;
    for (GList *l = windows; l; l = l->next)
        if (g_strcmp0(gtk_window_get_title(l->data), "Notifications") == 0) found = l->data;
    g_list_free(windows); return found;
}
static GtkWidget *find_button(GtkWidget *w, const gchar *text) {
    if (GTK_IS_BUTTON(w) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), text) == 0) return w;
    if (!GTK_IS_CONTAINER(w)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w)); GtkWidget *found = NULL;
    for (GList *l = children; l && !found; l = l->next) found = find_button(l->data, text);
    g_list_free(children); return found;
}
static guint summaries(GtkWidget *w) {
    guint count = GTK_IS_LABEL(w) && g_str_has_prefix(gtk_label_get_text(GTK_LABEL(w)), "TEST SUMMARY ");
    if (GTK_IS_CONTAINER(w)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = children; l; l = l->next) count += summaries(l->data);
        g_list_free(children);
    }
    return count;
}
static GtkWidget *find_scroll(GtkWidget *w) {
    if (GTK_IS_SCROLLED_WINDOW(w)) return w;
    if (!GTK_IS_CONTAINER(w)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w)); GtkWidget *found = NULL;
    for (GList *l = children; l && !found; l = l->next) found = find_scroll(l->data);
    g_list_free(children); return found;
}
static GtkWidget *find_label(GtkWidget *w, const gchar *text) {
    if (GTK_IS_LABEL(w) && !strcmp(gtk_label_get_text(GTK_LABEL(w)), text)) return w;
    if (!GTK_IS_CONTAINER(w)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w)); GtkWidget *found = NULL;
    for (GList *l = children; l && !found; l = l->next) found = find_label(l->data, text);
    g_list_free(children); return found;
}
static gdouble row_offset(const char *summary, GtkAdjustment *adjustment) {
    GtkWidget *label = find_label(history(), summary); g_assert_nonnull(label);
    GtkAllocation allocation; gtk_widget_get_allocation(gtk_widget_get_parent(label), &allocation);
    return allocation.y - gtk_adjustment_get_value(adjustment);
}
static GtkWidget *find_label_prefix(GtkWidget *w, const gchar *prefix) {
    if (GTK_IS_LABEL(w) && g_str_has_prefix(gtk_label_get_text(GTK_LABEL(w)), prefix)) return w;
    if (!GTK_IS_CONTAINER(w)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w)); GtkWidget *found = NULL;
    for (GList *l = children; l && !found; l = l->next) found = find_label_prefix(l->data, prefix);
    g_list_free(children); return found;
}
static void assert_badge(NotificationCenter *center, gboolean unread) {
    GtkWidget *button = notification_center_get_button(center);
    gchar *tooltip = gtk_widget_get_tooltip_text(button);
    g_assert_cmpstr(tooltip, ==, unread ? "Notifications: unread items" : "Notifications");
    g_free(tooltip);
    GtkWidget *badge = find_label(button, unread ? "•" : "");
    g_assert_nonnull(badge);
    g_assert_true(GTK_IS_OVERLAY(gtk_widget_get_parent(badge)));
    g_assert_false(gtk_label_get_use_markup(GTK_LABEL(badge)));
    if (!unread) g_assert_null(find_label(button, "•"));
}
static gboolean has_text(GtkWidget *w, const char *needle) {
    if (GTK_IS_LABEL(w) && strstr(gtk_label_get_text(GTK_LABEL(w)), needle)) return TRUE;
    if (!GTK_IS_CONTAINER(w)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w)); gboolean found = FALSE;
    for (GList *l = children; l && !found; l = l->next) found = has_text(l->data, needle);
    g_list_free(children); return found;
}
static void signal_change(const char *name) {
    GError *error = NULL;
    GVariant *args = !strcmp(name, "Cleared") ? NULL :
        !strcmp(name, "Truncated") ? g_variant_new("(u)", rows) : g_variant_new("(s)", "synthetic-0");
    g_dbus_connection_emit_signal(bus, NULL, PATH, IFACE, name, args, &error);
    g_assert_no_error(error);
}
static void click(const char *text) {
    GtkWidget *button = find_button(history(), text); g_assert_nonnull(button);
    g_assert_true(gtk_widget_get_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); spin(450);
}
static void integration(void) {
    GError *error = NULL;
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error); g_assert_no_error(error);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &error); g_assert_no_error(error);
    registration = g_dbus_connection_register_object(bus, PATH, info->interfaces[0], &vtable, NULL, NULL, &error);
    g_assert_no_error(error); g_assert_cmpuint(registration, >, 0);
    NotificationCenter *center = notification_center_new(NULL);
    gtk_button_clicked(GTK_BUTTON(notification_center_get_button(center))); spin(250);
    g_assert_nonnull(history()); g_assert_true(has_text(history(), "service unavailable"));
    g_assert_cmpuint(list_calls, ==, 0);
    own(TRUE); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 3);
    assert_badge(center, TRUE);
    unread_override = 0; signal_change("RowChanged"); spin(450);
    assert_badge(center, FALSE);
    unread_override = 1; signal_change("RowChanged"); spin(450);
    assert_badge(center, TRUE); unread_override = -1;
    g_assert_cmpuint(mark_calls + all_calls + delete_calls, ==, 0);
    g_test_message("absent service recovers; unread tooltip/overlay badge track true/false without marking read");

    guint before = list_calls; rows = 4;
    for (guint i = 0; i < 30; i++) signal_change("RowAdded");
    spin(650); g_assert_cmpuint(summaries(history()), ==, 4);
    g_assert_cmpuint(list_calls - before, <=, 2);
    duplicate = TRUE; signal_change("RowChanged"); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 3);
    duplicate = FALSE;
    g_test_message("signal bursts coalesce and duplicate IDs render once");

    gtk_widget_hide(history()); before = list_calls;
    guint unread_before = unread_calls;
    for (guint i = 0; i < 30; i++) signal_change("RowChanged");
    spin(450); g_assert_cmpuint(list_calls, ==, before);
    g_assert_cmpuint(unread_calls - unread_before, <=, 2);
    gtk_button_clicked(GTK_BUTTON(notification_center_get_button(center))); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 4);
    g_test_message("hidden history avoids List requests while unread badge refreshes stay bounded");

    click("Mark read"); g_assert_cmpuint(mark_calls, ==, 1); g_assert_cmpstr(last_id, ==, "synthetic-0");
    click("Delete"); g_assert_cmpuint(delete_calls, ==, 1); g_assert_cmpstr(last_id, ==, "synthetic-0");
    click("Mark all read"); g_assert_cmpuint(all_calls, ==, 1);
    g_test_message("row read/delete and explicit mark-all deliver exact DBus methods and IDs");

    GString *long_body = g_string_new("<b>literal</b> 한글 ");
    for (guint i = 0; i < 4000; i++) g_string_append(long_body, "가🙂");
    edge_body = g_string_free(long_body, FALSE);
    signal_change("RowChanged"); spin(450);
    GtkWidget *summary_label = find_label(history(), "TEST SUMMARY 0 <b>literal</b> 한글");
    g_assert_nonnull(summary_label); g_assert_false(gtk_label_get_use_markup(GTK_LABEL(summary_label)));
    GtkWidget *body_label = find_label_prefix(history(), "<b>literal</b> 한글 ");
    g_assert_nonnull(body_label); g_assert_false(gtk_label_get_use_markup(GTK_LABEL(body_label)));
    const gchar *body_text = gtk_label_get_text(GTK_LABEL(body_label));
    g_assert_true(g_utf8_validate(body_text, -1, NULL));
    g_assert_cmpint(g_utf8_strlen(body_text, -1), ==, 2000);
    gchar *expected = g_utf8_substring(edge_body, 0, 2000);
    g_assert_cmpstr(body_text, ==, expected); g_free(expected);
    g_clear_pointer(&edge_body, g_free);
    g_test_message("markup remains literal text and long Korean/emoji bodies truncate at 2000 valid UTF8 characters");

    rows = 205; signal_change("RowAdded"); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 100);
    before = list_calls; spin(300); g_assert_cmpuint(list_calls, ==, before);
    click("More (100)"); g_assert_cmpstr(last_cursor, ==, "synthetic-99");
    g_assert_cmpuint(summaries(history()), ==, 200);
    click("More (100)"); g_assert_cmpstr(last_cursor, ==, "synthetic-199");
    g_assert_cmpuint(summaries(history()), ==, 205);
    g_assert_false(gtk_widget_get_sensitive(find_button(history(), "More (100)")));
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(find_scroll(history())));
    gtk_adjustment_set_value(adjustment, 200.0); spin(50);
    signal_change("RowChanged"); spin(500);
    g_assert_cmpuint(summaries(history()), ==, 205); g_assert_cmpuint(last_count, ==, 300);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment), 200.0, 2.0);
    gdouble anchor_offset = row_offset("TEST SUMMARY 1", adjustment);
    prepend = TRUE; rows = 206; signal_change("RowAdded"); spin(500);
    g_assert_cmpuint(summaries(history()), ==, 206);
    g_assert_cmpfloat_with_epsilon(row_offset("TEST SUMMARY 1", adjustment), anchor_offset, 2.0);
    g_assert_cmpfloat(gtk_adjustment_get_value(adjustment), >, 200.0);
    prepend = FALSE;
    g_test_message("refresh preserves expanded depth and the visible row anchor when a new row is prepended");
    rows = 1200; signal_change("RowAdded"); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 300);
    for (guint i = 0; i < 7; i++) click("More (100)");
    g_assert_cmpuint(summaries(history()), ==, 1000);
    g_assert_false(gtk_widget_get_sensitive(find_button(history(), "More (100)")));
    g_test_message("manual pages request 100 rows; refresh depth and rendered rows are bounded at 1000");

    rows = 0; signal_change("Cleared"); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 0); g_assert_true(has_text(history(), "No saved notifications"));
    fail_list = TRUE; signal_change("Truncated"); spin(450);
    g_assert_true(has_text(history(), "unavailable or unsupported"));
    fail_list = FALSE; rows = 2; signal_change("RowAdded"); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 2);
    g_test_message("empty/error states recover on later service signals");

    hold_list = TRUE; signal_change("RowChanged"); spin(350); g_assert_nonnull(pending);
    own(FALSE); g_assert_cmpuint(summaries(history()), ==, 0);
    hold_list = FALSE; own(TRUE); spin(450);
    g_assert_cmpuint(summaries(history()), ==, 2);
    rows = 9; reply_list(pending, 0, 100); g_clear_object(&pending); spin(150);
    g_assert_cmpuint(summaries(history()), ==, 2);
    g_test_message("owner loss/reappearance invalidates delayed replies from the previous generation");

    hold_list = TRUE; signal_change("RowChanged"); spin(350); g_assert_nonnull(pending);
    notification_center_free(center);
    reply_list(pending, 0, 100); g_clear_object(&pending); spin(200);
    g_assert_null(history());
    g_test_message("destroying a center with a pending request safely ignores its late reply");
    own(FALSE); g_dbus_connection_unregister_object(bus, registration);
    g_dbus_node_info_unref(info); g_clear_object(&bus); g_free(last_id); g_free(last_cursor);
}
int main(int argc, char **argv) {
    g_assert_cmpstr(g_getenv("MYPANEL_NOTIFICATION_TEST_BUS"), ==, "private");
    const char *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    g_assert_nonnull(address); g_assert_null(strstr(address, "/run/user/"));
    gtk_test_init(&argc, &argv, NULL);
    g_test_add_func("/notification-center/private-bus-integration", integration);
    return g_test_run();
}
