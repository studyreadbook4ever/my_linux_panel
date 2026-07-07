#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int text_has_spotify(const char *text) {
    if (!text)
        return 0;

    char lowered[512];
    size_t len = strlen(text);
    if (len >= sizeof(lowered))
        len = sizeof(lowered) - 1;

    for (size_t i = 0; i < len; i++)
        lowered[i] = (char)tolower((unsigned char)text[i]);
    lowered[len] = '\0';

    return strstr(lowered, "spotify") != NULL;
}

static char *read_text_property(Display *dpy, Window win, Atom atom) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, win, atom, 0, 1024, False, AnyPropertyType,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &prop) != Success || !prop)
        return NULL;

    char *copy = calloc(nitems + 1, 1);
    if (copy)
        memcpy(copy, prop, nitems);
    XFree(prop);
    return copy;
}

static int is_spotify_window(Display *dpy, Window win) {
    Atom wm_class = XInternAtom(dpy, "WM_CLASS", False);
    Atom wm_name = XInternAtom(dpy, "WM_NAME", False);
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    char *class_text = read_text_property(dpy, win, wm_class);
    char *name_text = read_text_property(dpy, win, net_wm_name);

    if (!name_text)
        name_text = read_text_property(dpy, win, wm_name);

    int match = text_has_spotify(class_text) || text_has_spotify(name_text);
    free(class_text);
    free(name_text);
    return match;
}

static Window find_spotify_window(Display *dpy, Window root) {
    Atom client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, root, client_list, 0, 4096, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &prop) != Success || !prop)
        return None;

    Window found = None;
    Window *windows = (Window *)prop;
    for (unsigned long i = 0; i < nitems; i++) {
        if (is_spotify_window(dpy, windows[i]))
            found = windows[i];
    }

    XFree(prop);
    return found;
}

static void activate_window(Display *dpy, Window root, Window win) {
    Atom active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = win;
    event.xclient.message_type = active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;

    XMapRaised(dpy, win);
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(dpy);
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fputs("spotify-window-activate: cannot open X display\n", stderr);
        return 2;
    }

    Window root = DefaultRootWindow(dpy);
    Window win = find_spotify_window(dpy, root);
    if (win == None) {
        XCloseDisplay(dpy);
        return 1;
    }

    activate_window(dpy, root, win);
    XCloseDisplay(dpy);
    return 0;
}
