/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include "output_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

LV_IMG_DECLARE(sym_usb);
LV_IMG_DECLARE(sym_bt);
LV_IMG_DECLARE(sym_ok);
LV_IMG_DECLARE(sym_nok);
LV_IMG_DECLARE(sym_open);
LV_IMG_DECLARE(sym_1);
LV_IMG_DECLARE(sym_2);
LV_IMG_DECLARE(sym_3);
LV_IMG_DECLARE(sym_4);
LV_IMG_DECLARE(sym_5);

const lv_img_dsc_t *sym_num[] = {
    &sym_1,
    &sym_2,
    &sym_3,
    &sym_4,
    &sym_5,
};

enum output_symbol {
    output_symbol_usb,
    output_symbol_usb_hid_status,
    output_symbol_bt,
    output_symbol_bt_number,
    output_symbol_bt_status,
    output_symbol_selection_line
};

/* selection line kept but always hidden now */
static lv_point_t selection_line_points[] = { {-1, 0}, {12, 0} };

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool usb_is_hid_ready;
};

static struct output_status_state get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
        .usb_is_hid_ready = zmk_usb_is_hid_ready()
    };
}

static void set_status_symbol(lv_obj_t *widget, struct output_status_state state) {
    lv_obj_t *usb = lv_obj_get_child(widget, output_symbol_usb);
    lv_obj_t *usb_hid_status = lv_obj_get_child(widget, output_symbol_usb_hid_status);
    lv_obj_t *bt = lv_obj_get_child(widget, output_symbol_bt);
    lv_obj_t *bt_number = lv_obj_get_child(widget, output_symbol_bt_number);
    lv_obj_t *bt_status = lv_obj_get_child(widget, output_symbol_bt_status);
    lv_obj_t *selection_line = lv_obj_get_child(widget, output_symbol_selection_line);

    /* Always hide the selection line (we only show one output now) */
    if (selection_line) {
        lv_obj_add_flag(selection_line, LV_OBJ_FLAG_HIDDEN);
    }

    if (state.selected_endpoint.transport == ZMK_TRANSPORT_USB) {
        /* Show USB; hide BT */
        lv_obj_clear_flag(usb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(usb_hid_status, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(bt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bt_number, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bt_status, LV_OBJ_FLAG_HIDDEN);

        /* Update USB HID status icon */
        lv_img_set_src(usb_hid_status, state.usb_is_hid_ready ? &sym_ok : &sym_nok);

    } else { /* ZMK_TRANSPORT_BLE */
        /* Show BT; hide USB */
        lv_obj_add_flag(usb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(usb_hid_status, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(bt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bt_number, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bt_status, LV_OBJ_FLAG_HIDDEN);

        /* Update BT profile number */
        if (state.active_profile_index >= 0 &&
            state.active_profile_index < (int)(sizeof(sym_num) / sizeof(sym_num[0]))) {
            lv_img_set_src(bt_number, sym_num[state.active_profile_index]);
        } else {
            lv_img_set_src(bt_number, &sym_nok);
        }

        /* Update BT status: bonded + connected? open? */
        if (state.active_profile_bonded) {
            lv_img_set_src(bt_status, state.active_profile_connected ? &sym_ok : &sym_nok);
        } else {
            lv_img_set_src(bt_status, &sym_open);
        }
    }
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_output_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_status_symbol(widget->obj, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);

int zmk_widget_output_status_init(struct zmk_widget_output_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* USB block */
    lv_obj_t *usb = lv_img_create(widget->obj);
    lv_obj_align(usb, LV_ALIGN_TOP_LEFT, 1, 4);
    lv_img_set_src(usb, &sym_usb);

    lv_obj_t *usb_hid_status = lv_img_create(widget->obj);
    lv_obj_align_to(usb_hid_status, usb, LV_ALIGN_BOTTOM_LEFT, 2, -7);

    /* BT block */
    lv_obj_t *bt = lv_img_create(widget->obj);
    lv_obj_align_to(bt, usb, LV_ALIGN_OUT_RIGHT_TOP, 6, 0);
    lv_img_set_src(bt, &sym_bt);

    lv_obj_t *bt_number = lv_img_create(widget->obj);
    lv_obj_align_to(bt_number, bt, LV_ALIGN_OUT_RIGHT_TOP, 2, 7);

    lv_obj_t *bt_status = lv_img_create(widget->obj);
    lv_obj_align_to(bt_status, bt, LV_ALIGN_OUT_RIGHT_TOP, 2, 1);

    /* Selection underline (kept for compatibility but hidden) */
    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_width(&style_line, 2);

    lv_obj_t *selection_line = lv_line_create(widget->obj);
    lv_line_set_points(selection_line, selection_line_points, 2);
    lv_obj_add_style(selection_line, &style_line, 0);
    lv_obj_align_to(selection_line, usb, LV_ALIGN_OUT_TOP_LEFT, 3, -1);
    lv_obj_add_flag(selection_line, LV_OBJ_FLAG_HIDDEN);

    sys_slist_append(&widgets, &widget->node);

    widget_output_status_init();
    return 0;
}

lv_obj_t *zmk_widget_output_status_obj(struct zmk_widget_output_status *widget) {
    return widget->obj;
}
