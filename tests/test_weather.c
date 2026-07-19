// Unit tests for weather.c's pure logic (WMO code -> icon/text mapping,
// unit conversion, date/time formatting) -- no GTK init, no Wayland, no
// network. #includes the plugin source directly to reach its `static`
// functions without changing their visibility for production code; this
// file supplies its own main() (weather.c has none), so nothing conflicts.
#include "../src/weather.c"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
	else { printf("ok - %s\n", msg); } \
} while (0)
#define CHECK_STR(a, b, msg) CHECK(strcmp((a), (b)) == 0, msg)

int main(void) {
	// wmo_icon: day/night-aware for clear/partly-cloudy, otherwise fixed
	CHECK_STR(wmo_icon(0, TRUE), "sunny.svg", "wmo_icon(0, day) is sunny");
	CHECK_STR(wmo_icon(0, FALSE), "night.svg", "wmo_icon(0, night) is night");
	CHECK_STR(wmo_icon(2, TRUE), "pcloudy.svg", "wmo_icon(2, day) is partly cloudy");
	CHECK_STR(wmo_icon(2, FALSE), "npcloudy.svg", "wmo_icon(2, night) is night partly cloudy");
	CHECK_STR(wmo_icon(3, TRUE), "cloud.svg", "wmo_icon(3) is overcast regardless of day/night");
	CHECK_STR(wmo_icon(95, TRUE), "tstorm.svg", "wmo_icon(95) is thunderstorm");
	CHECK_STR(wmo_icon(9999, TRUE), "cloud.svg", "wmo_icon of an unknown code falls back to cloud");

	// wmo_text
	CHECK_STR(wmo_text(0), "Clear", "wmo_text(0) is Clear");
	CHECK_STR(wmo_text(65), "Heavy Rain", "wmo_text(65) is Heavy Rain");
	CHECK_STR(wmo_text(9999), "Unknown", "wmo_text of an unknown code is Unknown");

	// to_unit / unit_suffix: Celsius (default) vs Fahrenheit conversion
	Inst c_inst = {0};  // fahrenheit defaults to FALSE (zero-initialized)
	CHECK(to_unit(&c_inst, 20.0) == 20, "to_unit in Celsius mode is a passthrough (rounded)");
	CHECK_STR(unit_suffix(&c_inst), "\xc2\xb0""C", "unit_suffix in Celsius mode is degree-C");

	Inst f_inst = {0}; f_inst.fahrenheit = TRUE;
	CHECK(to_unit(&f_inst, 20.0) == 68, "to_unit converts 20C to 68F");
	CHECK(to_unit(&f_inst, 0.0) == 32, "to_unit converts 0C to 32F");
	CHECK_STR(unit_suffix(&f_inst), "\xc2\xb0""F", "unit_suffix in Fahrenheit mode is degree-F");

	// hhmm: "2026-07-13T05:12" -> "05:12", missing/malformed -> "--:--"
	char buf[8];
	hhmm("2026-07-13T05:12", buf, sizeof buf);
	CHECK_STR(buf, "05:12", "hhmm extracts HH:MM after the T separator");
	hhmm(NULL, buf, sizeof buf);
	CHECK_STR(buf, "--:--", "hhmm falls back to --:-- for a NULL input");
	hhmm("no-time-here", buf, sizeof buf);
	CHECK_STR(buf, "--:--", "hhmm falls back to --:-- when there's no T separator");

	// day_name: idx 0/1 are special-cased regardless of the date string,
	// idx >= 2 formats the actual weekday
	char dbuf[12];
	day_name("2026-07-19", 0, dbuf, sizeof dbuf);
	CHECK_STR(dbuf, "Today", "day_name idx 0 is always Today");
	day_name("2026-07-20", 1, dbuf, sizeof dbuf);
	CHECK_STR(dbuf, "Tmrw", "day_name idx 1 is always Tmrw");
	day_name("2026-07-21", 2, dbuf, sizeof dbuf);  // a Tuesday
	CHECK_STR(dbuf, "Tue", "day_name idx>=2 formats the actual weekday abbreviation");
	day_name(NULL, 2, dbuf, sizeof dbuf);
	CHECK_STR(dbuf, "--", "day_name idx>=2 with no date string falls back to --");

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
