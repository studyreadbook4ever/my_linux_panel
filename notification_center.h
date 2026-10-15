#ifndef NOTIFICATION_CENTER_H
#define NOTIFICATION_CENTER_H
#include <gtk/gtk.h>
typedef struct NotificationCenter NotificationCenter;
/* Borrowed button; the center owns it. Call free before releasing the parent.
 */
NotificationCenter *notification_center_new(GtkWindow *parent);
GtkWidget *notification_center_get_button(NotificationCenter *center);
void notification_center_free(NotificationCenter *center);
#endif
