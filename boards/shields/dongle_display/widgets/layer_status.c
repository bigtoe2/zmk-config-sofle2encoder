/*
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* How far to slide text (pixels) and how long (ms) */
#define LAYER_SLIDE_PX 6
#define LAYER_ANIM_MS 120

struct layer_status_state {
    uint8_t index;
    const char *label;
};

/* --- LVGL anim helpers (opacity + Y) --- */

static void anim_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void anim_y_cb(void *var, int32_t v) {
    lv_obj_set_y((lv_obj_t *)var, v);
}

/* --- text building --- */

static void build_text_for_state(char *out, size_t out_sz, struct layer_status_state s) {
    if (s.label && *s.label) {
        /* Copy the label */
        snprintf(out, out_sz, "%s", s.label);
    } else {
        /* No label set: show index */
        snprintf(out, out_sz, "%u", (unsigned)s.index);
    }
}

/* --- Core: set text with a slide/fade animation when it changes --- */

static void set_layer_symbol(lv_obj_t *label, struct layer_status_state state) {
    static char current_text[16] = {0};
    char next_text[16] = {0};
    build_text_for_state(next_text, sizeof(next_text), state);

    /* First-time init: no animation */
    if (current_text[0] == '\0') {
        lv_label_set_text(label, next_text);
        lv_obj_set_style_opa(label, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_y(label, 0);
        strncpy(current_text, next_text, sizeof(current_text) - 1);
        return;
    }

    /* If unchanged, do nothing */
    if (strncmp(current_text, next_text, sizeof(current_text)) == 0) {
        return;
    }

    /* Cancel any in-flight animations on this label */
    lv_anim_del(label, anim_opa_cb);
    lv_anim_del(label, anim_y_cb);

    /* OUT phase: slide up & fade out */
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, label);
        lv_anim_set_time(&a, LAYER_ANIM_MS);
        lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_values(&a, 0, -LAYER_SLIDE_PX);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, label);
        lv_anim_set_time(&a, LAYER_ANIM_MS);
        lv_anim_set_exec_cb(&a, anim_opa_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    /* After OUT completes, swap text, jump below, then animate IN */
    /* We piggyback on a timer to sequence the IN phase cleanly. */
    struct anim_ctx {
        lv_obj_t *label;
        char text[16];
    };
    static struct anim_ctx ctx; /* single instance needed (only one widget typically) */
    ctx.label = label;
    strncpy(ctx.text, next_text, sizeof(ctx.text) - 1);

    lv_timer_t *t = lv_timer_create_basic();
    lv_timer_set_period(t, LAYER_ANIM_MS + 1); /* run once right after OUT finishes */
    lv_timer_set_repeat_count(t, 1);
    lv_timer_set_user_data(t, &ctx);
    lv_timer_set_cb(t, [](lv_timer_t *timer) {
        struct anim_ctx *c = (struct anim_ctx *)lv_timer_get_user_data(timer);
        lv_obj_t *lbl = c->label;

        /* Swap text and reset starting pose (below, transparent) */
        lv_label_set_text(lbl, c->text);
        lv_obj_set_y(lbl, LAYER_SLIDE_PX);
        lv_obj_set_style_opa(lbl, LV_OPA_TRANSP, LV_PART_MAIN);

        /* IN phase: slide up to 0 & fade in */
        lv_anim_t a1;
        lv_anim_init(&a1);
        lv_anim_set_var(&a1, lbl);
        lv_anim_set_time(&a1, LAYER_ANIM_MS);
        lv_anim_set_exec_cb(&a1, anim_y_cb);
        lv_anim_set_values(&a1, LAYER_SLIDE_PX, 0);
        lv_anim_set_path_cb(&a1, lv_anim_path_ease_in_out);
        lv_anim_start(&a1);

        lv_anim_t a2;
        lv_anim_init(&a2);
        lv_anim_set_var(&a2, lbl);
        lv_anim_set_time(&a2, LAYER_ANIM_MS);
        lv_anim_set_exec_cb(&a2, anim_opa_cb);
        lv_anim_set_values(&a2, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_path_cb(&a2, lv_anim_path_ease_in_out);
        lv_anim_start(&a2);

        /* Done with this timer */
        lv_timer_del(timer);
    });

    /* Update our "current" text snapshot */
    strncpy(current_text, next_text, sizeof(current_text) - 1);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_layer_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_layer_symbol(widget->obj, state);
    }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    uint8_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(index),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state,
                            layer_status_update_cb, layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);

    /* Start fully opaque at baseline */
    lv_obj_set_style_opa(widget->obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_y(widget->obj, 0);

    sys_slist_append(&widgets, &widget->node);

    widget_layer_status_init();
    return 0;
}

lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget) {
    return widget->obj;
}
