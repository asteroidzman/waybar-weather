// waybar CFFI plugin: weather.
//
//  - Bar pill: [condition icon] NN°  (nf-md-weather glyph mapped from the WMO
//    code, day/night aware) + temperature.
//  - Click opens a popover: current conditions (big icon, temp, feels-like,
//    condition, city), a metrics grid (humidity/wind/pressure/precip/sun) and a
//    7-day forecast row (day · icon · hi/lo).
//  - Data: open-meteo (no API key), fetched with curl every 15 min. Location is
//    IP-geolocated (ip-api.com) unless latitude/longitude or a city are configured.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

typedef struct { char day[12]; int code; int tmin, tmax, precip; } Day;

typedef struct {
  GtkWidget *box, *icon, *label, *popover;
  // config
  double cfg_lat, cfg_lon; gboolean cfg_coords, fahrenheit;
  char *cfg_city; int interval;
  // resolved location
  double lat, lon; gboolean have_loc; char city[96], country[64];
  // current conditions
  gboolean available;
  int temp, feels, code, humidity, wind, pressure, precip; gboolean isDay;
  char sunrise[8], sunset[8];
  Day days[7]; int ndays;
  guint timer; GCancellable *cancel;
} Inst;

// ─── WMO weather_code → nf-md-weather glyph + condition text ─────────────────
static const char *wmo_glyph(int code, gboolean day) {
  switch (code) {
    case 0: case 1:            return day ? "\xf3\xb0\x96\x99" : "\xf3\xb0\x96\x94"; // sunny / night
    case 2:                    return day ? "\xf3\xb0\x96\x95" : "\xf3\xb0\xbc\xb1"; // partly-cloudy day/night
    case 3:                    return "\xf3\xb0\x96\x90";  // cloudy
    case 45: case 48:          return "\xf3\xb0\x96\x91";  // fog
    case 65: case 67: case 82: return "\xf3\xb0\x96\x96";  // pouring (heavy rain/showers)
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 66: case 80: case 81:
                               return "\xf3\xb0\x96\x97";  // rainy
    case 75: case 86:          return "\xf3\xb0\xbc\xb6";  // heavy snow
    case 71: case 73: case 77: case 85:
                               return "\xf3\xb0\x96\x98";  // snowy
    case 95: case 96: case 99: return "\xf3\xb0\x99\xbe";  // thunderstorm
    default:                   return "\xf3\xb0\x96\x90";  // cloud
  }
}
static const char *wmo_text(int code) {
  switch (code) {
    case 0: case 1: return "Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing Drizzle";
    case 61: case 80: return "Light Rain";
    case 63: case 81: return "Rain";
    case 65: case 82: return "Heavy Rain";
    case 66: case 67: return "Freezing Rain";
    case 71: case 85: return "Light Snow";
    case 73: return "Snow";
    case 75: case 86: return "Heavy Snow";
    case 77: return "Snow";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, Hail";
    default: return "Unknown";
  }
}

static int to_unit(Inst *s, double c) { return (int)lround(s->fahrenheit ? c * 9.0 / 5.0 + 32.0 : c); }
static const char *unit_suffix(Inst *s) { return s->fahrenheit ? "\xc2\xb0""F" : "\xc2\xb0""C"; }

// "2026-07-13T05:12" -> "05:12"
static void hhmm(const char *iso, char *out, size_t n) {
  const char *t = iso ? strchr(iso, 'T') : NULL;
  if (t && strlen(t) >= 6) g_strlcpy(out, t + 1, 6 <= n ? 6 : n);
  else g_strlcpy(out, "--:--", n);
}
// "2026-07-13" (+index) -> "Today"/"Tomorrow"/"Mon"
static void day_name(const char *iso, int idx, char *out, size_t n) {
  if (idx == 0) { g_strlcpy(out, "Today", n); return; }
  if (idx == 1) { g_strlcpy(out, "Tmrw", n); return; }
  struct tm tm; memset(&tm, 0, sizeof tm);
  if (iso && strptime(iso, "%Y-%m-%d", &tm)) {
    tm.tm_isdst = -1; mktime(&tm);
    strftime(out, n, "%a", &tm);
  } else g_strlcpy(out, "--", n);
}

// ─── bar ─────────────────────────────────────────────────────────────────────
static void update_bar(Inst *self) {
  if (!self->available) {
    gtk_label_set_text(GTK_LABEL(self->icon), "\xf3\xb0\x96\x90");   // cloud
    char t[16]; g_snprintf(t, sizeof t, "--%s", unit_suffix(self));
    gtk_label_set_text(GTK_LABEL(self->label), t);
    return;
  }
  gtk_label_set_text(GTK_LABEL(self->icon), wmo_glyph(self->code, self->isDay));
  char t[24]; g_snprintf(t, sizeof t, "%d%s", to_unit(self, self->temp), unit_suffix(self));
  gtk_label_set_text(GTK_LABEL(self->label), t);
}

// ─── JSON helpers ────────────────────────────────────────────────────────────
static double dmember(JsonObject *o, const char *k, double dflt) {
  return (o && json_object_has_member(o, k)) ? json_object_get_double_member(o, k) : dflt;
}
static double arr_d(JsonArray *a, guint i) {
  return (a && i < json_array_get_length(a)) ? json_array_get_double_element(a, i) : 0;
}

// ─── parse open-meteo forecast ───────────────────────────────────────────────
static void parse_forecast(Inst *self, const char *body) {
  JsonParser *p = json_parser_new();
  if (!json_parser_load_from_data(p, body, -1, NULL)) { g_object_unref(p); return; }
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  if (!root || !json_object_has_member(root, "current") || !json_object_has_member(root, "daily")) {
    g_object_unref(p); return;
  }
  JsonObject *cur = json_object_get_object_member(root, "current");
  self->temp = (int)lround(dmember(cur, "temperature_2m", 0));
  self->feels = (int)lround(dmember(cur, "apparent_temperature", self->temp));
  self->code = (int)dmember(cur, "weather_code", 0);
  self->humidity = (int)lround(dmember(cur, "relative_humidity_2m", 0));
  self->wind = (int)lround(dmember(cur, "wind_speed_10m", 0));
  self->pressure = (int)lround(dmember(cur, "surface_pressure", 0));
  self->isDay = dmember(cur, "is_day", 1) != 0;

  JsonObject *d = json_object_get_object_member(root, "daily");
  JsonArray *tm = json_object_get_array_member(d, "time");
  JsonArray *hi = json_object_get_array_member(d, "temperature_2m_max");
  JsonArray *lo = json_object_get_array_member(d, "temperature_2m_min");
  JsonArray *wc = json_object_get_array_member(d, "weather_code");
  JsonArray *pp = json_object_has_member(d, "precipitation_probability_max")
                  ? json_object_get_array_member(d, "precipitation_probability_max") : NULL;
  JsonArray *sr = json_object_get_array_member(d, "sunrise");
  JsonArray *ss = json_object_get_array_member(d, "sunset");
  if (sr && json_array_get_length(sr)) hhmm(json_array_get_string_element(sr, 0), self->sunrise, sizeof self->sunrise);
  if (ss && json_array_get_length(ss)) hhmm(json_array_get_string_element(ss, 0), self->sunset, sizeof self->sunset);
  self->precip = pp ? (int)lround(arr_d(pp, 0)) : 0;

  guint n = tm ? json_array_get_length(tm) : 0; if (n > 7) n = 7;
  self->ndays = n;
  for (guint i = 0; i < n; i++) {
    Day *day = &self->days[i];
    day_name(json_array_get_string_element(tm, i), i, day->day, sizeof day->day);
    day->code = (int)arr_d(wc, i);
    day->tmax = (int)lround(arr_d(hi, i));
    day->tmin = (int)lround(arr_d(lo, i));
    day->precip = pp ? (int)lround(arr_d(pp, i)) : 0;
  }
  self->available = TRUE;
  g_object_unref(p);
  update_bar(self);
}

static void fetch_forecast(Inst *self);   // fwd

static void parse_geo(Inst *self, const char *body) {
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    if (o && json_object_has_member(o, "lat") && json_object_has_member(o, "lon")) {
      self->lat = json_object_get_double_member(o, "lat");
      self->lon = json_object_get_double_member(o, "lon");
      self->have_loc = TRUE;
      if (json_object_has_member(o, "city"))
        g_strlcpy(self->city, json_object_get_string_member(o, "city") ?: "", sizeof self->city);
    }
  }
  g_object_unref(p);
  if (self->have_loc) fetch_forecast(self);
}
static void parse_geocode(Inst *self, const char *body) {  // open-meteo geocoding
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    if (o && json_object_has_member(o, "results")) {
      JsonArray *r = json_object_get_array_member(o, "results");
      if (json_array_get_length(r)) {
        JsonObject *g0 = json_array_get_object_element(r, 0);
        self->lat = dmember(g0, "latitude", 0);
        self->lon = dmember(g0, "longitude", 0);
        self->have_loc = TRUE;
        if (json_object_has_member(g0, "name"))
          g_strlcpy(self->city, json_object_get_string_member(g0, "name") ?: "", sizeof self->city);
      }
    }
  }
  g_object_unref(p);
  if (self->have_loc) fetch_forecast(self);
}

// ─── curl (async, teardown-safe) ─────────────────────────────────────────────
typedef struct { Inst *self; GCancellable *cancel; int kind; } FCtx;  // 0 geo, 1 forecast, 2 geocode
static void fctx_free(FCtx *c) { g_object_unref(c->cancel); g_free(c); }
static void on_curl(GObject *src, GAsyncResult *res, gpointer data) {
  FCtx *c = data;
  char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL);
  if (g_cancellable_is_cancelled(c->cancel)) { g_free(out); fctx_free(c); return; }
  if (ok && out && *out) {
    if (c->kind == 0) parse_geo(c->self, out);
    else if (c->kind == 2) parse_geocode(c->self, out);
    else parse_forecast(c->self, out);
  }
  g_free(out); fctx_free(c);
}
static void run_curl(Inst *self, int kind, const char *url) {
  GSubprocess *sp = g_subprocess_new(
    G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
    "curl", "-sS", "--fail", "--connect-timeout", "3", "--max-time", "8", "--compressed",
    "-A", "waybar-weather", url, NULL);
  if (!sp) return;
  FCtx *c = g_new0(FCtx, 1); c->self = self; c->cancel = g_object_ref(self->cancel); c->kind = kind;
  g_subprocess_communicate_utf8_async(sp, NULL, self->cancel, on_curl, c);
  g_object_unref(sp);   // the async op keeps it alive until on_curl
}

static void fetch_forecast(Inst *self) {
  char *url = g_strdup_printf(
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,surface_pressure,wind_speed_10m"
    "&daily=sunrise,sunset,temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max"
    "&timezone=auto&forecast_days=7", self->lat, self->lon);
  run_curl(self, 1, url);
  g_free(url);
}
static void fetch(Inst *self) {
  if (self->cfg_coords) { self->lat = self->cfg_lat; self->lon = self->cfg_lon; self->have_loc = TRUE; }
  if (self->have_loc) { fetch_forecast(self); return; }
  if (self->cfg_city && *self->cfg_city) {          // geocode a city name
    char *q = g_uri_escape_string(self->cfg_city, NULL, FALSE);
    char *url = g_strdup_printf("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json", q);
    run_curl(self, 2, url); g_free(url); g_free(q);
  } else {                                          // IP geolocation
    run_curl(self, 0, "http://ip-api.com/json/");
  }
}

// ─── popover (current + metrics + 7-day forecast) ────────────────────────────
static GtkWidget *metric(const char *glyph, const char *label, const char *value) {
  GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *g = gtk_label_new(glyph);
  gtk_style_context_add_class(gtk_widget_get_style_context(g), "wx-metric-icon");
  GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *l = gtk_label_new(label), *v = gtk_label_new(value);
  gtk_widget_set_halign(l, GTK_ALIGN_START); gtk_widget_set_halign(v, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(l), "wx-metric-label");
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "wx-metric-value");
  gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(col), v, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(r), g, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(r), col, FALSE, FALSE, 0);
  return r;
}
static void rebuild_popover(Inst *self) {
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(self->popover));
  if (old) gtk_widget_destroy(old);
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
  gtk_widget_set_margin_top(v, 14); gtk_widget_set_margin_bottom(v, 14);
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "wx-pop");

  if (!self->available) {
    gtk_box_pack_start(GTK_BOX(v), gtk_label_new("Weather unavailable"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(self->popover), v); gtk_widget_show_all(v); return;
  }

  // hero: big icon + temp + condition/feels/city
  GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *bigicon = gtk_label_new(wmo_glyph(self->code, self->isDay));
  gtk_style_context_add_class(gtk_widget_get_style_context(bigicon), "wx-hero-icon");
  GtkWidget *hcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  char buf[96];
  g_snprintf(buf, sizeof buf, "%d%s", to_unit(self, self->temp), unit_suffix(self));
  GtkWidget *bigtemp = gtk_label_new(buf);
  gtk_widget_set_halign(bigtemp, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(bigtemp), "wx-hero-temp");
  GtkWidget *cond = gtk_label_new(wmo_text(self->code));
  gtk_widget_set_halign(cond, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(cond), "wx-cond");
  g_snprintf(buf, sizeof buf, "Feels like %d%s", to_unit(self, self->feels), unit_suffix(self));
  GtkWidget *feels = gtk_label_new(buf);
  gtk_widget_set_halign(feels, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(feels), "wx-sub");
  GtkWidget *cityl = gtk_label_new(self->city[0] ? self->city : "");
  gtk_widget_set_halign(cityl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(cityl), "wx-sub");
  gtk_box_pack_start(GTK_BOX(hcol), bigtemp, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hcol), cond, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hcol), feels, FALSE, FALSE, 0);
  if (self->city[0]) gtk_box_pack_start(GTK_BOX(hcol), cityl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hero), bigicon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hero), hcol, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), hero, FALSE, FALSE, 0);

  // metrics grid (2 columns x 3 rows)
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  char h[16], w[16], pr[16], pc[16];
  g_snprintf(h, sizeof h, "%d%%", self->humidity);
  g_snprintf(w, sizeof w, self->fahrenheit ? "%d mph" : "%d km/h",
             self->fahrenheit ? (int)lround(self->wind * 0.621371) : self->wind);
  g_snprintf(pr, sizeof pr, "%d hPa", self->pressure);
  g_snprintf(pc, sizeof pc, "%d%%", self->precip);
  gtk_grid_attach(GTK_GRID(grid), metric("\xf3\xb0\x96\x9c", "Humidity", h), 0, 0, 1, 1);   // water-percent
  gtk_grid_attach(GTK_GRID(grid), metric("\xf3\xb0\x96\xbd", "Wind", w), 1, 0, 1, 1);        // weather-windy
  gtk_grid_attach(GTK_GRID(grid), metric("\xf3\xb0\x8a\x8b", "Pressure", pr), 0, 1, 1, 1);   // gauge
  gtk_grid_attach(GTK_GRID(grid), metric("\xf3\xb0\x96\x96", "Precip", pc), 1, 1, 1, 1);     // pouring
  gtk_grid_attach(GTK_GRID(grid), metric("\xf3\xb0\x96\x99", "Sunrise", self->sunrise), 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), metric("\xf3\xb0\x96\x94", "Sunset", self->sunset), 1, 2, 1, 1);
  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(v), grid, FALSE, FALSE, 0);

  // 7-day forecast row
  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
  GtkWidget *fc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous(GTK_BOX(fc), TRUE);
  for (int i = 0; i < self->ndays; i++) {
    Day *day = &self->days[i];
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(cell), i == 0 ? "wx-day wx-today" : "wx-day");
    GtkWidget *dn = gtk_label_new(day->day);
    gtk_style_context_add_class(gtk_widget_get_style_context(dn), "wx-dayname");
    GtkWidget *di = gtk_label_new(wmo_glyph(day->code, TRUE));
    gtk_style_context_add_class(gtk_widget_get_style_context(di), "wx-dayicon");
    char hilo[24]; g_snprintf(hilo, sizeof hilo, "%d\xc2\xb0/%d\xc2\xb0", to_unit(self, day->tmax), to_unit(self, day->tmin));
    GtkWidget *tl = gtk_label_new(hilo);
    gtk_style_context_add_class(gtk_widget_get_style_context(tl), "wx-daytemp");
    gtk_box_pack_start(GTK_BOX(cell), dn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cell), di, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cell), tl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fc), cell, TRUE, TRUE, 0);
  }
  gtk_box_pack_start(GTK_BOX(v), fc, FALSE, FALSE, 0);

  gtk_container_add(GTK_CONTAINER(self->popover), v);
  gtk_widget_show_all(v);
}
static gboolean on_pop_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
  (void)d; if (e->keyval == GDK_KEY_Escape) { gtk_popover_popdown(GTK_POPOVER(w)); return TRUE; }
  return FALSE;
}
static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w; if (ev->button != 1) return FALSE;
  Inst *self = data;
  rebuild_popover(self);
  gtk_popover_popup(GTK_POPOVER(self->popover));
  gtk_widget_grab_focus(self->popover);   // so Escape reaches it
  return TRUE;
}

static gboolean on_timer(gpointer data) { fetch(data); return G_SOURCE_CONTINUE; }
static gboolean debug_popup(gpointer d) {   // WX_DEBUG_POPUP test hook
  Inst *s = d; rebuild_popover(s); gtk_popover_popup(GTK_POPOVER(s->popover)); return G_SOURCE_REMOVE;
}

// ─── CFFI entry points ───────────────────────────────────────────────────────
void *wbcffi_init(const wbcffi_init_info *info,
                  const wbcffi_config_entry *entries, size_t entries_len) {
  Inst *self = g_new0(Inst, 1);
  self->interval = 900;
  gboolean has_lat = FALSE, has_lon = FALSE;
  for (size_t i = 0; i < entries_len; i++) {
    const char *k = entries[i].key, *v = entries[i].value;
    if (!strcmp(k, "latitude")) { self->cfg_lat = atof(v); has_lat = TRUE; }
    else if (!strcmp(k, "longitude")) { self->cfg_lon = atof(v); has_lon = TRUE; }
    else if (!strcmp(k, "location")) self->cfg_city = g_strdup(v);
    else if (!strcmp(k, "fahrenheit")) self->fahrenheit = atoi(v) != 0;
    else if (!strcmp(k, "interval")) { self->interval = atoi(v); if (self->interval < 300) self->interval = 300; }
  }
  self->cfg_coords = has_lat && has_lon;
  self->cancel = g_cancellable_new();

  GtkContainer *root = info->get_root_widget(info->obj);
  self->box = gtk_event_box_new();
  gtk_widget_set_name(self->box, "weather");
  gtk_widget_add_events(self->box, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_margin_start(self->box, 8);
  gtk_widget_set_margin_end(self->box, 8);
  GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->icon = gtk_label_new("\xf3\xb0\x96\x90");
  gtk_style_context_add_class(gtk_widget_get_style_context(self->icon), "wx-icon");
  self->label = gtk_label_new("--");
  gtk_style_context_add_class(gtk_widget_get_style_context(self->label), "wx-label");
  gtk_box_pack_start(GTK_BOX(h), self->icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(h), self->label, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(self->box), h);
  self->popover = gtk_popover_new(self->box);
  gtk_popover_set_position(GTK_POPOVER(self->popover), GTK_POS_BOTTOM);
  gtk_popover_set_constrain_to(GTK_POPOVER(self->popover), GTK_POPOVER_CONSTRAINT_NONE);
  gtk_popover_set_modal(GTK_POPOVER(self->popover), TRUE);   // click-outside dismisses
  gtk_widget_add_events(self->popover, GDK_KEY_PRESS_MASK);
  g_signal_connect(self->popover, "key-press-event", G_CALLBACK(on_pop_key), NULL);
  g_signal_connect(self->box, "button-press-event", G_CALLBACK(on_click), self);
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  update_bar(self);
  fetch(self);
  self->timer = g_timeout_add_seconds(self->interval, on_timer, self);
  if (g_getenv("WX_DEBUG_POPUP")) g_timeout_add(3000, debug_popup, self);  // test hook
  return self;
}

void wbcffi_deinit(void *instance) {
  Inst *self = instance;
  if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->timer) g_source_remove(self->timer);
  g_clear_object(&self->cancel);
  g_free(self->cfg_city);
  g_free(self);
}
