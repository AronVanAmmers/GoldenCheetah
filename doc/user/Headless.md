# GoldenCheetah without a window: command line and REST API

GoldenCheetah can run its main jobs without opening a window. You can:

- import activity files
- run data processors, including Python fixes
- edit activity fields, zones and measures
- query metrics, PMC and critical power
- draw charts to PNG, SVG or PDF

The same commands are available from the command line and, over HTTP, as a REST API.

Each command opens the athlete, runs, saves and closes the athlete again before it exits. GoldenCheetah does not stay running.

```sh
GoldenCheetah --cli --athlete Joe import ~/Downloads/morning-ride.fit
GoldenCheetah --cli --athlete Joe activity list --filter 'isRun = 0' --metric coggan_tss
GoldenCheetah --cli --athlete Joe chart meanmax --from 2026-01-01 -o bests.png
```

A symlink named `gc-cli` (or `goldencheetah-cli`) to the GoldenCheetah binary behaves like `GoldenCheetah --cli`.

## Choosing the athlete

| Option | Meaning |
|---|---|
| `--home DIR` | The athletes folder. Default: `$GC_HOME`, else the library folder set in the GUI, else the platform default (`~/.goldencheetah` on Linux). |
| `--athlete NAME`, `-a` | The athlete's folder name inside `--home`. It can be left out when there is only one athlete. |
| `--athlete-dir DIR` | The full path of an athlete folder. It sets both of the above. |

Activity field definitions (`field ...`) and Python processors (`processor install`) are stored in the athletes folder and shared by every athlete in it, as in the GUI.

## Output and exit status

The default output is text for people. With `--format json` every command prints the same envelope the REST API returns:

```json
{ "ok": true, "status": "ok", "command": "activity.list", "data": { ... } }
```

With `--format csv` the result is CSV, for spreadsheets and data tools. Only data goes to stdout, and errors go to stderr.

- A result that is a list (activities, intervals, metrics, fields ...) is one table: a row per item, with nested values such as `metrics` as columns. Numbers are in full precision, or as the GUI shows them with `--display` where a command has it.
- Other results (`activity show`, `zones show`, `version` ...) are `key,value` lines. Nested values have dotted paths, such as `metrics.average_power` or `zones.hr.zones[0].percent`.
- `activity overview` with a single table tile (`--tile "Intervals Data"`) gives that table: a header, then one row per record, and only the header when there are none. Units stay in the header, such as `Pace (min/km)`, for one row or many. With several tiles each value is a line: `tile,kind,row,column,units,value`.

```sh
gc-cli -a Joe --format csv interval list last --metric Average_Power,Duration > laps.csv
gc-cli -a Joe --format csv activity overview last --tile "Intervals Data" > intervals.csv
```

`--output FILE` (`-o`) writes the result there: a chart, an exported activity, or the text, JSON, or CSV that would otherwise be printed. Charts and exports are still written to a file named after the activity or chart when `-o` is omitted. `-o -` writes to stdout. If the file cannot be written, the command exits with an error.

| Exit | Meaning |
|---|---|
| 0 | ok |
| 1 | done, but some items failed (see the report) |
| 2 | bad arguments |
| 3 | athlete, activity or processor not found |
| 4 | the athlete is in use by another GoldenCheetah |
| 5 | the command failed |
| 6 | internal error |

`GoldenCheetah --cli --help` lists every command. `GoldenCheetah --cli help <command>` describes one command. `commands --format json` describes them all in machine-readable form.

## Commands

| Area | Commands |
|---|---|
| Athletes | `athlete list`, `athlete create`, `athlete show`, `athlete refresh [--rebuild]` |
| Import | `import FILE-OR-FOLDER... [--recursive] [--dry-run]`, `formats` |
| Activities | `activity list`, `activity show`, `activity overview [--tile NAME]`, `layout list`, `layout tile list|show|set`, `activity export --as tcx`, `activity set --set 'Field=value'`, `activity delete`, `activity eval --expression '...'`, `activity column list|add|remove` |
| Intervals | `interval list ACTIVITY [--type user,effort] [--metric ...] [--display]`, `interval show ACTIVITY NUMBER-OR-NAME` |
| Fields | `field list`, `field add NAME... --type double --tab TAB`, `field remove` |
| Processors | `processor list`, `processor show`, `processor install NAME --file script.py`, `processor configure`, `processor remove`, `processor run NAME ...` |
| Metrics | `metric list`, `metric user list|show|add|edit|remove`, `metric favourite list|add|remove|set`, `metric aggregate`, `pmc`, `meanmax`, `cp`, `cp estimates` |
| Zones and measures | `zones show`, `zones set` (`--type power\|hr\|pace`), `measures list`, `measures add` |
| Charts | `chart activity`, `chart meanmax`, `chart pmc`, `chart zones [--type power\|hr\|pace\|fatigue]`, `chart trend` (`--as png\|svg\|pdf`, `--width`, `--height`, `--dark`), `chart library list\|show\|add\|edit\|remove`, `chart library curve add\|edit\|remove` |
| Server | `serve` (see [REST API](#rest-api)) |

### Choosing activities

Commands that work on several activities accept the same selection options:

- **Names**: activity ids as arguments. An id can be a file name (`2026_03_01_09_30_00`), a start time (`2026-03-01T09:30:00`), a date (when only one activity starts on it), `first` or `last`.
- **`--filter EXPR`**: a filter as typed in the GUI filter box, e.g. `isRun = 0` or `Sport = "Bike" and Average_Power > 200`.
- **`--search TEXT`**: a free text search, as in the GUI search box.
- **`--named NAME`**: a search or filter saved in the GUI.
- **`--from DATE --to DATE`**, **`--sport Bike`**, **`--limit N`** (the most recent N), **`--planned`**.

All given criteria must match. `processor run` and `activity set` change nothing unless activities are chosen or `--all` is given.

### Setting fields

`activity set ACTIVITY --set 'Field=value'` edits a field the way the Details tab does:

- A metric field (`Distance`, `Average Power`, `Work` ...) sets that metric's override, as ticking its box and typing a value does. `--set 'Average Power='` removes the override, and the computed value is used again. Values are in the units the GUI shows.
- `Start Date` (`2026-03-01`) and `Start Time` (`09:30` or `09:30:15`) change when the activity started, and its file is renamed after the new start, as in the GUI. A start another activity already has is refused, because saving would overwrite that activity.
- `Device` and `Recording Interval` change the activity itself.
- Other fields are tags. Numbers, dates (`yyyy-mm-dd`), times of day and checkboxes (`1`, `0`, `yes`, `no`) are checked against the field's type. An empty value clears the field.
- Fields linked by a default (Preferences, Data Fields, Defaults) are filled in when they are empty, as in the GUI.
- `Summary` is computed and fields of intervals can't be set here.

### What the activity view shows

`activity overview ACTIVITY` returns the activity's overview as the GUI draws it. It uses the athlete's own layout, the same one the GUI switches to for that activity: the first layout whose expression matches (for example `isRun`), or else the first layout. Each tile is worked out the way the GUI works it out:

- Tables have the same columns, units and rows, formatted as shown (`05:30`, `160`, `51:51`), in the tile's saved sort order.
- Metric, field, RPE and KPI tiles give the value as displayed.
- Zone tiles give each zone's time and %.
- PMC tiles give form, fitness, fatigue and risk.

`--tile "Intervals Data"` picks tiles by their title, and `--layout NAME` uses another layout. A table with more than one row of values comes back as a grid (`style: grid`, one entry in `columns` per column). With a single row the GUI shows a list of name, value and units, and so do the text report and JSON (`style: list`). CSV is always the grid: the tile's column names, units in the header, one record per row. Route and chart tiles are named but not reproduced as data.

```sh
gc-cli -a Joe activity overview last --tile "Intervals Data"
gc-cli -a Joe activity overview last --format json
```

For raw numbers:

| In the GUI | On the command line |
|---|---|
| Date, Sport, Workout Code, Notes, RPE and other fields | `activity show` (`metadata`) |
| Metrics | `activity show` (`metrics`, every metric relevant for the activity, zeros included, keyed by symbol) |
| Power, heart rate, pace and W' balance (fatigue) zone tables | `activity show` (`zones`: name, description, low, high, time and % of the recording time, as the GUI) |
| Form, Fitness, Fatigue and Risk | `activity show` (`pmc`: `tsb`, `ctl`, `atl`, `rr` on the day, from GOVSS for runs, SwimScore for swims and TSS otherwise; `--pmc-metric` to choose) |
| Intervals | `interval list` (the intervals sidebar's metrics, or `--metric`), `interval show` (every relevant metric) |
| Route and data series | `activity export --as gpx`, `--as csv` or `--as json` |
| Zone and PMC charts | `chart zones`, `chart pmc`, `chart activity` |

Wherever a command takes `--metric`, a metric can be given by its symbol (`average_power`, `skiba_wprime_exp`) or by the name used in formulas and in the GUI's table definitions (`Average_Power`, `W'_Work`). `metric list` shows both, including this athlete's user metrics, and `--search` matches either. `--display` on `interval list` and `interval show` returns values as the GUI formats them rather than as numbers.

### User metrics, favourites and zones

A user metric is a formula GoldenCheetah evaluates for the whole activity and again for every interval. `metric user add` writes it to the shared `usermetrics.xml` in the athletes folder, the same file as Preferences → Metrics → Custom, and rebuilds the metric cache. The formula is checked first. A program that does not parse, or has no `value` block, is refused and the file is left unchanged.

```sh
gc-cli -a Joe metric user add --symbol hrr_v --name "HRR/v" --type average \
    --units bpm/kph --imperial-units bpm/mph --conversion 1.609 --precision 2 \
    --file hrr_v.formula
gc-cli -a Joe metric user edit hrr_v --precision 1
gc-cli -a Joe metric user show hrr_v
```

`--program` takes the formula text. `--file -` reads it from stdin. Type `average` with `count { Duration; }` makes a weekly trend a time-weighted mean.

Favourites are what the ride summary and `interval list` show, in that order. `metric favourite set` replaces the list; the order of the arguments is the top-to-bottom order, the same list Preferences saves after the up and down buttons. `metric favourite add` appends, so new rows go at the bottom.

```sh
gc-cli -a Joe metric favourite add hrr_v
gc-cli -a Joe metric favourite set workout_time average_hr hrr_v average_speed
```

The intervals table on the activity overview is a different list. It is the program of a table tile, saved in the athlete's `config/analysis-perspectives.xml`. The Run layout (`isRun`) and the Swim layout (`isSwim`) each have one, named Intervals Data. `layout list` shows the layouts, `layout tile show` prints the program, and `layout tile set` replaces it. A program that does not parse is refused and the file is left unchanged.

A column in that program is a formula name, the name `metric list` prints as `formula`: the metric name with spaces turned into underscores (`Average_Heart_Rate`). `metricname` and `intervalstrings` only accept that kind of symbol. A name such as `HRR/v` is refused, because `/` is division. Name the metric `HRR_v` and use that.

```sh
gc-cli -a Joe layout tile show "Intervals Data" --layout Run
gc-cli -a Joe layout tile set "Intervals Data" --layout Run --file intervals.formula
```

`activity column add` adds a metric to the activity list. Existing columns stay where they are.

Heart rate, power and pace zones are date ranges. `zones show` prints them. `zones set` adds a range starting on `--from`, or changes the range that already starts that day. A new range copies anchors you leave out from the range that covered that day, so changing resting heart rate does not reset LTHR or maximum heart rate. The first range for a sport still needs `--cp`, `--lthr` or `--cv`. Pace is critical velocity in km/h, for Run or Swim.

```sh
gc-cli -a Joe zones set --type hr --sport Bike --from 2026-01-01 --resthr 40
gc-cli -a Joe zones set --type pace --sport Run --from 2026-01-01 --cv 12.5
```

### Trends charts

`chart library` reads and writes the charts in the Trends sidebar, the athlete's `config/charts.xml`. `chart library list` shows the same charts the sidebar shows. Until a chart is added, edited or removed, that file is not created and the list is the built-in set.

`chart library add` appends a chart. A curve is one of three kinds. `--metric` is a metric from `metric list` (a symbol such as `p_v`, or a formula name such as `Average_Power`). `--best` is a peak over a duration: `--best 45 --unit min --series power` is the 45-minute peak power on CP Analysis. `--estimate` is a value from a critical-power model, `--model cp2`, `cp3` or `ext`, such as `--estimate cp`. A model that does not offer that value is refused. PMC, Banister, performance, formula and measure curves are refused.

With no drawing flags, a metric is stored as GoldenCheetah stores a normal metric curve: a line and a circle for an average, a square for a peak, bars for a total. `--style` is `bar`, `line`, `sticks` or `dots`. `--symbol` is `none`, `circle`, `square`, `diamond`, `triangle`, `cross`, `hexagon` or `star`. `--color` is `RRGGBB`. `--fill` fills under the curve. `--filter` is that curve's own data filter, the same language as the activity filter, so `isRun` limits one series and leaves the chart's filter empty. It is stored as Curve Settings stores a filter, and `--filter ""` removes it. `show` prints the expression as `filter`, or `null` when there is none; a free-text search set in the GUI is printed as `search` and kept as it is. Those flags apply to the one curve on the command. Several `--metric` flags and one `--style` is refused; add each curve with `chart library curve add`. `--by` is `day`, `week`, `month`, `year`, `tod` or `all`, and defaults to `week`.

`chart library show` prints each curve as the Curves table does: its number, its type, what it plots, then the style and the symbol. `chart library edit --metric` (or `--best` or `--estimate`) replaces the whole curve list. `chart library curve edit` changes one curve and leaves the others, including bests and estimates already in the file. The chart is written with the same file format the GUI uses, and it is read back before the file is touched. An unknown metric, an empty or duplicate name, a filter that does not parse, or a chart that does not read back is refused and the file is left unchanged.

When a metric a chart uses is no longer defined (a user metric was removed, or `usermetrics.xml` does not parse), that curve is left out when the charts are read, as the GUI does. Commands that only read leave `charts.xml` alone. A command that changes the charts is refused while any chart has lost a curve that way, because saving would drop it for good; the error names the charts and the metrics. Define the metric again, or pass `--drop-unknown` to save without those curves.

```sh
gc-cli -a Joe chart library add --name "P v" --metric p_v
gc-cli -a Joe chart library add --name "Estimated VO2max" --by day --metric vo2max --style dots --symbol none
gc-cli -a Joe chart library add --name "CP Analysis" --by day --best 45 --unit min --series power --style dots --symbol circle
gc-cli -a Joe chart library curve add "CP Analysis" --estimate cp --model cp2
gc-cli -a Joe chart library show "CP Analysis"
gc-cli -a Joe chart library curve edit "CP Analysis" 1 --style line
gc-cli -a Joe chart library edit "P v" --name "Power and speed" --metric average_power --metric average_speed --by week
gc-cli -a Joe chart library remove "Power and speed"
```

Clicking the chart in the Trends sidebar applies its curves and keeps the view's current grouping and date range. The saved grouping is what `chart library show` reports, and what Chart Setup applies.

Intervals are numbered in the order `interval list` shows them: usually the entire activity, the laps and marked intervals, then the efforts, climbs and segments GoldenCheetah found. Each has a `type`, which `--type` accepts: `user`, `all`, `device`, `peakpower`, `peakpace`, `effort`, `route` or `climb`. Each also has a `group`, the title the sidebar shows (`USER`, `EFFORTS`, `PEAK POWER` ...), and `--type` accepts those titles too.

`interval list`, and an interval tile on `activity overview`, start the text report with one line of counts for the whole activity, including zeros: recorded laps (from the device), user intervals, and discovered efforts (efforts, peaks, climbs and segments). The entire activity is not part of that count. `--type` filters the rows and leaves the counts unchanged. JSON has the same counts as `recorded_laps`, `user_intervals` and `discovered_efforts`. CSV does not include that line; it starts with the table header.

```sh
gc-cli -a Joe interval list last --type user --metric Pace,Average_Power,Average_Heart_Rate,1m_peak_hr --display
gc-cli -a Joe interval show last "Lap 3" --format json
```

## Example: estimating power

This is the job the command line was built for.

Once per athlete, add the numeric fields the processor reads and install the processor:

```sh
gc-cli -a Joe field add "EP Wind Speed" "EP Wind Direction" "EP CdA" "EP Crr" \
    "EP Z0" "EP Anchor W" "EP Bike Weight" --type double --tab "Estimate Power"
gc-cli -a Joe processor install estimate-power --file estimate_power.py
```

Both commands can be run again safely: existing fields are left alone, and installing the same script again changes nothing.

For each new ride:

```sh
gc-cli -a Joe import ~/incoming/        # already imported files are skipped
```

To re-run the estimate on outdoor rides and save the results:

```sh
gc-cli -a Joe processor run estimate-power --filter 'isRun = 0 and Sport = "Bike"'
```

The report lists each activity as processed, skipped (no change) or failed, with the script's output and errors. Processed activities are saved, so the trends and critical power charts use the new values. Use `--dry-run` to run without saving.

## Living with other tools and the GUI

- **Only one GoldenCheetah at a time.** While the GUI has an athlete open, commands on that athlete refuse to run (exit status 4). The same applies to a command running while another command or the REST server is using the athlete. Add `--lock-wait SECS` to wait instead. Older GoldenCheetah versions don't take the lock, so a command also refuses when the athlete is marked as not closed cleanly. If GoldenCheetah really isn't running (it crashed), use `--force` once.
- **Files on disk are the truth.** Every command starts by bringing the athlete up to date with its activity and zone files, the same refresh the GUI does when it opens an athlete. Tools that edit activity files or zones while GoldenCheetah isn't running are picked up by the next command, and metrics and critical power catch up without opening the window. `athlete refresh --rebuild` recomputes everything.
- **No questions.** Nothing waits for a dialog. Problems that the GUI would show in a dialog are reported as errors or warnings, and one-off questions such as downloading the default icons are left for the next time the GUI opens.

## REST API

```sh
GoldenCheetah --cli --home ~/.goldencheetah serve --port 12022 [--token SECRET]
```

This serves every command as JSON under `http://127.0.0.1:12022/v1` until interrupted. Each request opens the athlete, runs the command and closes the athlete again, exactly like a command line run, so the GUI can open the athlete between requests.

`GET /v1/openapi.json` describes all routes. Some examples:

| Method and path | Command |
|---|---|
| `GET /v1/athletes` | `athlete list` |
| `GET /v1/athletes/Joe/activities?filter=isRun%3D0&metric=coggan_tss` | `activity list` |
| `GET /v1/athletes/Joe/activities/last` | `activity show last` |
| `GET /v1/athletes/Joe/activities/last/overview?tile=Intervals%20Data` | `activity overview last --tile "Intervals Data"` |
| `GET /v1/athletes/Joe/layouts/Run/tiles/Intervals%20Data` | `layout tile show "Intervals Data" --layout Run` |
| `PUT /v1/athletes/Joe/layouts/Run/tiles/Intervals%20Data` `{"program": "..."}` | `layout tile set "Intervals Data" --layout Run --program "..."` |
| `GET /v1/athletes/Joe/activities/last/intervals?type=user` | `interval list last --type user` |
| `GET /v1/athletes/Joe/activities/last/intervals/Lap%203` | `interval show last "Lap 3"` |
| `POST /v1/athletes/Joe/imports` (multipart, or the raw file with `?filename=`) | `import` |
| `POST /v1/fields` `{"name": ["EP CdA"], "type": "double"}` | `field add` |
| `PUT /v1/processors/estimate-power` `{"source": "..."}` | `processor install` |
| `POST /v1/athletes/Joe/processors/estimate-power/runs` `{"filter": "isRun = 0"}` | `processor run` |
| `GET /v1/athletes/Joe/charts` | `chart library list` |
| `POST /v1/athletes/Joe/charts` `{"name": "P v", "metric": ["p_v"]}` | `chart library add` |
| `POST /v1/athletes/Joe/charts/CP%20Analysis/curves` `{"best": 45, "unit": "min", "series": "power", "style": "dots", "symbol": "circle"}` | `chart library curve add` |
| `GET /v1/athletes/Joe/activities/last/chart?width=800` | `chart activity`, returns `image/png` |
| `POST /v1/commands/<command>` `{"athlete": "Joe", "args": {...}}` | any command |

Notes:

- Query parameters and JSON body fields have the same names as the command line options. Send JSON bodies with `Content-Type: application/json`.
- Options that read a file where the command runs (`--file` of `processor install`, `metric user` and `layout tile set`) are refused with 400: the server's files aren't the client's to read. Send the content instead (`source`, `program`). Activity files for `import` are uploaded.
- Charts and exports come back as files. Add `?envelope=1` to get the JSON envelope instead.
- Add `?format=csv` to get a result as CSV (`text/csv`), laid out as `--format csv` does. Errors are still JSON.
- HTTP status codes follow the exit status: 400 bad arguments, 404 not found, 409 athlete in use, 422 failed.

The server listens on 127.0.0.1 by default. It refuses requests from web pages on other sites and requests with an unexpected `Host`. Listening on another address requires `--token` (or `$GC_API_TOKEN`), and clients then send `Authorization: Bearer <token>`.

### The older API web service

GoldenCheetah also has an older, separate web service: **Enable API Web Services** in Preferences, Integration, or `GoldenCheetah --server` (see `doc/user/rest-api.txt`). It is read-only, returns CSV on port 12021, and runs inside the GUI. It reads the saved cache as it is and doesn't take the athlete lock, so it keeps answering while the GUI has the athlete open. `serve` refuses (409) an athlete that the GUI has open. Use the older service to read data while the GUI is open. Use `serve` for everything else: changes, imports, processors, charts, and data that is always up to date with the files on disk. Neither service changes the other, and they can run side by side.
