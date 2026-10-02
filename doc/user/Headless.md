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

- Lines end with LF. Values are written as they are: one starting with `=`, `+`, `-` or `@` isn't escaped, so a spreadsheet may read text from an activity (a note, a name) as a formula. Escaping would change negative numbers; import into a spreadsheet as text where that matters.
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
| Athletes | `athlete list`, `athlete create`, `athlete show`, `athlete set`, `athlete refresh [--rebuild]` |
| Import | `import FILE-OR-FOLDER... [--recursive] [--dry-run]`, `formats` |
| Activities | `activity list`, `activity show`, `activity overview [--tile NAME]`, `layout list`, `layout tile list|show|set`, `activity export --as tcx`, `activity set --set 'Field=value'`, `activity delete`, `activity eval --expression '...'`, `activity column list|add|remove` |
| Intervals | `interval list ACTIVITY [--type user,effort] [--metric ...] [--display]`, `interval show ACTIVITY NUMBER-OR-NAME` |
| Fields | `field list`, `field add NAME... --type double --tab TAB`, `field remove` |
| Processors | `processor list`, `processor show`, `processor install NAME --file script.py`, `processor configure`, `processor remove`, `processor run NAME ...` |
| Metrics | `metric list`, `metric user list|show|add|edit|remove`, `metric favourite list|add|remove|set`, `metric aggregate`, `pmc`, `meanmax`, `cp`, `cp estimates` |
| Zones and measures | `zones show`, `zones set`, `zones remove` (`--type power\|hr\|pace`), `zones options`, `zones scheme show\|set`, `measures list`, `measures add`, `measures edit`, `measures remove` |
| Charts | `chart activity`, `chart meanmax`, `chart pmc`, `chart zones [--type power\|hr\|pace\|fatigue]`, `chart trend` (`--as png\|svg\|pdf`, `--width`, `--height`, `--dark`), `chart library list\|show\|add\|edit\|remove`, `chart library curve add\|edit\|remove`, `chart library render\|data` |
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
- Zone tiles give each zone's time and %. As on the overview tile, % is the share of the time in all zones; in `activity show` it is the share of the recording time, as the GUI's zone tables have it, so the two can differ when part of the recording is in no zone.
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

### Athlete settings

`athlete show` prints the athlete's About and Model settings: `nickname`, `dob`, `sex`, `height` (cm), `weight` (the default weight, kg), `crank_length` and `wheel_size` (mm), `wbal_tau` (s), `sts_days` and `lts_days` (the PMC's short and long term stress averages) and `sb_today` (the PMC shows today's stress balance). `weight_today` is the weight GoldenCheetah uses today, and `weight_source` says where it comes from: a Body `measure`, else the `setting`, else the `default` of 75 kg.

`athlete set` changes the settings you pass and leaves the others, as the About and Model tabs of the athlete's settings do:

```sh
gc-cli -a Joe athlete set --weight 71.5 --height 178 --crank-length 172.5 --wheel-size 2096
gc-cli -a Joe athlete set --nickname JJ --dob 1985-04-12 --sex male
gc-cli -a Joe athlete set --sts-days 7 --lts-days 42 --sb-today true --wbal-tau 300
```

The values must be ones the GUI's fields allow: weight and height 0 to 999.9 (kept to one decimal, as the GUI), crank length one of the lengths the About tab lists (130 to 220 mm), wheel size 1 to 9999 mm, W'bal tau 30 to 1200 s, STS 1 to 21 days and LTS 7 to 56 days. A wrong value is refused and nothing is changed.

What a change recomputes is what the GUI recomputes:

- The default weight feeds every metric per kg, for activities without a Body measure or a weight of their own. Those activities are recomputed and `refreshed` says how many.
- Height, wheel size and crank length start the refresh the GUI starts, which recomputes only activities that are out of date for another reason. As in the GUI, a new height doesn't by itself recompute GOVSS, which uses it; `athlete refresh --rebuild` does.
- STS and LTS days change the PMC, which is worked out when it is asked for (`pmc`, `chart pmc`).
- W'bal tau is the tau Train's real time W'bal uses. An activity's W' metrics work out their own tau from the ride (or its `Tau` field), so nothing is recomputed.

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

`zones remove --from DATE` deletes the range that starts that day, as the Delete button under a zones page's ranges does: the range before it then covers its days. The only range of a sport can't be removed, change it with `zones set` instead. A sport without zones of its own (it uses Bike's, and the GUI's page shows no ranges for it) has nothing to remove. Every change to zones is written, read back and the activities it affects are recomputed, and the result says how many (`refreshed`).

```sh
gc-cli -a Joe zones remove --type hr --from 2026-01-01
```

The power zones page has two choices per sport, which `zones show` prints as `cp_model` and `coggan_metrics`, and `zones options` sets:

- `--cp-model manual|cp2|cp3|ext`: Manual, or Semi-Automatic from the CP2, CP3 or Extended model, in which case the GUI offers new ranges from the estimates. The CLI only stores the choice.
- `--coggan-metrics cp|ftp`: "Use CP for all metrics" or "Use FTP for Coggan metrics". It decides whether NP based metrics (IF, TSS ...) use the range's CP or its FTP, and the activities are recomputed.

```sh
gc-cli -a Joe zones options --sport Bike --coggan-metrics ftp
```

The Default tab of each zones page is the zone scheme: the zones' names, descriptions and lower bounds in % of CP, LT or CV, and for heart rate the Trimp k of each zone. `zones scheme show` prints it and `zones scheme set` replaces it, one `--zone NAME,DESCRIPTION,PERCENT` (`,TRIMPK` added for heart rate) per zone, sorted by the lower bound as the GUI sorts them. Every range whose zones come from the default follows the new scheme, existing ranges included, and so does every range added later; a range given zones of its own in the GUI keeps them. As with removing, a sport that uses Bike's zones is refused until it has a range of its own.

```sh
gc-cli -a Joe zones scheme set --type power --zone "Z1,Recovery,0" --zone "Z2,Endurance,56" \
    --zone "Z3,Tempo,76" --zone "Z4,Threshold,91" --zone "Z5,VO2Max,106"
gc-cli -a Joe zones scheme set --type hr --zone "Z1,Easy,0,1" --zone "Z2,Hard,85,2.5"
```

Body weight and other measures are dated readings. `measures add` records one, `measures edit` changes the values you pass of an existing one and keeps the others, and `measures remove` deletes one. `--when` is the time `measures list` prints, or just the date when there is only one reading on it. An edited reading becomes a manual entry (`source` in `measures list`), as in the GUI's measures table; adding a reading at the time of an existing one replaces its values and keeps its source, as the GUI's Add does. Values go from 0 to 9999.99, in metric units. Activities whose weight comes from the changed readings are recomputed.

```sh
gc-cli -a Joe measures add --when 2026-03-01T07:00:00 --set WEIGHTKG=71.5 --set FATPERCENT=14
gc-cli -a Joe measures edit --when 2026-03-01 --set WEIGHTKG=71.2 --comment "after breakfast"
gc-cli -a Joe measures remove --when 2026-03-01
```

### Trends charts

`chart library` reads and writes the charts in the Trends sidebar, the athlete's `config/charts.xml`. `chart library list` shows the same charts the sidebar shows. Until a chart is added, edited or removed, that file is not created and the list is the built-in set.

`chart library add` appends a chart. A curve is one of three kinds. `--metric` is a metric from `metric list` (a symbol such as `p_v`, or a formula name such as `Average_Power`). `--best` is a peak over a duration: `--best 45 --unit min --series power` is the 45-minute peak power on CP Analysis. `--estimate` is a value from a critical-power model, `--model cp2`, `cp3` or `ext`, such as `--estimate cp`. A model that does not offer that value is refused. `--wpk` makes an estimate per kilogram, as Curve Settings' Per Kilogram, and `curve edit` keeps it unless `--wpk` is given again. `--pmc` is a PMC curve of a stress metric (`--stress`, default `coggan_tss`): `sts` (ATL), `lts` (CTL), `sb` (TSB) or `rr` (ramp rate), and the same with `planned-` or `expected-` in front, the planned and expected PMC the Plan view's Expected PMC chart draws. It is named `ATL`, `CTL`, `TSB`, `Ramp Rate` (`Planned ATL`, `Expected ATL` ...) on a `Stress` axis. `--measure` is a daily measure, a field of `measures list` such as `WEIGHTKG` or `Weight`, from `--group` (default `Body`, or `Hrv` ...), named as Curve Settings names it (`Body - Weight`). Banister, performance and formula curves are refused.

Curves with the same units share an axis. A best or an estimate gets the units the built-in charts use: `Watts` for every kind of power, `Joules` for W', `ml/min/kg` for VO2max (`Watts/kg` and `Joules/kg` per kilogram), so a best and CP go on one axis. `--units` sets them for any curve. Flags that belong to another kind of curve (`--series` without `--best`, `--wpk` without `--estimate` ...) are refused, on `chart library edit` too.

With no drawing flags, a metric is stored as GoldenCheetah stores a normal metric curve: a line and a circle for an average, a square for a peak, bars for a total. `--style` is `bar`, `line`, `sticks` or `dots`. `--marker` is `none`, `circle`, `square`, `diamond`, `triangle`, `cross`, `hexagon` or `star`. `--color` is `RRGGBB`. `--fill` fills under the curve. `--filter` is that curve's own data filter, the same language as the activity filter, so `isRun` limits one series and leaves the chart's filter empty. It is stored as Curve Settings stores a filter, and `--filter ""` removes it. `show` prints the expression as `filter`, or `null` when there is none; a free-text search set in the GUI is printed as `search` and kept as it is. Those flags apply to the one curve on the command. Several `--metric` flags and one `--style` is refused; add each curve with `chart library curve add`. `--by` is `day`, `week`, `month`, `year`, `tod` or `all`, and defaults to `week`.

`chart library show` prints each curve as the Curves table does: its number, its type, what it plots, then the style and the marker. `chart library edit --metric` (or `--best` or `--estimate`) replaces the whole curve list. `chart library curve edit` changes one curve and leaves the others, including bests and estimates already in the file. The chart is written with the same file format the GUI uses, and it is read back before the file is touched. An unknown metric, an empty or duplicate name, a filter that does not parse, or a chart that does not read back is refused and the file is left unchanged. The REST routes are `/v1/athletes/<athlete>/charts/<chart>` and `.../curves/<n>`; a chart name containing `/` can't be put in a path, and `GET .../charts/meanmax`, `pmc` and `trend` are the chart images, so use `POST /v1/commands/chart.library.show` (or the other chart library commands) for those names.

The first change writes every chart, the built-in ones included, with their names in the language GoldenCheetah runs in, as the GUI's first save does.

When a metric a chart uses is no longer defined (a user metric was removed, or `usermetrics.xml` does not parse), that curve is left out when the charts are read, as the GUI does. Commands that only read leave `charts.xml` alone. A command that changes the charts is refused while any chart has lost a curve that way, because saving would drop it for good; the error names the charts and the metrics. Define the metric again, or pass `--drop-unknown` to save without those curves.

```sh
gc-cli -a Joe chart library add --name "P v" --metric p_v
gc-cli -a Joe chart library add --name "Estimated VO2max" --by day --metric vo2max --style dots --marker none
gc-cli -a Joe chart library add --name "CP Analysis" --by day --best 45 --unit min --series power --style dots --marker circle
gc-cli -a Joe chart library curve add "CP Analysis" --estimate cp --model cp2
gc-cli -a Joe chart library show "CP Analysis"
gc-cli -a Joe chart library curve edit "CP Analysis" 1 --style line
gc-cli -a Joe chart library edit "P v" --name "Power and speed" --metric average_power --metric average_speed --by week
gc-cli -a Joe chart library remove "Power and speed"
```

`chart library render NAME` draws a chart the way the Trends view draws it: the GUI's own plot draws the curves, axes, legend, zone shading, and season, event and today markers, in the GUI's colours and chart fonts (there is no `--dark`). `--as png|svg|pdf`, `--width` and `--height` (default 1200x600) and `--title` are those of the other `chart` commands. `chart library data NAME` gives the chart's numbers: the rows of the chart's Data Table and of Export Chart Data, a column per curve (with the units the table shows: hours for seconds), one row per group from the first group with data to the last; grouped by day, days whose values are all below 1 are left out, as the GUI's table does. JSON has each row's `date` (ISO, or the label for `all` and `tod`), its `label` as the table shows it, the `values` and their `text` at the metric's precision; text and CSV are the table.

Both take the same options. With no dates the chart covers all dates, like the sidebar's All Dates season: from the first activity to today, or to the last activity when that is later. `--from` and `--to` set the dates, as Chart Setup's custom dates; a weekly chart starts on the Monday before. `--season NAME` uses a season's or a built-in range's dates instead (`--season "Last 3 months"`), as picking it in the sidebar does; the GUI opens on the season picked last (Last 3 months the first time). `--by` groups for this command only, and defaults to the chart's own grouping (a few built-in charts were saved without one; those are grouped by week). `--filter` adds a filter, as the GUI's filter box adds one to a chart: only activities that pass it count, for every curve, on top of each curve's own filter. Estimates and performance curves are computed first when the chart has any. An unknown chart is exit 3. The REST routes are `GET /v1/athletes/<athlete>/charts/<chart>/image` (the body is the image) and `.../data`, with the options as query parameters.

```sh
gc-cli -a Joe chart library add --name "Expected PMC" --by day --pmc expected-lts
gc-cli -a Joe chart library curve add "Expected PMC" --pmc expected-sts
gc-cli -a Joe chart library curve add "Expected PMC" --pmc expected-sb
gc-cli -a Joe chart library add --name Weight --by day --measure WEIGHTKG
gc-cli -a Joe -o pmc.png chart library render "PMC (Coggan)" --from 2026-01-01 --to 2026-06-30
gc-cli -a Joe -o cp.svg chart library render "CP Analysis" --as svg --by month --filter 'isRun = 0'
gc-cli -a Joe --format csv chart library data "PMC (Coggan)" --by week --from 2026-01-01
gc-cli -a Joe -o season.png chart library render "PMC (Coggan)" --season "This Year"
```

Clicking the chart in the Trends sidebar applies its curves and keeps the view's current grouping and date range. The saved grouping is what `chart library show` reports, and what Chart Setup applies.

Intervals are numbered in the order `interval list` shows them: usually the entire activity, the laps and marked intervals, then the efforts, climbs and segments GoldenCheetah found. Each has a `type`, which `--type` accepts: `user`, `all`, `device`, `peakpower`, `peakpace`, `effort`, `route` or `climb`. Each also has a `group`, the title the sidebar shows (`USER`, `EFFORTS`, `PEAK POWER` ...), and `--type` accepts those titles too.

`interval list`, and an interval tile on `activity overview`, start the text report with one line of counts for the whole activity, including zeros: recorded laps (from the device), user intervals, and discovered efforts (efforts, peaks, climbs and segments). The entire activity is not part of that count. `--type` filters the rows and leaves the counts unchanged. JSON has the same counts as `recorded_laps`, `user_intervals` and `discovered_efforts`. CSV does not include that line; it starts with the table header.

```sh
gc-cli -a Joe interval list last --type user --metric Pace,Average_Power,Average_Heart_Rate,1m_peak_hr --display
gc-cli -a Joe interval show last "Lap 3" --format json
```

### Seasons, phases and events

Seasons are the date ranges of the Trends sidebar: the ones you made, stored in the athlete's `config/seasons.xml`, and the built-in ranges (All Dates, This Year, Last 6 weeks ...), which can't be changed. `season list` shows them all with their dates as of today, the seasons you made first, newest at the top, each followed by its phases. `season show` gives the definition, the starting LTS (`seed`) and lowest SB (`low`), the phases and the events. A season is named by its name (any case) or its id; when two have the same name, the command refuses and lists their ids.

`season add NAME` takes what the Edit Date Range dialog can express. The start is a date (`--from`), some weeks, months or years ago (`--start-ago N --start-unit weeks|months|years`, weeks by default), or a length before the end. The end is a date (`--to`), some time ago (`--end-ago N --end-unit ...`), a length after the start, or year to date (`--ytd`: today's day and month in the start's year). `--length` is years, months and days, such as `1y`, `6m`, `10d` or `1y2m3d`, and takes the place of the side that isn't given. `--type` is `season` (the default), `cycle` or `adhoc`; a cycle or an adhoc range has fixed dates, as in the dialog. `--seed` sets the PMC's starting LTS on the first day, and `--low` the lowest SB. A range that would end before it starts is refused, and so is a name another season or range already has.

```sh
gc-cli -a Joe season add "2026 Season" --from 2026-01-01 --to 2026-12-31 --seed 45
gc-cli -a Joe season add "Last quarter" --start-ago 3 --start-unit months --length 3m
gc-cli -a Joe season add "Race build-up" --to 2026-06-14 --length 3m
gc-cli -a Joe season add "This season so far" --from 2026-01-01 --ytd
gc-cli -a Joe season edit "2026 Season" --name "Season 2026" --to 2026-10-31
gc-cli -a Joe season remove "Last quarter"
```

`season edit` changes the side it is given and keeps the other; `--length` alone changes the length, or makes the end a length after the start. Phases and events go on seasons you made with fixed dates, and such a season can't be changed to move with today (`--start-ago`, `--end-ago`, `--ytd`), as in the GUI.

A phase lies within its season and lasts at least a day. `--from` and `--to` default to the season's dates, and `--type` is `phase`, `prep`, `base`, `build` or `camp`. An event is on a day within its season, the season's last day unless `--date` says otherwise, with a priority `A` to `E` or `none`. Events get an id as the GUI gives them; `event edit` and `event remove` take the id, or the name with `--season` when another season has an event of that name. `event list` lists the events by date, `--season`, `--from` and `--to` narrow it.

```sh
gc-cli -a Joe season phase add "Season 2026" Base --type base --to 2026-03-31
gc-cli -a Joe season phase edit "Season 2026" Base --to 2026-04-15
gc-cli -a Joe event add "Season 2026" "Spring classic" --date 2026-04-19 --priority A
gc-cli -a Joe event list --from 2026-04-01
gc-cli -a Joe event edit "Spring classic" --priority B
```

Every change is written the way the sidebar writes `seasons.xml`, in one go, so a failed write (a read-only folder, a full disk) leaves the old file; the command then fails and names the file. A changed seed changes the PMC from the next command on. The REST routes are `/v1/athletes/<athlete>/seasons[/<season>[/phases[/<phase>]]]`, `/v1/athletes/<athlete>/seasons/<season>/events` and `/v1/athletes/<athlete>/events[/<event>]`; ids go in the path URL-encoded, braces and all.

**`--season NAME`** stands for that season's dates, resolved as of today, wherever a command takes `--from` and `--to`: every command that chooses activities (`activity list`, `metric aggregate`, `chart trend`, `meanmax`, `cp` ...), and `pmc`, `chart pmc`, `cp estimates` and `measures list`. It takes a season, a built-in range (`--season "Last 6 weeks"`), a phase as `Season/Phase` or an id. Giving it with `--from` or `--to` is refused.

```sh
gc-cli -a Joe activity list --season "Season 2026/Base"
gc-cli -a Joe metric aggregate --metric total_distance --season "This Year"
```

### PMC: actual, planned and expected

`pmc` and `chart pmc` compute the performance manager as the GUI does. `--series` chooses which:

- `actual` (the default): completed activities.
- `planned`: planned activities only.
- `expected`: what the Plan view's Expected PMC shows: completed activities up to today, planned ones after it (today counts what was done, or what was planned if nothing was), and a planned activity that is already linked to a completed one is not counted again.
- `all` (`pmc` only): all three. The actual values keep their names (`ctl`, `atl`, `tsb`, `rr`, `stress`) and the others are nested as `planned` and `expected` in JSON, and are columns such as `planned.ctl` and `expected.tsb` in the text table and CSV.

`--to` defaults to today; for `planned`, `expected` and `all` it defaults to the last planned day when that is later. `--from` defaults to the start of the data for `pmc` and to half a year before `--to` (or the end of the data) for `chart pmc`. `--sport` and `--filter` limit the PMC to the activities, completed and planned, that pass, as a PMC curve with a data filter does in the GUI; `--season` gives the dates. `chart pmc` marks today with a dashed line.

```sh
gc-cli -a Joe pmc --series expected --metric coggan_tss
gc-cli -a Joe --format csv pmc --series all --from 2026-09-01 > pmc.csv
gc-cli -a Joe chart pmc --series expected --sport Bike -o expected.png
gc-cli -a Joe pmc --filter 'isRun' --metric govss --season "This Year"
```

### Manual entry

`activity add` enters an activity by hand, as Activity > Manual entry does, and writes the same file: the fields go into tags, the numbers into metric overrides, as if each box had been ticked and typed in.

```sh
gc-cli -a Joe activity add --date 2026-10-01 --time 07:30 --sport Run --duration 0:45:00 --distance 9 \
    --avg-hr 142 --rpe 4 --notes "easy, windy"
```

- `--date` is required; it can't be in the future (that's a plan) or before 2000, as in the wizard. `--time` is `hh:mm`; without it the activity starts four hours ago, as the wizard's default.
- `--sport` is required. `--subsport`, `--workout-code`, `--notes` and `--title` (the Route field) are optional, as is `--rpe` (0 to 10).
- `--duration` is `h:mm:ss` or seconds. `--distance` is in km for every sport, swims too: 1500 m is `--distance 1.5`. The wizard asks for swims in m or yd; the file holds km either way.
- `--avg-hr`, `--avg-cadence`, `--avg-power`, `--work` (kJ), `--bikestress`, `--bikescore`, `--swimscore`, `--triscore`, `--elevation-gain` (m), `--isopower` and `--xpower` take whole numbers, within what the wizard's fields take (for example 0 to 250 bpm).
- Work and stress are estimated as the wizard's Estimate by does: from the athlete's activities of the same sport in the last `--estimate-days` days (all of them when there are none), per hour (`--estimate time`) or per km (`--estimate distance`). The defaults are what the wizard was last used with (Preferences keep them); without that, nothing is estimated. Giving `--work` or a stress value enters them by hand, as the wizard's Manually, and can't be combined with `--estimate time` or `distance`. The command line reads these settings but doesn't change them.
- The wizard's laps editor for runs and swims isn't offered: enter the totals.
- A start another activity already has is refused. The wizard would overwrite that activity after a warning.

The result is the new activity, as `activity list` shows it, with its fields and overrides.

### Planned activities

Planned activities are activity files in the athlete's `planned` folder. They have no samples, only the expected values as overrides. Activity commands work on them with `--planned`: `activity show --planned`, `activity export --planned`, `activity set --planned`, `activity delete --planned`, and the other commands that select activities (`activity list --planned`, `metric aggregate --planned` ...). With `--planned`, a date, a start time, `first` and `last` mean planned activities. A file name finds either kind; when a planned and a completed activity have the same file name, `--planned` chooses the planned one. `activity delete` refuses such a pair, because GoldenCheetah deletes by file name; move the planned one first.

`plan add` plans an activity, as Activity > Plan activity does. It takes the same options as `activity add`, except `--rpe`, plus `--objective` and `--workout`. The date is today or later and the time defaults to 16:00, as in the wizard. The file records the day it was planned for (Original Date), which moving it keeps.

```sh
gc-cli -a Joe plan add --date 2026-10-12 --sport Bike --duration 1:30:00 --title "Tempo" --bikestress 85
gc-cli -a Joe plan add --date 2026-10-14 --workout FTPCheckup
```

`--workout` is a workout of the workout library (the Train view's list): its file, its file name or its title. The activity is then a Bike activity named after the workout, its description is added to the notes, and an erg workout sets the duration, average power, IsoPower, xPower, BikeStress and BikeScore, as the wizard does. What the workout decides can't be given as well (`--duration` with an erg workout, `--distance` with a slope one, `--title`, `--sport` other than Bike).

`plan list` is the agenda: the planned activities from today on, or `--from`/`--to`, or `--all`. Each has its day and time, sport, title and workout code, the expected duration, distance and BikeStress, the completed activity it is linked to (`linked`), and the day it was first planned for when it was moved (`original_date`). `--metric` adds metrics, as in `activity list`. Text, JSON and CSV as the other lists.

The calendar's actions:

| In the calendar | On the command line |
|---|---|
| drag a planned activity to another day | `plan move ID --to DATE [--time hh:mm]` |
| copy, then paste on a day | `plan copy ID --to DATE [--time hh:mm]` |
| link to an activity | `plan link PLANNED ACTUAL` |
| unlink | `plan unlink ID` (either side) |
| insert rest day / delete rest day | `plan shift --from DATE --days N` (N > 0 inserts N days, N < 0 deletes them) |
| Repeat plan... | `plan repeat --from A --to B --start C` |
| Export plan... | `plan export --from A --to B --name NAME -o plan.gcplan` |
| Import plan... | `plan import plan.gcplan --start DATE` |

They make the same checks and the same changes as the calendar:

- A move or copy to a start that is taken is refused. A move keeps Original Date. A copy is a new plan, so its Original Date is its own day, and it isn't linked.
- Linking is refused when either activity is already linked, or both are planned. Unlinking clears the link on both sides. Importing an activity links it to an unlinked planned activity of the same sport on the same day, as the GUI import does.
- `plan shift` moves every planned activity from `--from` on. As deleting a rest day, it never moves anything to before `--from`: with `--days -5` and the first planned activity two days after `--from`, everything moves back two days. Linked activities keep their link.
- `plan repeat` copies the planned activities of `--from` .. `--to` to the same days counted from `--start`, as the Repeat Plan wizard with its defaults: the activities as originally planned (`--current` takes them on their current day), keeping the days before the first and after the last (`--no-gaps` drops them). Unlinked planned activities already in the new period are deleted first; a copy that would start when a linked planned activity does is left out, and so is the second of two activities that would start at the same time. `--start` must come after `--to`.
- `plan export` writes a plan bundle (`.gcplan`) of the period's planned activities and the workouts they use, as the Export Plan wizard does; power targets are stored relative to the athlete's CP, as there. `--name` is required, `--author` is the athlete's name unless given, and `--copyright` and `--description` (Markdown) are optional. In the description `$NAME`, `$AUTHOR`, `$SPORT` and `$COPYRIGHT` are filled in. Without `-o` the file is named after the plan; over REST the bundle is the response.
- `plan import` adds a bundle's activities from `--start` on, scaled to the athlete's CP, and adds their workouts to the workout library. The bundle's leading days are kept unless `--no-gap-days`. As in the wizard, unlinked planned activities already in the plan's period are deleted.

The calendar's weekly summary is `calendar summary`: per week from `--from` (or per `--days N`), the number of activities, distance, BikeStress and duration, or the `--metric` given. Planned activities count as the calendar's Include Planned setting says: `--planned always` (the default), `upcoming-or-missed` (those not linked), `upcoming` (not linked, from today) or `never`. Start `--from` on the first day of your calendar week to get the calendar's weeks.

```sh
gc-cli -a Joe calendar summary --from 2026-09-28 --to 2026-11-01
gc-cli -a Joe --format csv calendar summary --from 2026-09-28 --to 2026-11-01 --planned never
```

`plan adherence --from DATE [--to DATE]` is the Plan Adherence chart as data. Each planned activity planned for a day of the period (the day first planned for, so a moved one stays where it was planned) is `on time` when it was done that day, `done` when it was done on another day (`done_after` days later, negative for earlier), `missed` when it wasn't done and its day has passed, or `upcoming`; `moved_by` is how far it was moved. A planned activity counts as done when it is linked to a completed one. Completed activities that aren't linked to a plan are `unplanned`. `totals` has the chart's figures: total, planned, on time, moved, missed and unplanned, each as a count and a percentage, and the average and total days moved.

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

- **Only one GoldenCheetah at a time.** While the GUI has an athlete open, commands on that athlete refuse to run (exit status 4). The same applies to a command running while another command or the REST server is using the athlete. Add `--lock-wait SECS` to wait instead. Older GoldenCheetah versions don't take the lock, so a command also refuses when the athlete is marked as not closed cleanly. If GoldenCheetah really isn't running (it crashed), use `--force` once. The lock is per machine: it is not kept in the athlete folder, so a folder synced between computers (Dropbox, OneDrive) doesn't carry it, and doesn't stop two computers using the same athlete at once either.
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
| `POST /v1/athletes/Joe/charts/CP%20Analysis/curves` `{"best": 45, "unit": "min", "series": "power", "style": "dots", "marker": "circle"}` | `chart library curve add` |
| `GET /v1/athletes/Joe/activities/last/chart?width=800` | `chart activity`, returns `image/png` |
| `POST /v1/commands/<command>` `{"athlete": "Joe", "args": {...}}` | any command |

Notes:

- Query parameters and JSON body fields have the same names as the command line options. Send JSON bodies with `Content-Type: application/json`.
- Options that read a file where the command runs (`--file` of `processor install`, `metric user` and `layout tile set`) are refused with 400: the server's files aren't the client's to read. Send the content instead (`source`, `program`). Activity files for `import` are uploaded, as multipart form data or as the raw file. Send a raw file with `Content-Type: application/octet-stream` (curl's `--data-binary` alone sends it as a form, which the server has to take apart again), e.g. `curl --data-binary @ride.fit -H "Content-Type: application/octet-stream" "http://127.0.0.1:12022/v1/athletes/Joe/imports?filename=ride.fit"`.
- Charts and exports come back as files. Add `?envelope=1` to get the JSON envelope instead.
- `format` and `envelope` are read by the server for every request, so no command has parameters with those names.
- Path segments are URL-decoded before routing, so a name in the path can't contain `/`, not even as `%2F`. Activity ids never do; for other names, pass the value in the query or body (for example with `POST /v1/commands/<command>`).
- Add `?format=csv` to get a result as CSV (`text/csv`), laid out as `--format csv` does. Errors are still JSON.
- HTTP status codes follow the exit status: 400 bad arguments, 404 not found, 409 athlete in use, 422 failed.

The server listens on 127.0.0.1 by default. It refuses requests from web pages on other sites and requests with an unexpected `Host`. Listening on another address requires `--token` (or `$GC_API_TOKEN`), and clients then send `Authorization: Bearer <token>`.

### The older API web service

GoldenCheetah also has an older, separate web service: **Enable API Web Services** in Preferences, Integration, or `GoldenCheetah --server` (see `doc/user/rest-api.txt`). It is read-only, returns CSV on port 12021, and runs inside the GUI. It reads the saved cache as it is and doesn't take the athlete lock, so it keeps answering while the GUI has the athlete open. `serve` refuses (409) an athlete that the GUI has open. Use the older service to read data while the GUI is open. Use `serve` for everything else: changes, imports, processors, charts, and data that is always up to date with the files on disk. Neither service changes the other, and they can run side by side.
