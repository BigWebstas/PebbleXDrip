/* xDrip CGM - a quick-launch glucose viewer for Pebble.
 *
 * Data comes from the phone (PebbleKit JS) which polls the xDrip+ local
 * web server. This app is NOT a watchface: open it from a quick-launch
 * button when you want a glucose glance, so any watchface can stay set.
 */

#include <pebble.h>

static Window *s_window;
static Layer *s_canvas;
static GPath *s_arrow;
static AppTimer *s_refresh_timer;
static AppTimer *s_dirty_timer;

// Latest state from the phone.
static int   s_sgv = 0;          // mg/dL, 0 = none yet
static int   s_delta = 0;        // mg/dL
static int   s_trend = 0;        // 1..7, 0 = unknown
static int   s_age = 0;          // seconds old, as reported by the phone
static time_t s_last_rx = 0;     // when we received it
static int   s_err = 0;          // last error code, 0 = none
static bool  s_have_data = false;

static uint8_t s_hist[24];       // oldest-first, each byte = mg/dL / 2
static int   s_hist_count = 0;

// Arrow shape pointing right (+x). Rotated per trend.
static const GPathInfo ARROW_PATH = {
  .num_points = 7,
  .points = (GPoint[]) {
    {-11, -3}, {3, -3}, {3, -8}, {12, 0}, {3, 8}, {3, 3}, {-11, 3}
  }
};

// ---------------------------------------------------------------------------

static void request_refresh(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint8(out, MESSAGE_KEY_ERR, 0);   // any message = "please refresh"
    app_message_outbox_send();
  }
}

static int effective_age(void) {
  if (!s_have_data) return 0;
  return s_age + (int)(time(NULL) - s_last_rx);
}

static bool is_stale(void) {
  return !s_have_data || effective_age() > 720;   // > 12 min
}

static GColor sgv_color(int sgv) {
#if defined(PBL_COLOR)
  if (sgv <= 0)   return GColorLightGray;
  if (sgv < 70)   return GColorRed;
  if (sgv > 180)  return GColorYellow;
  return GColorGreen;
#else
  return GColorWhite;
#endif
}

// ---------------------------------------------------------------------------

static void draw_arrow(GContext *ctx, GPoint center, int trend, GColor color) {
  if (trend < 1 || trend > 7) {
    // Unknown trend: a question mark instead.
    graphics_context_set_text_color(ctx, color);
    graphics_draw_text(ctx, "?", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                       GRect(center.x - 14, center.y - 18, 28, 30),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  int deg;
  switch (trend) {
    case 1: case 2: deg = -90; break;   // up / double up
    case 3:         deg = -45; break;
    case 4:         deg =   0; break;
    case 5:         deg =  45; break;
    case 6: case 7: deg =  90; break;   // down / double down
    default:        deg =   0; break;
  }
  bool dbl = (trend == 1 || trend == 7);

  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_color(ctx, color);

  gpath_rotate_to(s_arrow, DEG_TO_TRIGANGLE(deg));

  if (dbl) {
    // Two arrowheads stacked along the travel direction.
    int dx = (deg == 0) ? 6 : 0;
    int dy = (deg == 0) ? 0 : 6;
    gpath_move_to(s_arrow, GPoint(center.x - dx, center.y - dy));
    gpath_draw_filled(ctx, s_arrow);
    gpath_move_to(s_arrow, GPoint(center.x + dx, center.y + dy));
    gpath_draw_filled(ctx, s_arrow);
  } else {
    gpath_move_to(s_arrow, center);
    gpath_draw_filled(ctx, s_arrow);
  }
}

static void draw_graph(GContext *ctx, GRect r) {
  const int lo = 40, hi = 260;      // vertical scale, mg/dL
  #define Y_FOR(v) (r.origin.y + r.size.h - \
    (((v) < lo ? lo : ((v) > hi ? hi : (v))) - lo) * r.size.h / (hi - lo))

  int yb1 = Y_FOR(180), yb2 = Y_FOR(70);

#if defined(PBL_COLOR)
  // Whole plot area is grey, target band a shade lighter.
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, r, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(r.origin.x, yb1, r.size.w, yb2 - yb1), 0, GCornerNone);
#else
  graphics_context_set_stroke_color(ctx, GColorWhite);
  for (int x = r.origin.x; x < r.origin.x + r.size.w; x += 4) {
    graphics_draw_pixel(ctx, GPoint(x, yb1));
    graphics_draw_pixel(ctx, GPoint(x, yb2));
  }
#endif

  if (s_hist_count < 2) return;

  int n = s_hist_count;
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  for (int i = 0; i < n; i++) {
    int v = s_hist[i] * 2;
    int x = r.origin.x + (r.size.w - 1) * i / (n - 1);
    int y = Y_FOR(v);
    graphics_fill_circle(ctx, GPoint(x, y), 2);
  }
  #undef Y_FOR
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  const int top = PBL_IF_ROUND_ELSE(22, 4);
  char buf[16];

  // --- big glucose number ------------------------------------------------
  GColor col = sgv_color(s_sgv);
  if (s_err && !s_have_data) {
    col = PBL_IF_COLOR_ELSE(GColorRed, GColorWhite);
    strncpy(buf, "---", sizeof(buf));
  } else if (s_sgv <= 0) {
    strncpy(buf, "...", sizeof(buf));
  } else {
    snprintf(buf, sizeof(buf), "%d", s_sgv);
  }
  if (is_stale() && s_have_data) col = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);

  graphics_context_set_text_color(ctx, col);
  // ROBOTO_BOLD_SUBSET_49 is the largest stock font: 49px, digits + ':' only.
  // For "LOW"/"HIGH"/"---" (letters) fall back to the largest full font.
  bool digits_only = (buf[0] >= '0' && buf[0] <= '9');
  GFont big = fonts_get_system_font(digits_only ? FONT_KEY_ROBOTO_BOLD_SUBSET_49
                                                : FONT_KEY_BITHAM_42_BOLD);
  graphics_draw_text(ctx, buf, big,
                     GRect(0, top - 4, b.size.w, 54),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // --- trend arrow, left of the number ---------------------------------
  if (s_have_data && s_sgv > 0) {
    draw_arrow(ctx, GPoint(PBL_IF_ROUND_ELSE(30, 15), top + 26), s_trend, col);
  }

  // --- delta + age line ------------------------------------------------
  char line[32];
  if (s_err && !s_have_data) {
    snprintf(line, sizeof(line), "no data (e%d)", s_err);
  } else if (!s_have_data) {
    strncpy(line, "waiting...", sizeof(line));
  } else {
    int mins = effective_age() / 60;
    char agebuf[12];
    if (mins <= 0)        strncpy(agebuf, "now", sizeof(agebuf));
    else if (mins > 99)   strncpy(agebuf, ">99m", sizeof(agebuf));
    else                  snprintf(agebuf, sizeof(agebuf), "%dm", mins);
    snprintf(line, sizeof(line), "%+d   %s", s_delta, agebuf);
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, line, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, top + 50, b.size.w, 30),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // --- history graph -------------------------------------------------
  GRect g = PBL_IF_ROUND_ELSE(GRect(26, top + 82, b.size.w - 52, 48),
                              GRect(4, top + 84, b.size.w - 8, b.size.h - (top + 88)));
  draw_graph(ctx, g);
}

// ---------------------------------------------------------------------------

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *sgv = dict_find(iter, MESSAGE_KEY_SGV);
  Tuple *err = dict_find(iter, MESSAGE_KEY_ERR);

  if (sgv) {
    s_sgv   = sgv->value->int32;
    Tuple *t;
    if ((t = dict_find(iter, MESSAGE_KEY_DELTA))) s_delta = t->value->int32;
    if ((t = dict_find(iter, MESSAGE_KEY_TREND))) s_trend = t->value->int32;
    if ((t = dict_find(iter, MESSAGE_KEY_AGE)))   s_age   = t->value->int32;
    if ((t = dict_find(iter, MESSAGE_KEY_HISTORY))) {
      int n = t->length;
      if (n > (int)sizeof(s_hist)) n = sizeof(s_hist);
      memcpy(s_hist, t->value->data, n);
      s_hist_count = n;
    }
    s_err = 0;
    s_have_data = true;
    s_last_rx = time(NULL);
  } else if (err && err->value->int32 != 0) {
    s_err = err->value->int32;
  }
  layer_mark_dirty(s_canvas);
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", reason);
}

// ---------------------------------------------------------------------------

static void refresh_tick(void *data) {
  request_refresh();
  s_refresh_timer = app_timer_register(60000, refresh_tick, NULL);
}

static void dirty_tick(void *data) {
  layer_mark_dirty(s_canvas);   // keep the "Xm ago" line moving
  s_dirty_timer = app_timer_register(20000, dirty_tick, NULL);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  request_refresh();
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  window_set_background_color(window, GColorBlack);
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void init(void) {
  s_arrow = gpath_create(&ARROW_PATH);

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(256, 64);

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  request_refresh();
  s_refresh_timer = app_timer_register(60000, refresh_tick, NULL);
  s_dirty_timer = app_timer_register(20000, dirty_tick, NULL);
}

static void deinit(void) {
  if (s_refresh_timer) app_timer_cancel(s_refresh_timer);
  if (s_dirty_timer) app_timer_cancel(s_dirty_timer);
  window_destroy(s_window);
  gpath_destroy(s_arrow);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
