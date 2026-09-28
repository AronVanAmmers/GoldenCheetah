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

Charts and exports are written to `--output FILE`, or to a file named after the activity or chart when that option is not given. `-o -` writes to stdout.

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
| Activities | `activity list`, `activity show`, `activity export --as tcx`, `activity set --set 'Field=value'`, `activity delete`, `activity eval --expression '...'` |
| Intervals | `interval list ACTIVITY [--type user,effort] [--metric ...]`, `interval show ACTIVITY NUMBER-OR-NAME` |
| Fields | `field list`, `field add NAME... --type double --tab TAB`, `field remove` |
| Processors | `processor list`, `processor show`, `processor install NAME --file script.py`, `processor configure`, `processor remove`, `processor run NAME ...` |
| Metrics | `metric list`, `metric aggregate`, `pmc`, `meanmax`, `cp`, `cp estimates` |
| Zones and measures | `zones show`, `zones set`, `measures list`, `measures add` |
| Charts | `chart activity`, `chart meanmax`, `chart pmc`, `chart zones [--type power\|hr\|pace\|fatigue]`, `chart trend` (`--as png\|svg\|pdf`, `--width`, `--height`, `--dark`) |
| Server | `serve` (see [REST API](#rest-api)) |

### Choosing activities

Commands that work on several activities accept the same selection options:

- **Names**: activity ids as arguments. An id can be a file name (`2026_03_01_09_30_00`), a start time (`2026-03-01T09:30:00`), a date (when only one activity starts on it), `first` or `last`.
- **`--filter EXPR`**: a filter as typed in the GUI filter box, e.g. `isRun = 0` or `Sport = "Bike" and Average_Power > 200`.
- **`--search TEXT`**: a free text search, as in the GUI search box.
- **`--named NAME`**: a search or filter saved in the GUI.
- **`--from DATE --to DATE`**, **`--sport Bike`**, **`--limit N`** (the most recent N), **`--planned`**.

All given criteria must match. `processor run` and `activity set` change nothing unless activities are chosen or `--all` is given.

### What the activity view shows

| In the GUI | On the command line |
|---|---|
| Date, Sport, Workout Code, Notes, RPE and other fields | `activity show` (`metadata`) |
| Totals, Averages, Maximum, Metrics and single metric tiles | `activity show` (`metrics`, every metric with a value, keyed by symbol; see `metric list` for names and units) |
| Power, heart rate, pace and W' balance (fatigue) zone tables | `activity show` (`zones`: name, description, low, high, time and % of the recording time, as the GUI) |
| Form, Fitness, Fatigue and Risk | `activity show` (`pmc`: `tsb`, `ctl`, `atl`, `rr` on the day, from GOVSS for runs, SwimScore for swims and TSS otherwise; `--pmc-metric` to choose) |
| Intervals sidebar and interval tables | `interval list` (the sidebar's metrics, or `--metric`), `interval show` (every metric) |
| Route and data series | `activity export --as gpx`, `--as csv` or `--as json` |
| Zone and PMC charts | `chart zones`, `chart pmc`, `chart activity` |

Intervals are numbered in the order `interval list` shows them: usually the entire activity, the laps and marked intervals, then the efforts, climbs and segments GoldenCheetah found. `--type` takes `user`, `all`, `device`, `peakpower`, `peakpace`, `effort`, `route` or `climb`.

```sh
gc-cli -a Joe interval list last --type user --metric pace,average_power,average_hr,1m_peak_hr
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
| `GET /v1/athletes/Joe/activities/last/intervals?type=user` | `interval list last --type user` |
| `GET /v1/athletes/Joe/activities/last/intervals/Lap%203` | `interval show last "Lap 3"` |
| `POST /v1/athletes/Joe/imports` (multipart, or the raw file with `?filename=`) | `import` |
| `POST /v1/fields` `{"name": ["EP CdA"], "type": "double"}` | `field add` |
| `PUT /v1/processors/estimate-power` `{"file": "/path/script.py"}` | `processor install` |
| `POST /v1/athletes/Joe/processors/estimate-power/runs` `{"filter": "isRun = 0"}` | `processor run` |
| `GET /v1/athletes/Joe/activities/last/chart?width=800` | `chart activity`, returns `image/png` |
| `POST /v1/commands/<command>` `{"athlete": "Joe", "args": {...}}` | any command |

Notes:

- Query parameters and JSON body fields have the same names as the command line options. Send JSON bodies with `Content-Type: application/json`.
- Charts and exports come back as files. Add `?envelope=1` to get the JSON envelope instead.
- HTTP status codes follow the exit status: 400 bad arguments, 404 not found, 409 athlete in use, 422 failed.

The server listens on 127.0.0.1 by default. It refuses requests from web pages on other sites and requests with an unexpected `Host`. Listening on another address requires `--token` (or `$GC_API_TOKEN`), and clients then send `Authorization: Bearer <token>`.
