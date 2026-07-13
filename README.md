# waybar-weather

A [waybar](https://github.com/Alexays/Waybar) **CFFI plugin** for weather.

## Features

- **Bar pill:** a condition icon (nf-md-weather glyph, day/night aware, mapped
  from the WMO code) + the current temperature — e.g. ` 26°C`.
- **Click → forecast popover:**
  - current conditions: big icon, temperature, condition, feels-like, city;
  - metrics grid: humidity, wind, pressure, precipitation chance, sunrise, sunset;
  - a **7-day forecast** row: day · icon · hi/lo.
- **Data:** [open-meteo](https://open-meteo.com) (no API key), fetched with `curl`
  every 15 minutes.
- **Location:** IP-geolocated (ip-api.com) by default; or set fixed
  `latitude`/`longitude`, or a `location` city name (open-meteo geocoding).
- Metric or imperial (`fahrenheit`).

## Build & install

Requires `gtk3`, `glib2`, `json-glib` (+dev headers), `curl`, and a C compiler.

```sh
make
make install                 # → ~/.local/lib/waybar/libweather.so
```

## waybar config

```jsonc
"modules-center": ["cffi/weather"],

"cffi/weather": {
    "module_path": "/home/YOU/.local/lib/waybar/libweather.so"
}
```

Options:

| key | default | meaning |
|-----|---------|---------|
| `module_path` | *(required)* | path to `libweather.so` |
| `latitude` / `longitude` | *(unset)* | fixed coordinates (skips geolocation) |
| `location` | *(unset)* | city name, geocoded via open-meteo |
| `fahrenheit` | `0` | `1` = °F / mph |
| `interval` | `900` | refresh seconds (min 300) |

If neither coordinates nor a city are given, the location is IP-geolocated.

## style.css

Bar: `#weather`, with `.wx-icon` / `.wx-label`. Popover content classes:
`.wx-hero-icon`, `.wx-hero-temp`, `.wx-cond`, `.wx-sub`, `.wx-metric-icon`,
`.wx-metric-label`, `.wx-metric-value`, `.wx-day`, `.wx-today`, `.wx-dayname`,
`.wx-dayicon`, `.wx-daytemp`. The popover itself is a standard GTK `popover`.

```css
#weather .wx-icon   { font-size: 22px; }
.wx-hero-icon       { font-size: 56px; color: @primary; }
.wx-today           { background-color: alpha(@primary, 0.16); }
```

## License

MIT
