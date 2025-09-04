/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/modifiers.h>

#include "modifiers.h"

struct modifiers_state {
    uint8_t modifiers;
};

struct modifier_symbol {
    uint8_t modifier;
    const lv_img_dsc_t *symbol_dsc;
    lv_obj_t *symbol;
    lv_obj_t *selection_line;
    bool is_active;
};

LV_IMG_DECLARE(control_icon);
struct modifier_symbol ms_control = {
    .modifier = MOD_LCTL | MOD_RCTL,
    .symbol_dsc = &control_icon,
};

LV_IMG_DECLARE(shift_icon);
struct modifier_symbol ms_shift = {
    .modifier = MOD_LSFT | MOD_RSFT,
    .symbol_dsc = &shift_icon,
};

LV_IMG_DECLARE(alt_icon);
struct modifier_symbol ms_alt = {
    .modifier = MOD_LALT | MOD_RALT,
    .symbol_dsc = &alt_icon,
};

LV_IMG_DECLARE(win_icon);
struct modifier_symbol ms_gui = {
    .modifier = MOD_LGUI | MOD_RGUI,
    .symbol_dsc = &win_icon,
};

LV_IMG_DECLARE(opt_icon);
struct modifier_symbol ms_opt = {
    .modifier = MOD_LALT | MOD_RALT,
    .symbol_dsc = &opt_icon,
};

LV_IMG_DECLARE(cmd_icon);
struct modifier_symbol ms_cmd = {
    .modifier = MOD_LGUI | MOD_RGUI,
    .symbol_dsc = &cmd_icon,
};

/* Fixed priority order (left -> right when 2+ active): control, ui, shift, alt */
struct modifier_symbol *modifier_symbols[] = {
    &ms_control,  /* Ctrl first */
    &ms_gui,      /* UI/GUI second */
    &ms_shift,    /* Shift third */
    &ms_alt       /* Alt/Option fourth */
};

#define NUM_SYMBOLS (sizeof(modifier_symbols) / sizeof(struct modifier_symbol *))

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* ---------- Opacity animations + helpers ---------- */
static void anim_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void hide_ready_cb(lv_anim_t *a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void fade_in(lv_obj_t *obj, uint32_t ms) {
    /* debounce: cancel any in-flight opacity anims */
    lv_anim_del(obj, anim_opa_cb);

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_time(&a, ms);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&a);
}

static void fade_out_and_hide(lv_obj_t *obj, uint32_t ms) {
    /* debounce: cancel any in-flight opacity anims */
    lv_anim_del(obj, anim_opa_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_time(&a, ms);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, lv_obj_get_style_opa(obj, LV_PART_MAIN), LV_OPA_TRANSP);
    lv_anim_set_ready_cb(&a, hide_ready_cb);
    lv_anim_start(&a);
}
/* -------------------------------------------------- */

/* ---------- Y bounce animation ---------- */
static void anim_y_cb(void *var, int32_t v) {
    lv_obj_set_y(var, v);
}

static void move_object_y(void *obj, int32_t from, int32_t to) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_time(&a, 200); /* your timing */
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_values(&a, from, to);
    lv_anim_start(&a);
}
/* ---------------------------------------- */

/* ---------- X slide animation for layout changes ---------- */
static inline int column_x(int col) { return 1 + (SIZE_SYMBOLS + 1) * col; }

static void anim_x_cb(void *var, int32_t v) {
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void move_object_x(void *obj, int32_t from, int32_t to) {
    if (from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_time(&a, 100); /* 100 ms slide */
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_start(&a);
}

/* place both icon and underline at a given column (with X slide) */
static void place_at_col(struct modifier_symbol *ms, int col) {
    int target_x = column_x(col);
    int sx = lv_obj_get_x(ms->symbol);
    int lx = lv_obj_get_x(ms->selection_line);

    move_object_x(ms->symbol, sx, target_x);
    move_object_x(ms->selection_line, lx, target_x);
}
/* ---------------------------------------------------------- */

static void set_modifiers(lv_obj_t *widget, struct modifiers_state state) {
    /* visibility + bounce per symbol */
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        bool mod_is_active = (state.modifiers & modifier_symbols[i]->modifier) > 0;

        if (mod_is_active && !modifier_symbols[i]->is_active) {
            /* debounce opacity anims */
            lv_anim_del(modifier_symbols[i]->symbol, anim_opa_cb);
            lv_anim_del(modifier_symbols[i]->selection_line, anim_opa_cb);

            /* show + animate in */
            fade_in(modifier_symbols[i]->symbol, 140);
            fade_in(modifier_symbols[i]->selection_line, 140);
            move_object_y(modifier_symbols[i]->symbol, 1, 0);
            move_object_y(modifier_symbols[i]->selection_line, SIZE_SYMBOLS + 4, SIZE_SYMBOLS + 2);
            modifier_symbols[i]->is_active = true;

        } else if (!mod_is_active && modifier_symbols[i]->is_active) {
            /* debounce opacity anims */
            lv_anim_del(modifier_symbols[i]->symbol, anim_opa_cb);
            lv_anim_del(modifier_symbols[i]->selection_line, anim_opa_cb);

            /* animate out, then hide */
            move_object_y(modifier_symbols[i]->symbol, 0, 1);
            move_object_y(modifier_symbols[i]->selection_line, SIZE_SYMBOLS + 2, SIZE_SYMBOLS + 4);
            fade_out_and_hide(modifier_symbols[i]->symbol, 140);
            fade_out_and_hide(modifier_symbols[i]->selection_line, 140);
            modifier_symbols[i]->is_active = false;
        }
    }

    /* layout pass: float-left for single active; fixed priority for 2+ */
    int active_count = 0, last_active = -1;
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        if (modifier_symbols[i]->is_active) {
            active_count++;
            last_active = i;
        }
    }

    if (active_count == 1) {
        /* single active -> column 0 */
        place_at_col(modifier_symbols[last_active], 0);
    } else if (active_count >= 2) {
        /* pack by fixed order: control, ui, shift, alt */
        int col = 0;
        for (int i = 0; i < NUM_SYMBOLS; i++) {
            if (modifier_symbols[i]->is_active) {
                place_at_col(modifier_symbols[i], col++);
            }
        }
    }
}

void modifiers_update_cb(struct modifiers_state state) {
    struct zmk_widget_modifiers *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_modifiers(widget->obj, state); }
}

static struct modifiers_state modifiers_get_state(const zmk_event_t *eh) {
    return (struct modifiers_state){
        .modifiers = zmk_hid_get_explicit_mods()
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_modifiers, struct modifiers_state,
                            modifiers_update_cb, modifiers_get_state)

ZMK_SUBSCRIPTION(widget_modifiers, zmk_keycode_state_changed);

int zmk_widget_modifiers_init(struct zmk_widget_modifiers *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, NUM_SYMBOLS * (SIZE_SYMBOLS + 1) + 1, SIZE_SYMBOLS + 3);

    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_width(&style_line, 2);

    static const lv_point_t selection_line_points[] = { {0, 0}, {SIZE_SYMBOLS, 0} };

    for (int i = 0; i < NUM_SYMBOLS; i++) {
        /* icon */
        modifier_symbols[i]->symbol = lv_img_create(widget->obj);
        lv_obj_align(modifier_symbols[i]->symbol, LV_ALIGN_TOP_LEFT, column_x(i), 1);
        lv_img_set_src(modifier_symbols[i]->symbol, modifier_symbols[i]->symbol_dsc);

        /* selection underline */
        modifier_symbols[i]->selection_line = lv_line_create(widget->obj);
        lv_line_set_points(modifier_symbols[i]->selection_line, selection_line_points, 2);
        lv_obj_add_style(modifier_symbols[i]->selection_line, &style_line, 0);
        lv_obj_align_to(modifier_symbols[i]->selection_line, modifier_symbols[i]->symbol, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 3);

        /* start hidden & transparent until pressed */
        lv_obj_set_style_opa(modifier_symbols[i]->symbol, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_opa(modifier_symbols[i]->selection_line, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_flag(modifier_symbols[i]->symbol, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(modifier_symbols[i]->selection_line, LV_OBJ_FLAG_HIDDEN);

        /* inactive baseline positions */
        modifier_symbols[i]->is_active = false;
        lv_obj_set_y(modifier_symbols[i]->symbol, 1);
        lv_obj_set_y(modifier_symbols[i]->selection_line, SIZE_SYMBOLS + 4);
    }

    sys_slist_append(&widgets, &widget->node);
    widget_modifiers_init();
    return 0;
}

lv_obj_t *zmk_widget_modifiers_obj(struct zmk_widget_modifiers *widget) {
    return widget->obj;
}
