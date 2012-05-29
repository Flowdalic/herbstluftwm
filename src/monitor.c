/** Copyright 2011-2012 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "globals.h"
#include "ipc-protocol.h"
#include "utils.h"
#include "mouse.h"
#include "hook.h"
#include "layout.h"
#include "tag.h"
#include "ewmh.h"
#include "monitor.h"
#include "settings.h"

int* g_monitors_locked;
int* g_swap_monitors_to_get_tag;

typedef struct RectList {
    XRectangle rect;
    struct RectList* next;
} RectList;

static RectList* reclist_insert_disjoint(RectList* head, RectList* mt);

void monitor_init() {
    g_monitors_locked = &(settings_find("monitors_locked")->value.i);
    g_cur_monitor = 0;
    g_monitors = g_array_new(false, false, sizeof(HSMonitor));
    g_swap_monitors_to_get_tag = &(settings_find("swap_monitors_to_get_tag")->value.i);
}

void monitor_destroy() {
    g_array_free(g_monitors, true);
}


void monitor_apply_layout(HSMonitor* monitor) {
    if (monitor) {
        if (*g_monitors_locked) {
            monitor->dirty = true;
            return;
        }
        monitor->dirty = false;
        XRectangle rect = monitor->rect;
        // apply pad
        rect.x += monitor->pad_left;
        rect.width -= (monitor->pad_left + monitor->pad_right);
        rect.y += monitor->pad_up;
        rect.height -= (monitor->pad_up + monitor->pad_down);
        // apply window gap
        rect.x += *g_window_gap;
        rect.y += *g_window_gap;
        rect.height -= *g_window_gap;
        rect.width -= *g_window_gap;
        if (monitor->tag->floating) {
            frame_apply_floating_layout(monitor->tag->frame, monitor);
        } else {
            frame_apply_layout(monitor->tag->frame, rect);
        }
        if (get_current_monitor() == monitor) {
            frame_focus_recursive(monitor->tag->frame);
        }
        // remove all enternotify-events from the event queue that were
        // generated while arranging the clients on this monitor
        XEvent ev;
        XSync(g_display, False);
        while(XCheckMaskEvent(g_display, EnterWindowMask, &ev));
    }
}

int list_monitors(int argc, char** argv, GString** output) {
    (void)argc;
    (void)argv;
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* monitor = &g_array_index(g_monitors, HSMonitor, i);
        g_string_append_printf(*output, "%d: %dx%d%+d%+d with tag \"%s\"%s\n",
            i,
            monitor->rect.width, monitor->rect.height,
            monitor->rect.x, monitor->rect.y,
            monitor->tag ? monitor->tag->name->str : "???",
            (g_cur_monitor == i) ? " [FOCUS]" : "");
    }
    return 0;
}

static bool rects_intersect(RectList* m1, RectList* m2) {
    XRectangle *r1 = &m1->rect, *r2 = &m2->rect;
    bool is = TRUE;
    is = is && intervals_intersect(r1->x, r1->x + r1->width,
                                   r2->x, r2->x + r2->width);
    is = is && intervals_intersect(r1->y, r1->y + r1->height,
                                   r2->y, r2->y + r2->height);
    return is;
}

static XRectangle intersection_area(RectList* m1, RectList* m2) {
    XRectangle r; // intersection between m1->rect and m2->rect
    r.x = MAX(m1->rect.x, m2->rect.x);
    r.y = MAX(m1->rect.y, m2->rect.y);
    // the bottom right coordinates of the rects
    int br1_x = m1->rect.x + m1->rect.width;
    int br1_y = m1->rect.y + m1->rect.height;
    int br2_x = m2->rect.x + m2->rect.width;
    int br2_y = m2->rect.y + m2->rect.height;
    r.width = MIN(br1_x, br2_x) - r.x;
    r.height = MIN(br1_y, br2_y) - r.y;
    return r;
}

static RectList* rectlist_create_simple(int x1, int y1, int x2, int y2) {
    if (x1 >= x2 || y1 >= y2) {
        return NULL;
    }
    RectList* r = g_new0(RectList, 1);
    r->rect.x = x1;
    r->rect.y = y1;
    r->rect.width  = x2 - x1;
    r->rect.height = y2 - y1;
    r->next = NULL;
    return r;
}

static RectList* insert_rect_border(RectList* head,
                                    XRectangle large, XRectangle center)
{
    // given a large rectangle and a center which guaranteed to be a subset of
    // the large rect, the task is to split "large" into pieces and insert them
    // like this:
    //
    // +------- large ---------+
    // |         top           |
    // |------+--------+-------|
    // | left | center | right |
    // |------+--------+-------|
    // |        bottom         |
    // +-----------------------+
    RectList *top, *left, *right, *bottom;
    // coordinates of the bottom right corner of large
    int br_x = large.x + large.width, br_y = large.y + large.height;
    RectList* (*r)(int,int,int,int) = rectlist_create_simple;
    top   = r(large.x, large.y, large.x + large.width, center.y);
    left  = r(large.x, center.y, center.x, center.y + center.height);
    right = r(center.x + center.width, center.y, br_x, center.y + center.height);
    bottom= r(large.x, center.y + center.height, br_x, br_y);

    RectList* parts[] = { top, left, right, bottom };
    for (int i = 0; i < LENGTH(parts); i++) {
        head = reclist_insert_disjoint(head, parts[i]);
    }
    return head;
}

// insert a new element without any intersections into the given list
RectList* reclist_insert_disjoint(RectList* head, RectList* element) {
    if (!element) {
        return head;
    } else if (!head) {
        // if the list is empty, then intersection-free insertion is trivial
        element->next = NULL;
        return element;
    } else if (!rects_intersect(head, element)) {
        head->next = reclist_insert_disjoint(head->next, element);
        return head;
    } else {
        // element intersects with the head rect
        XRectangle center = intersection_area(head, element);
        XRectangle large = head->rect;
        head->rect = center;
        head->next = insert_rect_border(head->next, large, center);
        head->next = insert_rect_border(head->next, element->rect, center);
        g_free(element);
        return head;
    }
}

static void rectlist_free(RectList* head) {
    if (!head) return;
    RectList* next = head->next;
    g_free(head);
    rectlist_free(next);
}

static size_t rectlist_len(RectList* head) {
    if (!head) return 0;
    return 1 + rectlist_len(head->next);
}

static void rectlist_to_array(RectList* head, XRectangle* rects) {
    if (!head) return;
    *rects = head->rect;
    rectlist_to_array(head->next, rects + 1);
}

static RectList* disjoin_rects(XRectangle* buf, size_t count) {
    RectList* cur;
    struct RectList* rects = NULL;
    for (int i = 0; i < count; i++) {
        cur = g_new0(RectList, 1);
        cur->rect = buf[i];
        rects = reclist_insert_disjoint(rects, cur);
    }
    return rects;
}


int disjoin_rects_command(int argc, char** argv, GString** output) {
    (void)SHIFT(argc, argv);
    if (argc < 1) {
        g_string_append_printf(*output, "At least one rect is required.\n");
        return HERBST_INVALID_ARGUMENT;
    }
    XRectangle* buf = g_new(XRectangle, argc);
    for (int i = 0; i < argc; i++) {
        buf[i] = parse_rectangle(argv[i]);
    }

    RectList* rects = disjoin_rects(buf, argc);
    for (RectList* cur = rects; cur; cur = cur->next) {
        XRectangle r = cur->rect;
        g_string_append_printf(*output, "%dx%d%+d%+d\n",
            r.width, r.height, r.x, r.y);
    }
    rectlist_free(rects);
    g_free(buf);
    return 0;
}

int set_monitor_rects_command(int argc, char** argv, GString** output) {
    (void)SHIFT(argc, argv);
    if (argc < 1) {
        g_string_append_printf(*output, "At least one monitor is required.\n");
        return HERBST_INVALID_ARGUMENT;
    }
    XRectangle* templates = g_new0(XRectangle, argc);
    for (int i = 0; i < argc; i++) {
        templates[i] = parse_rectangle(argv[i]);
    }
    int status = set_monitor_rects(templates, argc);
    g_free(templates);
    return status;
}

int set_monitor_rects(XRectangle* templates, size_t count) {
    if (count < 1) {
        return HERBST_INVALID_ARGUMENT;
    }
    HSTag* tag = NULL;
    int i;
    for (i = 0; i < MIN(count, g_monitors->len); i++) {
        HSMonitor* m = monitor_with_index(i);
        m->rect = templates[i];
    }
    // add aditional monitors
    for (; i < count; i++) {
        tag = find_unused_tag();
        if (!tag) {
            return HERBST_TAG_IN_USE;
        }
        add_monitor(templates[i], tag);
        frame_show_recursive(tag->frame);
    }
    // remove monitors if there are too much
    while (i < g_monitors->len) {
        remove_monitor(i);
    }
    all_monitors_apply_layout();
    return 0;
}

HSMonitor* add_monitor(XRectangle rect, HSTag* tag) {
    assert(tag != NULL);
    HSMonitor m;
    memset(&m, 0, sizeof(m));
    m.rect = rect;
    m.tag = tag;
    m.mouse.x = 0;
    m.mouse.y = 0;
    m.dirty = true;
    g_array_append_val(g_monitors, m);
    return &g_array_index(g_monitors, HSMonitor, g_monitors->len-1);
}

int add_monitor_command(int argc, char** argv) {
    // usage: add_monitor RECTANGLE TAG [PADUP [PADRIGHT [PADDOWN [PADLEFT]]]]
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }
    XRectangle rect = parse_rectangle(argv[1]);
    HSTag* tag = NULL;
    if (argc == 2 || !strcmp("", argv[2])) {
        tag = find_unused_tag();
        if (!tag) {
            return HERBST_TAG_IN_USE;
        }
    }
    else {
        tag = find_tag(argv[2]);
        if (!tag) {
            return HERBST_INVALID_ARGUMENT;
        }
    }
    if (find_monitor_with_tag(tag)) {
        return HERBST_TAG_IN_USE;
    }
    HSMonitor* monitor = add_monitor(rect, tag);
    if (argc > 3 && argv[3][0] != '\0') monitor->pad_up       = atoi(argv[3]);
    if (argc > 4 && argv[4][0] != '\0') monitor->pad_right    = atoi(argv[4]);
    if (argc > 5 && argv[5][0] != '\0') monitor->pad_down     = atoi(argv[5]);
    if (argc > 6 && argv[6][0] != '\0') monitor->pad_left     = atoi(argv[6]);
    frame_show_recursive(tag->frame);
    monitor_apply_layout(monitor);
    emit_tag_changed(tag, g_monitors->len - 1);
    return 0;
}

int remove_monitor_command(int argc, char** argv) {
    // usage: remove_monitor INDEX
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }
    int index = atoi(argv[1]);
    return remove_monitor(index);
}

int remove_monitor(int index) {
    if (index < 0 || index >= g_monitors->len) {
        return HERBST_INVALID_ARGUMENT;
    }
    if (g_monitors->len <= 1) {
        return HERBST_FORBIDDEN;
    }
    HSMonitor* monitor = monitor_with_index(index);
    // adjust selection
    if (g_cur_monitor > index) {
        // same monitor shall be selected after remove
        g_cur_monitor--;
    }
    assert(monitor->tag);
    assert(monitor->tag->frame);
    // hide clients
    frame_hide_recursive(monitor->tag->frame);
    // and remove monitor completly
    g_array_remove_index(g_monitors, index);
    if (g_cur_monitor >= g_monitors->len) {
        g_cur_monitor--;
        // if selection has changed, then relayout focused monitor
        monitor_apply_layout(get_current_monitor());
    }
    return 0;
}

int move_monitor_command(int argc, char** argv) {
    // usage: move_monitor INDEX RECT [PADUP [PADRIGHT [PADDOWN [PADLEFT]]]]
    // moves monitor with number to RECT
    if (argc < 3) {
        return HERBST_INVALID_ARGUMENT;
    }
    int index = atoi(argv[1]);
    if (index < 0 || index >= g_monitors->len) {
        return HERBST_INVALID_ARGUMENT;
    }
    XRectangle rect = parse_rectangle(argv[2]);
    if (rect.width < WINDOW_MIN_WIDTH || rect.height < WINDOW_MIN_HEIGHT) {
        return HERBST_INVALID_ARGUMENT;
    }
    // else: just move it:
    HSMonitor* monitor = &g_array_index(g_monitors, HSMonitor, index);
    monitor->rect = rect;
    if (argc > 3 && argv[3][0] != '\0') monitor->pad_up       = atoi(argv[3]);
    if (argc > 4 && argv[4][0] != '\0') monitor->pad_right    = atoi(argv[4]);
    if (argc > 5 && argv[5][0] != '\0') monitor->pad_down     = atoi(argv[5]);
    if (argc > 6 && argv[6][0] != '\0') monitor->pad_left     = atoi(argv[6]);
    monitor_apply_layout(monitor);
    return 0;
}

int monitor_rect_command(int argc, char** argv, GString** result) {
    // usage: monitor_rect [[-p] INDEX]
    char* index_str = NULL;
    HSMonitor* m = NULL;
    bool with_pad = false;

    // if index is supplied
    if (argc > 1) {
        index_str = argv[1];
    }
    // if -p is supplied
    if (argc > 2) {
        index_str = argv[2];
        if (!strcmp("-p", argv[1])) {
            with_pad = true;
        } else {
            fprintf(stderr, "monitor_rect_command: invalid argument \"%s\"\n",
                    argv[1]);
            return HERBST_INVALID_ARGUMENT;
        }
    }
    // if an index is set
    if (index_str) {
        int index;
        if (1 == sscanf(index_str, "%d", &index)) {
            m = monitor_with_index(index);
            if (!m) {
                fprintf(stderr,"monitor_rect_command: invalid index \"%s\"\n",
                        index_str);
                return HERBST_INVALID_ARGUMENT;
            }
        }
    }

    if (!m) {
        m = get_current_monitor();
    }
    XRectangle rect = m->rect;
    if (with_pad) {
        rect.x += m->pad_left;
        rect.width -= m->pad_left + m->pad_right;
        rect.y += m->pad_up;
        rect.height -= m->pad_up + m->pad_down;
    }
    g_string_printf(*result, "%d %d %d %d",
                    rect.x, rect.y, rect.width, rect.height);
    return 0;
}

int monitor_set_pad_command(int argc, char** argv) {
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }
    int index = atoi(argv[1]);
    if (index < 0 || index >= g_monitors->len) {
        return HERBST_INVALID_ARGUMENT;
    }
    HSMonitor* monitor = &g_array_index(g_monitors, HSMonitor, index);
    if (argc > 2 && argv[2][0] != '\0') monitor->pad_up       = atoi(argv[2]);
    if (argc > 3 && argv[3][0] != '\0') monitor->pad_right    = atoi(argv[3]);
    if (argc > 4 && argv[4][0] != '\0') monitor->pad_down     = atoi(argv[4]);
    if (argc > 5 && argv[5][0] != '\0') monitor->pad_left     = atoi(argv[5]);
    monitor_apply_layout(monitor);
    return 0;
}

HSMonitor* find_monitor_with_tag(HSTag* tag) {
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = &g_array_index(g_monitors, HSMonitor, i);
        if (m->tag == tag) {
            return m;
        }
    }
    return NULL;
}

void ensure_monitors_are_available() {
    if (g_monitors->len > 0) {
        // nothing to do
        return;
    }
    // add monitor if necessary
    XRectangle rect = {
        .x = 0, .y = 0,
        .width = DisplayWidth(g_display, DefaultScreen(g_display)),
        .height = DisplayHeight(g_display, DefaultScreen(g_display)),
    };
    ensure_tags_are_available();
    // add monitor with first tag
    HSMonitor* m = add_monitor(rect, g_array_index(g_tags, HSTag*, 0));
    g_cur_monitor = 0;
    g_cur_frame = m->tag->frame;
}

HSMonitor* monitor_with_frame(HSFrame* frame) {
    // find toplevel Frame
    while (frame->parent) {
        frame = frame->parent;
    }
    HSTag* tag = find_tag_with_toplevel_frame(frame);
    return find_monitor_with_tag(tag);
}

HSMonitor* get_current_monitor() {
    return &g_array_index(g_monitors, HSMonitor, g_cur_monitor);
}

void all_monitors_apply_layout() {
    for (int i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = &g_array_index(g_monitors, HSMonitor, i);
        monitor_apply_layout(m);
    }
}

void monitor_set_tag(HSMonitor* monitor, HSTag* tag) {
    HSMonitor* other = find_monitor_with_tag(tag);
    if (monitor == other) {
        // nothing to do
        return;
    }
    if (other != NULL) {
        if (*g_swap_monitors_to_get_tag) {
            // swap tags
            other->tag = monitor->tag;
            monitor->tag = tag;
            // reset focus
            frame_focus_recursive(tag->frame);
            monitor_apply_layout(other);
            monitor_apply_layout(monitor);
            ewmh_update_current_desktop();
            emit_tag_changed(other->tag, other - (HSMonitor*)g_monitors->data);
            emit_tag_changed(tag, g_cur_monitor);
        }
        return;
    }
    HSTag* old_tag = monitor->tag;
    // 1. show new tag
    monitor->tag = tag;
    // first reset focus and arrange windows
    frame_focus_recursive(tag->frame);
    monitor_apply_layout(monitor);
    // then show them (should reduce flicker)
    frame_show_recursive(tag->frame);
    // 2. hide old tag
    frame_hide_recursive(old_tag->frame);
    // focus window just has been shown
    // focus again to give input focus
    frame_focus_recursive(tag->frame);
    // discard enternotify-events
    XEvent ev;
    XSync(g_display, False);
    while (XCheckMaskEvent(g_display, EnterWindowMask, &ev));
    ewmh_update_current_desktop();
    emit_tag_changed(tag, g_cur_monitor);
}

int monitor_set_tag_command(int argc, char** argv) {
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }
    HSMonitor* monitor = get_current_monitor();
    HSTag*  tag = find_tag(argv[1]);
    if (monitor && tag) {
        monitor_set_tag(get_current_monitor(), tag);
    }
    return 0;
}

int monitor_set_tag_by_index_command(int argc, char** argv) {
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }
    bool skip_visible = false;
    if (argc >= 3 && !strcmp(argv[2], "--skip-visible")) {
        skip_visible = true;
    }
    HSTag* tag = get_tag_by_index(argv[1], skip_visible);
    if (!tag) {
        return HERBST_INVALID_ARGUMENT;
    }
    monitor_set_tag(get_current_monitor(), tag);
    return 0;
}

int monitor_focus_command(int argc, char** argv) {
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }
    int new_selection = atoi(argv[1]);
    // really change selection
    monitor_focus_by_index(new_selection);
    return 0;
}

int monitor_cycle_command(int argc, char** argv) {
    int delta = 1;
    int count = g_monitors->len;
    if (argc >= 2) {
        delta = atoi(argv[1]);
    }
    int new_selection = g_cur_monitor + delta;
    // fix range of index
    new_selection %= count;
    new_selection += count;
    new_selection %= count;
    // really change selection
    monitor_focus_by_index(new_selection);
    return 0;
}

int monitor_index_of(HSMonitor* monitor) {
    return monitor - (HSMonitor*)g_monitors->data;
}

void monitor_focus_by_index(int new_selection) {
    new_selection = CLAMP(new_selection, 0, g_monitors->len - 1);
    HSMonitor* old = &g_array_index(g_monitors, HSMonitor, g_cur_monitor);
    HSMonitor* monitor = &g_array_index(g_monitors, HSMonitor, new_selection);
    if (old == monitor) {
        // nothing to do
        return;
    }
    // change selection globals
    assert(monitor->tag);
    assert(monitor->tag->frame);
    g_cur_monitor = new_selection;
    frame_focus_recursive(monitor->tag->frame);
    // repaint monitors
    monitor_apply_layout(old);
    monitor_apply_layout(monitor);
    int rx, ry;
    {
        // save old mouse position
        Window win, child;
        int wx, wy;
        unsigned int mask;
        if (True == XQueryPointer(g_display, g_root, &win, &child,
            &rx, &ry, &wx, &wy, &mask)) {
            old->mouse.x = rx - old->rect.x;
            old->mouse.y = ry - old->rect.y;
            old->mouse.x = CLAMP(old->mouse.x, 0, old->rect.width-1);
            old->mouse.y = CLAMP(old->mouse.y, 0, old->rect.height-1);
        }
    }
    // restore position of new monitor
    // but only if mouse pointer is not already on new monitor
    int new_x, new_y;
    if ((monitor->rect.x <= rx) && (rx < monitor->rect.x + monitor->rect.width)
        && (monitor->rect.y <= ry) && (ry < monitor->rect.y + monitor->rect.height)) {
        // mouse already is on new monitor
    } else {
        new_x = monitor->rect.x + monitor->mouse.x;
        new_y = monitor->rect.y + monitor->mouse.y;
        XWarpPointer(g_display, None, g_root, 0, 0, 0, 0, new_x, new_y);
        // discard all mouse events caused by this pointer movage from the
        // event queue, so the focus really stays in the last focused window on
        // this monitor and doesn't jump to the window hovered by the mouse
        XEvent ev;
        XSync(g_display, False);
        while(XCheckMaskEvent(g_display, EnterWindowMask, &ev));
    }
    // emit hooks
    ewmh_update_current_desktop();
    emit_tag_changed(monitor->tag, new_selection);
}

int monitor_get_relative_x(HSMonitor* m, int x_root) {
    return x_root - m->rect.x - m->pad_left;
}

int monitor_get_relative_y(HSMonitor* m, int y_root) {
    return y_root - m->rect.y - m->pad_up;
}

HSMonitor* monitor_with_coordinate(int x, int y) {
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = &g_array_index(g_monitors, HSMonitor, i);
        if (m->rect.x + m->pad_left <= x
            && m->rect.x + m->rect.width - m->pad_right > x
            && m->rect.y + m->pad_up <= y
            && m->rect.y + m->rect.height - m->pad_down > y) {
            return m;
        }
    }
    return NULL;
}

HSMonitor* monitor_with_index(int index) {
    if (index < 0 || index >= g_monitors->len) {
        return NULL;
    }
    return &g_array_index(g_monitors, HSMonitor, index);
}

int monitors_lock_command(int argc, char** argv) {
    // lock-number must never be negative
    // ensure that lock value is valid
    if (*g_monitors_locked < 0) {
        *g_monitors_locked = 0;
    }
    // increase lock => it is definitely > 0, i.e. locked
    (*g_monitors_locked)++;
    monitors_lock_changed();
    return 0;
}

int monitors_unlock_command(int argc, char** argv) {
    // lock-number must never be lower than 1 if unlocking
    // so: ensure that lock value is valid
    if (*g_monitors_locked < 1) {
        *g_monitors_locked = 1;
    }
    // decrease lock => unlock
    (*g_monitors_locked)--;
    monitors_lock_changed();
    return 0;
}

void monitors_lock_changed() {
    if (*g_monitors_locked < 0) {
        *g_monitors_locked = 0;
        HSDebug("fixing invalid monitors_locked value to 0\n");
    }
    if (!*g_monitors_locked) {
        // if not locked anymore, then repaint all the dirty monitors
        for (int i = 0; i < g_monitors->len; i++) {
            HSMonitor* m = &g_array_index(g_monitors, HSMonitor, i);
            if (m->dirty) {
                monitor_apply_layout(m);
            }
        }
    }
}

