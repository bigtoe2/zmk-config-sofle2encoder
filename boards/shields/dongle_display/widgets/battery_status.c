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
#include <zmk/display/widgets/battery_status.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include "battery_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_battery_state {
    uint8_t source;
    uint8_t level;
};
    
static lv_color_t battery_image_buffer[ZMK_SPLIT_BLE_PERIPHERAL_COUNT][62 * 3];

static void draw_battery(lv_obj_t *canvas, uint8_t level) {
    // Clamp 0..100 then scale to 0..60
    if (level > 100) level = 100;
    uint8_t scaled = (level * 60) / 100;

    // Empty background = black
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    // Fill descriptor (charge in white)
    lv_draw_rect_dsc_t rect_fill_dsc;
    lv_draw_rect_dsc_init(&rect_fill_dsc);
    rect_fill_dsc.bg_color = lv_color_white();
    rect_fill_dsc.border_opa = LV_OPA_TRANSP;

    // Battery tip outline (x = 61, middle pixel only since height = 3)
    lv_canvas_set_px(canvas, 61, 1, lv_color_white());

    // Frame corners (optional)
    lv_canvas_set_px(canvas, 0, 0, lv_color_white());
    lv_canvas_set_px(canvas, 0, 2, lv_color_white());
    lv_canvas_set_px(canvas, 61, 0, lv_color_white());
    lv_canvas_set_px(canvas, 61, 2, lv_color_white());

    // Draw the filled portion from the left
    if (scaled > 0) {
        lv_canvas_draw_rect(canvas, 0, 0, scaled, 3, &rect_fill_dsc);
    }
}


void battery_status_update_cb(struct peripheral_battery_state state) {
    struct zmk_widget_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_symbol(widget->obj, state); }
}

static struct peripheral_battery_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    return (struct peripheral_battery_state){
        .source = ev->source,
        .level = ev->state_of_charge,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct peripheral_battery_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_peripheral_battery_state_changed);

int zmk_widget_peripheral_battery_status_init(struct zmk_widget_peripheral_battery_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);

    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        lv_obj_t *image_canvas = lv_canvas_create(widget->obj);
        lv_obj_t *battery_label = lv_label_create(widget->obj);

        lv_canvas_set_buffer(image_canvas, battery_image_buffer[i], 62, 3, LV_IMG_CF_TRUE_COLOR);

        lv_obj_align(image_canvas, LV_ALIGN_TOP_RIGHT, 0, i * 10);
        lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -7, i * 10);
    }

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();

    return 0;
}

lv_obj_t *zmk_widget_peripheral_battery_status_obj(struct zmk_widget_peripheral_battery_status *widget) {
    return widget->obj;
}
