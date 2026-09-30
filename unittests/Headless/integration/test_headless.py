#!/usr/bin/env python3
#
# End to end tests for the headless GoldenCheetah: the command line
# (GoldenCheetah --cli) and the REST server (--cli serve) are run as real
# processes against temporary athlete folders built from the files in test/.
#
# Usage:
#   python3 unittests/Headless/integration/test_headless.py [-v] [-k pattern]
#
# The binary defaults to src/GoldenCheetah (src/GoldenCheetah.app/... on
# macOS), override with GC_BINARY. Only the standard library is needed.
#

import csv
import io
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TESTDATA = os.path.join(ROOT, "test")


def default_binary():
    for candidate in ("src/GoldenCheetah", "src/GoldenCheetah.app/Contents/MacOS/GoldenCheetah",
                      "src/release/GoldenCheetah.exe"):
        path = os.path.join(ROOT, candidate)
        if os.path.exists(path):
            return path
    return os.path.join(ROOT, "src", "GoldenCheetah")


BINARY = os.environ.get("GC_BINARY", default_binary())

# activity files used throughout, all from the repository's test data
RIDE_POWER = os.path.join(TESTDATA, "rides", "Garmin830_with_Stages.fit")        # bike, power, hr, cadence
RIDE_GPS = os.path.join(TESTDATA, "rides", "2012_01_11_11_51_01.fit")            # bike, gps, hr
RUN_STRYD = os.path.join(TESTDATA, "roundtrip", "Fr955v19.28andStryd.fit")       # run with power
MULTI_TCX = os.path.join(TESTDATA, "rides", "activity_165850259_multi-session.tcx")  # 4 activities
TCX_RIDE = os.path.join(TESTDATA, "rides", "2009_08_30_21_17_25.tcx")

EP_FIELDS = ["EP Wind Speed", "EP Wind Direction", "EP CdA", "EP Crr", "EP Z0", "EP Anchor W", "EP Bike Weight"]

EP_SCRIPT = """
ws = GC.getTag("EP Wind Speed")
w = float(ws) if ws else 0.0
GC.setTag("EP Anchor W", str(200 + w))
print("estimated with wind", w)
"""

BAD_SCRIPT = 'raise ValueError("boom")\n'


class Result:
    def __init__(self, proc):
        self.code = proc.returncode
        self.out = proc.stdout
        self.err = proc.stderr

    def json(self):
        return json.loads(self.out)

    def __repr__(self):
        return "exit %d\nstdout:\n%s\nstderr:\n%s" % (self.code, self.out.decode(errors="replace")[:2000],
                                                     self.err.decode(errors="replace")[:2000])


class Headless(unittest.TestCase):
    """Base: a fresh athletes folder per test class"""

    athlete = "Tester"

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(BINARY):
            raise unittest.SkipTest("GoldenCheetah binary not found at %s (set GC_BINARY)" % BINARY)
        cls.tmp = tempfile.mkdtemp(prefix="gc-headless-")
        cls.home = os.path.join(cls.tmp, "athletes")
        cls.env = dict(os.environ)
        cls.env["HOME"] = os.path.join(cls.tmp, "userhome")   # isolate system settings
        # the app picks a platform itself (offscreen, else the native one: a
        # deployed macOS bundle only ships cocoa)
        cls.env.pop("QT_QPA_PLATFORM", None)
        cls.env.pop("GC_HOME", None)
        os.makedirs(cls.env["HOME"])
        r = cls.gc_class("athlete", "create", cls.athlete, "--cp", "250", "--weight", "70")
        assert r.code == 0, r

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def gc_class(cls, *args, timeout=180, input=None, extra=None):
        cmd = [BINARY, "--cli", "--home", cls.home] + (extra or []) + [str(a) for a in args]
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout, env=cls.env, input=input)
        return Result(proc)

    def gc(self, *args, **kw):
        return self.gc_class(*args, **kw)

    def gcj(self, *args, expect=0, **kw):
        """run with --format json, check the exit status, return the envelope"""
        r = self.gc_class("--format", "json", "--athlete", self.athlete, *args, **kw)
        self.assertEqual(r.code, expect, r)
        return r.json()

    @property
    def folder(self):
        return os.path.join(self.home, self.athlete)

    def activity_files(self):
        return sorted(f for f in os.listdir(os.path.join(self.folder, "activities")) if f.endswith(".json"))

    def assertClosed(self):
        """a command must never leave the athlete open"""
        self.assertFalse(os.path.exists(os.path.join(self.folder, "athlete.lock")), "athlete lock left behind")
        with open(os.path.join(self.folder, "config", "athlete-general.ini")) as f:
            self.assertNotIn("safeexit=false", f.read())


class TestBasics(Headless):

    def test_help_and_usage(self):
        r = self.gc("--help")
        self.assertEqual(r.code, 0)
        self.assertIn(b"Usage: GoldenCheetah --cli", r.out)
        self.assertIn(b"processor run", r.out)
        self.assertIn(b"text, json or csv", r.out)

        r = self.gc("help", "processor", "run")
        self.assertEqual(r.code, 0)
        self.assertIn(b"--filter", r.out)
        self.assertIn(b"REST: POST /v1/athletes/{athlete}/processors/{name}/runs", r.out)

    def test_usage_errors_exit_2(self):
        self.assertEqual(self.gc("fly").code, 2)
        self.assertEqual(self.gc("activity").code, 2)
        self.assertEqual(self.gc("activity", "list", "--colour", "red").code, 2)
        r = self.gc("--athlete", self.athlete, "activity", "list", "--from", "yesterday")
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"yyyy-mm-dd", r.err)

    def test_version(self):
        env = self.gcj("version")
        self.assertTrue(env["ok"])
        self.assertGreater(env["data"]["metrics"], 100)
        self.assertGreater(env["data"]["import_formats"], 20)

    def test_commands_are_described(self):
        env = self.gcj("commands")
        names = {c["name"] for c in env["data"]["commands"]}
        for n in ("import", "field.add", "processor.install", "processor.run", "activity.list", "chart.pmc"):
            self.assertIn(n, names)

    def test_athlete_list_and_show(self):
        env = self.gcj("athlete", "list")
        self.assertEqual([a["name"] for a in env["data"]["athletes"]], [self.athlete])
        env = self.gcj("athlete", "show")
        self.assertEqual(env["data"]["weight"], 70)
        self.assertEqual(env["data"]["activities"], 0)
        self.assertClosed()

    def test_create_existing_athlete_fails(self):
        r = self.gc("athlete", "create", self.athlete)
        self.assertEqual(r.code, 5, r)

    def test_unknown_athlete(self):
        r = self.gc("--athlete", "Nobody", "activity", "list")
        self.assertEqual(r.code, 3, r)

    def test_single_athlete_is_the_default(self):
        r = self.gc("activity", "list")
        self.assertEqual(r.code, 0, r)

    def test_athlete_names_are_not_paths(self):
        for name in ("..", "../x", "/etc", ".hidden"):
            r = self.gc("--athlete", name, "activity", "list")
            self.assertEqual(r.code, 2, (name, r))
        os.makedirs(os.path.join(self.home, "NotAnAthlete", "random"))
        r = self.gc("--athlete", "NotAnAthlete", "activity", "list")
        self.assertEqual(r.code, 3, r)
        self.assertIn(b"not an athlete folder", r.err)
        # and nothing was created in it
        self.assertEqual(os.listdir(os.path.join(self.home, "NotAnAthlete")), ["random"])

    def test_shared_settings_need_an_athletes_folder(self):
        missing = os.path.join(self.tmp, "missing")
        r = subprocess.run([BINARY, "--cli", "--home", missing, "field", "add", "Foo"], capture_output=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 3, r)
        r = subprocess.run([BINARY, "--cli", "--home", missing, "processor", "install", "x", "--source", "1"],
                           capture_output=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 3, r)
        self.assertFalse(os.path.exists(missing))

    def test_formats(self):
        env = self.gcj("formats")
        suffixes = {f["suffix"]: f for f in env["data"]["formats"]}
        self.assertTrue(suffixes["fit"]["import"])
        self.assertTrue(suffixes["tcx"]["export"])

    def test_metric_list(self):
        env = self.gcj("metric", "list", "--search", "tss")
        symbols = [m["symbol"] for m in env["data"]["metrics"]]
        self.assertIn("coggan_tss", symbols)


class TestImport(Headless):

    def test_import_fit_like_the_gui(self):
        count = len(self.activity_files())
        env = self.gcj("import", RIDE_POWER)
        data = env["data"]
        self.assertEqual(data["imported"], 1)
        item = data["files"][0]
        self.assertEqual(item["status"], "imported")
        self.assertEqual(item["sport"], "Bike")
        activity = item["activity"]

        # saved as GoldenCheetah json named after the start time
        self.assertIn(activity + ".json", self.activity_files())
        # the original is kept in imports, as the GUI does
        imports = os.listdir(os.path.join(self.folder, "imports"))
        self.assertTrue(any(f.endswith("_%s.fit" % activity) for f in imports), imports)

        # samples, laps and the metadata filled in on import
        show = self.gcj("activity", "show", activity)["data"]
        self.assertGreater(show["samples"], 1000)
        meta = show["metadata"]
        self.assertEqual(meta["Sport"], "Bike")
        self.assertEqual(meta["Athlete"], self.athlete)
        self.assertEqual(meta["Filename"], activity + ".json")
        self.assertTrue(meta["Source Filename"].endswith(".fit"))
        self.assertIn("P", meta["Data"])
        self.assertTrue(any(i["type"] != "ALL" for i in show["intervals"]) or len(show["intervals"]) >= 1)
        self.assertGreater(show["metrics"]["average_power"], 50)
        self.assertClosed()

        # and again: already imported, skipped
        env = self.gcj("import", RIDE_POWER)
        self.assertEqual(env["data"]["skipped"], 1)
        self.assertEqual(env["data"]["files"][0]["status"], "skipped")
        self.assertEqual(len(self.activity_files()), count + 1)

    def test_multi_activity_file(self):
        env = self.gcj("import", MULTI_TCX)
        self.assertEqual(env["data"]["imported"], 4)
        self.assertTrue(all("#" in f["source"] for f in env["data"]["files"]))

    def test_folder_and_dry_run(self):
        folder = os.path.join(self.tmp, "incoming")
        os.makedirs(folder, exist_ok=True)
        shutil.copy(RIDE_GPS, folder)
        shutil.copy(TCX_RIDE, folder)
        with open(os.path.join(folder, "notes.doc"), "w") as f:
            f.write("not an activity")

        before = self.activity_files()
        env = self.gcj("import", "--dry-run", folder)
        self.assertEqual(env["data"]["importable"], 2)
        self.assertEqual(self.activity_files(), before)  # nothing written

        env = self.gcj("import", folder)
        self.assertEqual(env["data"]["imported"], 2)

    def test_bad_files(self):
        broken = os.path.join(self.tmp, "broken.fit")
        with open(broken, "wb") as f:
            f.write(b"\x00" * 100)
        r = self.gc("--athlete", self.athlete, "import", broken, "/does/not/exist.fit")
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"file not found", r.out)

        # a mix of good and bad is a partial success
        r = self.gc("--athlete", self.athlete, "import", broken, RUN_STRYD)
        self.assertEqual(r.code, 1, r)
        self.assertIn(b"1 imported", r.out)
        self.assertClosed()


class TestEstimatePowerWorkflow(Headless):
    """The workflow the command line was built for, in order"""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        env = json.loads(cls.gc_class("--format", "json", "version").out)
        cls.python = env["data"]["python"]
        cls.script = os.path.join(cls.tmp, "estimate_power.py")
        with open(cls.script, "w") as f:
            f.write(EP_SCRIPT)
        cls.bad = os.path.join(cls.tmp, "bad.py")
        with open(cls.bad, "w") as f:
            f.write(BAD_SCRIPT)

    def setUp(self):
        if not self.python:
            self.skipTest("embedded Python not available")

    def test_1_once_per_athlete(self):
        env = self.gcj("field", "add", *EP_FIELDS, "--type", "double", "--tab", "Estimate Power")
        self.assertEqual(env["data"]["added"], len(EP_FIELDS))
        # again: nothing to do
        env = self.gcj("field", "add", *EP_FIELDS, "--type", "double", "--tab", "Estimate Power")
        self.assertEqual(env["data"]["unchanged"], len(EP_FIELDS))
        fields = {f["name"]: f for f in self.gcj("field", "list", "--tab", "Estimate Power")["data"]["fields"]}
        for name in EP_FIELDS:
            self.assertEqual(fields[name]["type"], "double")
            self.assertTrue(fields[name]["numeric"])

        env = self.gcj("processor", "install", "estimate-power", "--file", self.script)
        self.assertEqual(env["data"]["status"], "installed")
        env = self.gcj("processor", "install", "estimate-power", "--file", self.script)
        self.assertEqual(env["data"]["status"], "unchanged")
        self.gcj("processor", "install", "estimate-power", "--source", "print(1)", expect=5)
        names = [p["id"] for p in self.gcj("processor", "list", "--type", "python")["data"]["processors"]]
        self.assertIn("estimate-power", names)
        self.assertEqual(self.gcj("processor", "show", "estimate-power")["data"]["source"].strip(), EP_SCRIPT.strip())

    def test_2_each_new_ride(self):
        env = self.gcj("import", RIDE_POWER, RIDE_GPS, RUN_STRYD)
        self.assertEqual(env["data"]["imported"], 3)
        sports = sorted(f["sport"] for f in env["data"]["files"])
        self.assertEqual(sports, ["Bike", "Bike", "Run"])

    def test_3_rerun_the_estimate(self):
        run = [a["id"] for a in self.gcj("activity", "list", "--filter", "isRun=1")["data"]["activities"]][0]
        self.gcj("activity", "set", run, "--set", "EP Wind Speed=3.5")

        # choose like the GUI filter: outdoor rides only (not runs)
        r = self.gc("--athlete", self.athlete, "processor", "run", "estimate-power", "--filter", "isRun=0")
        self.assertEqual(r.code, 0, r)
        self.assertIn(b"2 processed, 0 skipped, 0 failed", r.out)
        self.assertIn(b"estimated with wind 0.0", r.out)

        # saved: in the files on disk, not just in memory
        for item in self.gcj("activity", "list", "--filter", "isRun=0", "--field", "EP Anchor W")["data"]["activities"]:
            self.assertEqual(item["metadata"]["EP Anchor W"].strip(), "200.0")
            with open(os.path.join(self.folder, "activities", item["file"])) as f:
                self.assertIn('"EP Anchor W"', f.read())

        # named activities
        env = self.gcj("processor", "run", "estimate-power", run)
        self.assertEqual(env["data"]["processed"], 1)
        self.assertEqual(env["data"]["activities"][0]["output"], "estimated with wind 3.5")
        item = self.gcj("activity", "list", run, "--field", "EP Anchor W")["data"]["activities"][0]
        self.assertEqual(item["metadata"]["EP Anchor W"].strip(), "203.5")
        self.assertClosed()

    def test_4_errors_are_reported(self):
        self.gcj("processor", "install", "broken", "--file", self.bad)
        r = self.gc("--athlete", self.athlete, "processor", "run", "broken", "last")
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"failed", r.out)
        self.assertIn(b"ValueError: boom", r.out)

        # a run on everything needs to be asked for
        r = self.gc("--athlete", self.athlete, "processor", "run", "estimate-power")
        self.assertEqual(r.code, 2, r)
        r = self.gc("--athlete", self.athlete, "processor", "run", "no-such-processor", "--all")
        self.assertEqual(r.code, 3, r)
        r = self.gc("--athlete", self.athlete, "processor", "run", "estimate-power", "--filter", "isRun=(")
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"bad filter", r.err)

    def test_4b_script_that_changes_nothing_is_skipped(self):
        self.gcj("processor", "install", "noop", "--source", 'print("looked")\n')
        env = self.gcj("processor", "run", "noop", "--all")
        self.assertEqual(env["data"]["processed"], 0)
        self.assertEqual(env["data"]["skipped"], 3)
        self.assertEqual(env["data"]["activities"][0]["output"], "looked")

    def test_5_dry_run_saves_nothing(self):
        path = os.path.join(self.folder, "activities", self.activity_files()[0])
        before = open(path, "rb").read()
        env = self.gcj("processor", "run", "estimate-power", "--all", "--dry-run")
        self.assertFalse(env["data"]["saved"])
        self.assertEqual(open(path, "rb").read(), before)

    def test_6_builtin_processor(self):
        env = self.gcj("processor", "run", "fixspikes", "--sport", "Bike")
        self.assertEqual(env["data"]["failed"], 0)
        self.assertEqual(len(env["data"]["activities"]), 2)


class TestLivesWithOtherTools(Headless):
    """fill-wind and fill-cp edit the folder while GoldenCheetah isn't running"""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        r = cls.gc_class("--athlete", cls.athlete, "import", RIDE_POWER)
        assert r.code == 0, r
        cls.activity = "2020_01_26_13_00_38"

    def test_edited_activity_file_is_used(self):
        path = os.path.join(self.folder, "activities", self.activity + ".json")
        time.sleep(1.1)  # mtime resolution
        with open(path, encoding="utf-8-sig") as f:
            doc = json.load(f)
        doc["RIDE"]["TAGS"]["Notes"] = "edited by another tool"
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f)

        env = self.gcj("activity", "list", self.activity, "--field", "Notes")
        self.assertEqual(env["data"]["activities"][0]["metadata"]["Notes"], "edited by another tool")

    def test_edited_zones_recompute_metrics(self):
        before = self.gcj("activity", "list", self.activity, "--metric", "coggan_tss")["data"]["activities"][0]
        zones = os.path.join(self.folder, "config", "power.zones")
        text = open(zones).read()
        self.assertIn("CP=250", text)
        with open(zones, "w") as f:
            f.write(text.replace("CP=250", "CP=200").replace("FTP=250", "FTP=200"))

        refreshed = self.gcj("athlete", "show")["data"]["refreshed"]
        self.assertGreaterEqual(refreshed, 1)
        after = self.gcj("activity", "list", self.activity, "--metric", "coggan_tss")["data"]["activities"][0]
        self.assertGreater(after["metrics"]["coggan_tss"], before["metrics"]["coggan_tss"])
        self.assertEqual(self.gcj("zones", "show")["data"]["power"]["ranges"][0]["cp"], 200)

        # put it back for the other tests
        with open(zones, "w") as f:
            f.write(text)

    @unittest.skipIf(os.name == "nt", "fakes a lock held by a POSIX 'sleep'; the AthleteLock unit test covers Windows")
    def test_refuses_while_another_process_holds_the_athlete(self):
        holder = subprocess.Popen(["sleep", "60"])
        try:
            lock = os.path.join(self.folder, "athlete.lock")
            with open(lock, "w") as f:
                f.write("%d\nsleep\n%s\n" % (holder.pid, self.gcj("version")["data"]["host"]))
            r = self.gc("--athlete", self.athlete, "activity", "list")
            self.assertEqual(r.code, 4, r)
            self.assertIn(b"in use", r.err)
            # the folder is untouched
            self.assertTrue(os.path.exists(lock))
        finally:
            holder.kill()
            holder.wait()
        # the holder died: its lock is stale and taken over
        r = self.gc("--athlete", self.athlete, "activity", "list")
        self.assertEqual(r.code, 0, r)
        self.assertClosed()

    def test_refuses_when_gui_did_not_close_cleanly(self):
        ini = os.path.join(self.folder, "config", "athlete-general.ini")
        text = open(ini).read()
        with open(ini, "w") as f:
            f.write(text.replace("safeexit=true", "safeexit=false"))
        try:
            r = self.gc("--athlete", self.athlete, "activity", "list")
            self.assertEqual(r.code, 4, r)
            self.assertIn(b"--force", r.err)
            r = self.gc("--athlete", self.athlete, "--force", "activity", "list")
            self.assertEqual(r.code, 0, r)
        finally:
            with open(ini, "w") as f:
                f.write(text)

    def test_zones_and_measures_commands(self):
        before = self.gcj("activity", "list", self.activity, "--metric", "coggan_tss")["data"]["activities"][0]["metrics"]["coggan_tss"]
        env = self.gcj("zones", "set", "--from", "2019-06-01", "--cp", "300", "--w", "25000")
        self.assertEqual(env["data"]["status"], "added")
        self.assertGreaterEqual(env["data"]["refreshed"], 1)
        after = self.gcj("activity", "list", self.activity, "--metric", "coggan_tss")["data"]["activities"][0]["metrics"]["coggan_tss"]
        self.assertLess(after, before)   # higher CP, lower stress
        env = self.gcj("zones", "set", "--from", "2019-06-01", "--cp", "250")
        self.assertEqual(env["data"]["status"], "updated")
        self.gcj("zones", "set", "--from", "2019-06-01", expect=2)   # power needs --cp
        env = self.gcj("zones", "set", "--type", "hr", "--from", "2019-06-01", "--lthr", "172")
        self.assertEqual(env["data"]["hr"]["ranges"][-1]["lthr"], 172)

        env = self.gcj("measures", "add", "--when", "2020-01-20", "--set", "WEIGHTKG=72.5")
        self.assertGreaterEqual(env["data"]["refreshed"], 1)
        rows = self.gcj("measures", "list", "--group", "Body")["data"]["measures"]
        self.assertEqual(rows[-1]["WEIGHTKG"], 72.5)
        self.gcj("measures", "add", "--when", "2020-01-20", "--set", "NOPE=1", expect=2)
        self.gcj("measures", "list", "--group", "nothing", expect=3)
        self.assertClosed()

    def test_refresh_rebuild(self):
        env = self.gcj("athlete", "refresh", "--rebuild")
        self.assertEqual(env["data"]["refreshed"], env["data"]["activities"])
        self.assertClosed()


class TestActivitiesMetricsCharts(Headless):

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        r = cls.gc_class("--athlete", cls.athlete, "import", RIDE_POWER, RIDE_GPS, RUN_STRYD)
        assert r.code == 0, r

    def test_selection(self):
        acts = self.gcj("activity", "list")["data"]["activities"]
        self.assertEqual(len(acts), 3)
        self.assertEqual(len(self.gcj("activity", "list", "--sport", "Run")["data"]["activities"]), 1)
        self.assertEqual(len(self.gcj("activity", "list", "--from", "2020-01-01")["data"]["activities"]), 2)
        self.assertEqual(len(self.gcj("activity", "list", "--limit", "1")["data"]["activities"]), 1)
        self.assertEqual(self.gcj("activity", "list", "first")["data"]["activities"][0]["id"], acts[0]["id"])
        self.assertEqual(self.gcj("activity", "list", "2024-07-09")["data"]["activities"][0]["sport"], "Run")
        self.gcj("activity", "list", "1999-01-01", expect=3)
        self.gcj("activity", "list", "--filter", "Average_Power >", expect=2)
        filtered = self.gcj("activity", "list", "--filter", "Average_Power > 100")["data"]["activities"]
        self.assertEqual(len(filtered), 2)

    def test_eval(self):
        env = self.gcj("activity", "eval", "--expression", "Duration / 60", "--sport", "Bike")
        self.assertEqual(len(env["data"]["activities"]), 2)
        self.assertTrue(all(a["value"] > 10 for a in env["data"]["activities"]))

    def test_set_validates_fields(self):
        self.gcj("activity", "set", "last", "--set", "Notes=hello")
        self.assertEqual(self.gcj("activity", "list", "last", "--field", "Notes")["data"]["activities"][0]["metadata"]["Notes"], "hello")
        self.gcj("activity", "set", "last", "--set", "Undefined Thing=1", expect=2)
        self.gcj("activity", "set", "--set", "Notes=x", expect=2)  # needs a selection

    def test_export(self):
        out = os.path.join(self.tmp, "export.tcx")
        env = self.gcj("--output", out, "activity", "export", "last", "--as", "tcx")
        self.assertEqual(env["data"]["output"], out)
        self.assertIn(b"TrainingCenterDatabase", open(out, "rb").read(2000))
        r = self.gc("--athlete", self.athlete, "-o", "-", "activity", "export", "last", "--as", "csv")
        self.assertEqual(r.code, 0)
        self.assertTrue(r.out.startswith(b"Minutes"))
        self.gcj("activity", "export", "last", "--as", "doc", expect=2)

        # a table is not a chart: -o writes the csv, and a bad path is an error
        table = os.path.join(self.tmp, "intervals.csv")
        r = self.gc("--athlete", self.athlete, "--format", "csv", "-o", table, "interval", "list", "last")
        self.assertEqual(r.code, 0, r)
        self.assertEqual(r.out, b"")
        self.assertIn(b"wrote", r.err)
        saved = open(table, "rb").read()
        self.assertTrue(saved.startswith(b'"recorded laps:'), saved[:80])
        self.assertIn(b"number,name,type", saved)
        missing = os.path.join(self.tmp, "no-such-dir", "intervals.csv")
        r = self.gc("--athlete", self.athlete, "--format", "csv", "-o", missing, "interval", "list", "last")
        self.assertNotEqual(r.code, 0)
        self.assertEqual(r.out, b"")
        self.assertIn(b"can't write", r.err)
        self.assertFalse(os.path.exists(missing))

    def test_metrics(self):
        agg = self.gcj("metric", "aggregate", "--metric", "workout_time,total_distance")["data"]
        self.assertEqual(agg["activities"], 3)
        self.assertGreater(agg["values"]["workout_time"], 3600)
        self.gcj("metric", "aggregate", "--metric", "nonsense", expect=2)

        pmc = self.gcj("pmc", "--from", "2020-01-25", "--to", "2020-01-28")["data"]
        self.assertEqual(len(pmc["days"]), 4)
        self.assertGreater(pmc["days"][1]["stress"], 0)
        self.assertGreater(pmc["days"][2]["ctl"], 0)

        mm = self.gcj("meanmax", "2020_01_26_13_00_38")["data"]
        points = {p["secs"]: p["value"] for p in mm["points"]}
        self.assertGreater(points[1], points[1200])

        cp = self.gcj("cp", "--model", "cp2")["data"]
        self.assertEqual(cp["activities"], 2)   # bikes only by default
        self.assertGreater(cp["cp"], 100)

        est = self.gcj("cp", "estimates")["data"]
        self.assertIsInstance(est["estimates"], list)

        zones = self.gcj("zones", "show")["data"]
        self.assertEqual(zones["power"]["ranges"][0]["cp"], 250)

    def test_intervals(self):
        run = "2024_07_09_15_12_48"
        env = self.gcj("interval", "list", run)["data"]
        intervals = env["intervals"]
        self.assertEqual(intervals[0]["type"], "all")
        self.assertEqual(intervals[0]["group"], "ALL")
        laps = [i for i in intervals if i["type"] == "user"]
        self.assertGreater(len(laps), 5)
        # counts cover empty groups too, and stay put when --type filters the rows
        discovered = {"effort", "peakpower", "peakpace", "climb", "route"}
        self.assertEqual(env["recorded_laps"], sum(i["type"] == "device" for i in intervals))
        self.assertEqual(env["user_intervals"], len(laps))
        self.assertEqual(env["discovered_efforts"], sum(i["type"] in discovered for i in intervals))
        filtered = self.gcj("interval", "list", run, "--type", "user")["data"]
        self.assertEqual(filtered["recorded_laps"], env["recorded_laps"])
        self.assertEqual(filtered["discovered_efforts"], env["discovered_efforts"])
        self.assertEqual(len(filtered["intervals"]), env["user_intervals"])
        bike = self.gcj("interval", "list", "2020_01_26_13_00_38")["data"]
        self.assertEqual([bike["recorded_laps"], bike["user_intervals"], bike["discovered_efforts"]], [0, 0, 0])
        text = self.gc("--athlete", self.athlete, "interval", "list", "2020_01_26_13_00_38")
        self.assertEqual(text.code, 0, text)
        self.assertTrue(text.out.startswith(b"recorded laps: 0, user intervals: 0, discovered efforts: 0\n"), text)
        # the intervals sidebar's metrics by default, relevant ones only
        self.assertIn("pace", laps[0]["metrics"])
        self.assertNotIn("pace_swim", laps[0]["metrics"])
        self.assertAlmostEqual(laps[0]["duration"], laps[0]["stop"] - laps[0]["start"])

        only = self.gcj("interval", "list", run, "--type", "user", "--metric", "average_power,1m_peak_hr")["data"]["intervals"]
        self.assertEqual([i["name"] for i in only], [i["name"] for i in laps])
        self.assertEqual(sorted(only[0]["metrics"]), ["1m_peak_hr", "average_power"])
        self.gcj("interval", "list", run, "--type", "bogus", expect=2)
        self.gcj("interval", "list", run, "--metric", "nonsense", expect=2)
        # the sidebar's group titles and the formula names charts use work too
        titled = self.gcj("interval", "list", run, "--type", "USER", "--metric", "Average_Power,W'_Work,Duration")["data"]["intervals"]
        self.assertEqual(len(titled), len(laps))
        self.assertEqual(sorted(titled[0]["metrics"]), ["average_power", "skiba_wprime_exp", "workout_time"])
        shown = self.gcj("interval", "list", run, "--type", "user", "--metric", "workout_time,average_power", "--display")["data"]["intervals"]
        self.assertRegex(shown[0]["metrics"]["workout_time"], r"^\d\d:\d\d$")
        self.assertRegex(shown[0]["metrics"]["average_power"], r"^\d+$")

        # by number or by name, with every metric
        lap = self.gcj("interval", "show", run, str(laps[1]["number"]))["data"]
        self.assertEqual(lap["name"], laps[1]["name"])
        self.assertGreater(len(lap["metrics"]), 50)
        self.assertIn(0, lap["metrics"].values())   # zeros are kept, as the GUI shows them
        self.assertNotIn("pace_swim", lap["metrics"])
        self.assertEqual(lap["metrics"]["average_power"], only[1]["metrics"]["average_power"])
        self.assertEqual(self.gcj("interval", "show", run, laps[1]["name"].upper())["data"]["number"], laps[1]["number"])
        self.gcj("interval", "show", run, "999", expect=3)
        self.gcj("interval", "show", run, "no such lap", expect=3)

    def csv_rows(self, *args, expect=0):
        r = self.gc("--athlete", self.athlete, "--format", "csv", *args)
        self.assertEqual(r.code, expect, r)
        return list(csv.reader(io.StringIO(r.out.decode())))

    def test_csv(self):
        run = "2024_07_09_15_12_48"
        # a list is a table, with its nested metrics as columns
        rows = self.csv_rows("interval", "list", run, "--metric", "average_power,Duration")
        self.assertIn("recorded laps:", rows[0][0])
        head, body = rows[1], rows[2:]
        self.assertEqual(head[:4], ["number", "name", "type", "start"])
        intervals = self.gcj("interval", "list", run, "--metric", "average_power")["data"]["intervals"]
        self.assertEqual(len(body), len(intervals))
        self.assertEqual(float(body[1][head.index("average_power")]), intervals[1]["metrics"]["average_power"])
        shown = self.csv_rows("interval", "list", run, "--metric", "workout_time", "--display")
        self.assertRegex(shown[2][shown[1].index("workout_time")], r"^\d+:\d\d")

        # fields are quoted where they need it
        self.gcj("activity", "set", run, "--set", 'Notes=easy, then "fast"')
        rows = self.csv_rows("activity", "list", run, "--field", "Notes")
        self.assertEqual(rows[1][rows[0].index("Notes")], 'easy, then "fast"')

        # anything else is complete as key,value lines
        rows = self.csv_rows("activity", "show", run)
        self.assertEqual(rows[0], ["key", "value"])
        self.assertTrue(all(len(r) == 2 for r in rows))
        values = dict(rows[1:])
        self.assertEqual(values["sport"], "Run")
        self.assertIn("metrics.average_power", values)
        self.assertIn("zones.hr.zones[0].name", values)
        self.assertEqual(values["intervals[0].name"], "Entire Activity")

        # an overview table as the GUI shows it, several tiles one value a line
        rows = self.csv_rows("activity", "overview", run, "--tile", "Intervals Data")
        self.assertIn("recorded laps:", rows[0][0])
        self.assertEqual(rows[1][:2], ["Name", "Pace (min/km)"])
        self.assertEqual(len(rows) - 2, len(intervals))
        rows = self.csv_rows("activity", "overview", run)
        self.assertEqual(rows[0], ["tile", "kind", "row", "column", "units", "value"])
        self.assertTrue(any(r[1] == "summary" and "recorded laps:" in r[5] for r in rows))
        self.assertIn(("Sport", "field", "", "value", "", "Run"), [tuple(r) for r in rows])

        # errors go to stderr only
        r = self.gc("--athlete", self.athlete, "--format", "csv", "activity", "show", "1999-01-01")
        self.assertEqual(r.code, 3)
        self.assertEqual(r.out, b"")
        self.assertIn(b"error:", r.err)

    def test_overview_as_the_gui_shows_it(self):
        # no saved layouts: the ones GoldenCheetah starts with
        ride = self.gcj("activity", "overview", "2020_01_26_13_00_38")["data"]
        self.assertEqual(ride["layout"], "General")
        self.assertEqual([ride["recorded_laps"], ride["user_intervals"], ride["discovered_efforts"]], [0, 0, 0])
        tiles = {(t["name"], t["kind"]): t for t in ride["tiles"]}
        self.assertEqual(tiles[("Sport", "field")]["value"], "Bike")
        # one interval: the GUI shows a list of name, value and units
        self.assertEqual(tiles[("Intervals", "table")]["style"], "list")
        tiles = {t["name"]: t for t in ride["tiles"] if t["kind"] == "table"}
        totals = {r[0]: r for r in tiles["Totals"]["rows"]}
        self.assertEqual(totals["Duration"][1], "29:58")
        self.assertEqual(tiles["Power Zones"]["kind"], "table")

        run = self.gcj("activity", "overview", "last")["data"]
        self.assertEqual(run["layout"], "Run")
        table = [t for t in run["tiles"] if t["name"] == "Intervals Data"][0]
        self.assertEqual(table["style"], "grid")
        names = [c["name"] for c in table["columns"]]
        self.assertEqual(names[:2], ["Name", "Pace"])
        self.assertEqual(table["columns"][1]["units"], "min/km")
        intervals = self.gcj("interval", "list", "last")["data"]["intervals"]
        self.assertEqual(len(table["rows"]), len(intervals))
        self.assertRegex(table["rows"][0][names.index("Duration")], r"^\d+:\d\d")   # durations as times
        pmc = [t for t in run["tiles"] if t["kind"] == "pmc"][0]
        self.assertEqual(pmc["metric"], "govss")
        self.assertIn("fitness", pmc)

        only = self.gcj("activity", "overview", "last", "--tile", "intervals data")["data"]["tiles"]
        self.assertEqual([t["name"] for t in only], ["Intervals Data"])
        self.gcj("activity", "overview", "last", "--tile", "nothing", expect=3)
        self.assertEqual(self.gcj("activity", "overview", "last", "--layout", "general")["data"]["layout"], "General")
        self.gcj("activity", "overview", "last", "--layout", "nothing", expect=3)
        text = self.gc("--athlete", self.athlete, "activity", "overview", "last", "--tile", "Intervals Data")
        self.assertEqual(text.code, 0, text)
        self.assertIn(b"min/km", text.out)
        self.assertIn(b"recorded laps:", text.out)
        bike = self.gc("--athlete", self.athlete, "activity", "overview", "2020_01_26_13_00_38", "--tile", "Intervals")
        self.assertEqual(bike.code, 0, bike)
        self.assertIn(b"recorded laps: 0, user intervals: 0, discovered efforts: 0\n", bike.out)

    def test_activity_zones_and_pmc(self):
        run = self.gcj("activity", "show", "2024_07_09_15_12_48")["data"]
        self.assertEqual(sorted(run["zones"]), ["fatigue", "hr", "pace", "power"])
        hr = run["zones"]["hr"]
        self.assertEqual(hr["lthr"], 165)
        # percentages of the recording time, as the GUI
        self.assertAlmostEqual(sum(z["seconds"] for z in hr["zones"]), hr["total_seconds"], delta=5)
        self.assertAlmostEqual(sum(z["percent"] for z in hr["zones"]), 100, delta=1)
        pace = run["zones"]["pace"]
        self.assertEqual(pace["units"], "min/km")
        self.assertRegex(pace["zones"][1]["low"], r"^\d\d:\d\d$")
        self.assertNotIn("high", pace["zones"][-1])  # open ended
        self.assertEqual(run["pmc"]["metric"], "govss")
        self.assertGreater(run["pmc"]["stress"], 0)
        self.assertEqual(run["intervals"][0]["number"], 1)
        self.assertIn("duration", run["intervals"][0])
        self.assertIn(0, run["metrics"].values())

        ride = self.gcj("activity", "show", "2020_01_26_13_00_38", "--pmc-metric", "trimp_points")["data"]
        self.assertNotIn("pace", ride["zones"])
        self.assertEqual(ride["zones"]["power"]["cp"], 250)
        self.assertEqual(ride["zones"]["fatigue"]["wprime"], 20000)
        self.assertEqual(ride["pmc"]["metric"], "trimp_points")
        self.gcj("activity", "show", "last", "--pmc-metric", "nonsense", expect=2)

        zones = self.gcj("zones", "show", "--sport", "Run")["data"]
        run_pace = [p for p in zones["pace"] if p["sport"] == "Run"][0]
        self.assertEqual(run_pace["cv_pace"], "05:00")
        self.assertGreater(len(run_pace["zones"]), 3)

    def test_charts(self):
        magic = {"png": b"\x89PNG", "svg": b"<?xml", "pdf": b"%PDF"}
        charts = [
            ("activity", "2020_01_26_13_00_38"),
            ("activity", "2020_01_26_13_00_38", "--series", "watts,hr", "--smooth", "5"),
            ("meanmax",),
            ("pmc", "--from", "2019-12-01", "--to", "2024-08-01"),
            ("zones", "2020_01_26_13_00_38"),
            ("zones", "2020_01_26_13_00_38", "--type", "hr"),
            ("zones", "2024_07_09_15_12_48", "--type", "pace"),
            ("zones", "2020_01_26_13_00_38", "--type", "fatigue"),
            ("trend", "workout_time", "--by", "month"),
        ]
        for i, chart in enumerate(charts):
            for fmt in ("png", "svg", "pdf"):
                out = os.path.join(self.tmp, "chart-%d.%s" % (i, fmt))
                r = self.gc("--athlete", self.athlete, "-o", out, "chart", *chart, "--as", fmt, "--width", "800", "--height", "400")
                self.assertEqual(r.code, 0, (chart, r))
                data = open(out, "rb").read()
                self.assertTrue(data.startswith(magic[fmt]), (chart, fmt, data[:20]))
        env = self.gcj("-o", os.path.join(self.tmp, "t.png"), "chart", "trend", "workout_time", "--by", "activity")
        self.assertEqual(env["data"]["periods"], env["data"]["activities"])
        r = self.gc("--athlete", self.athlete, "chart", "activity", "last", "--series", "nothing")
        self.assertEqual(r.code, 5, r)

    def test_zz_delete(self):  # last: changes the activity count
        env = self.gcj("import", MULTI_TCX)
        victim = env["data"]["files"][0]["activity"]
        self.gcj("activity", "delete", victim)
        self.assertNotIn(victim + ".json", self.activity_files())
        self.gcj("activity", "show", victim, expect=3)
        # kept as a backup
        self.assertTrue(any(victim in f for f in os.listdir(os.path.join(self.folder, "bak"))))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TestRest(Headless):

    token = uuid.uuid4().hex

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.port = free_port()
        cmd = [BINARY, "--cli", "--home", cls.home, "serve", "--port", str(cls.port), "--token", cls.token]
        cls.serverlog = open(os.path.join(cls.tmp, "server.log"), "wb")
        cls.server = subprocess.Popen(cmd, env=cls.env, stdout=subprocess.DEVNULL, stderr=cls.serverlog)
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", cls.port), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.2)
        else:
            cls.server.kill()
            raise RuntimeError("REST server did not start")

    @classmethod
    def tearDownClass(cls):
        if os.name == "nt":
            cls.server.terminate()      # no SIGINT for child processes on Windows
        else:
            cls.server.send_signal(signal.SIGINT)
        try:
            cls.server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            cls.server.kill()
        cls.serverlog.close()
        super().tearDownClass()

    def call(self, method, path, body=None, headers=None, raw=None, auth=True):
        url = "http://127.0.0.1:%d/v1%s" % (self.port, path)
        data = raw
        hdrs = dict(headers or {})
        if body is not None:
            data = json.dumps(body).encode()
            hdrs["Content-Type"] = "application/json"
        if auth:
            hdrs["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                return resp.status, resp.headers.get("Content-Type"), resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers.get("Content-Type"), e.read()

    def jcall(self, method, path, expect=200, **kw):
        status, ctype, data = self.call(method, path, **kw)
        self.assertEqual(status, expect, data[:500])
        self.assertIn("application/json", ctype)
        return json.loads(data)

    def test_auth(self):
        status, _, _ = self.call("GET", "/athletes", auth=False)
        self.assertEqual(status, 401)

    def test_openapi(self):
        doc = self.jcall("GET", "/openapi.json")
        self.assertIn("/v1/athletes/{athlete}/processors/{name}/runs", doc["paths"])
        self.assertIn("post", doc["paths"]["/v1/athletes/{athlete}/imports"])

    def test_workflow_over_http(self):
        a = "/athletes/" + self.athlete

        # raw upload
        with open(RIDE_POWER, "rb") as f:
            status, _, data = self.call("POST", a + "/imports?filename=ride.fit", raw=f.read(),
                                        headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(status, 200, data)
        self.assertEqual(json.loads(data)["data"]["imported"], 1)

        # multipart upload
        boundary = "gcboundary"
        with open(RUN_STRYD, "rb") as f:
            payload = (b"--" + boundary.encode() + b"\r\n"
                       b'Content-Disposition: form-data; name="file1"; filename="run.fit"\r\n'
                       b"Content-Type: application/octet-stream\r\n\r\n" + f.read() + b"\r\n"
                       b"--" + boundary.encode() + b"--\r\n")
        status, _, data = self.call("POST", a + "/imports", raw=payload,
                                    headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
        self.assertEqual(status, 200, data)
        self.assertEqual(json.loads(data)["data"]["files"][0]["sport"], "Run")
        self.assertEqual(json.loads(data)["data"]["files"][0]["source"], "run.fit")

        # already imported: skipped
        with open(RIDE_POWER, "rb") as f:
            _, _, data = self.call("POST", a + "/imports?filename=again.fit", raw=f.read(),
                                   headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(json.loads(data)["data"]["skipped"], 1)

        # fields and listing with a GUI filter
        env = self.jcall("POST", "/fields", body={"name": EP_FIELDS, "type": "double"})
        self.assertEqual(env["data"]["added"], len(EP_FIELDS))
        env = self.jcall("GET", a + "/activities?filter=isRun%3D0&metric=average_power")
        rides = {x["id"]: x for x in env["data"]["activities"]}
        self.assertTrue(all(x["sport"] != "Run" for x in rides.values()))
        self.assertGreater(rides["2020_01_26_13_00_38"]["metrics"]["average_power"], 50)

        # activity, patch fields, chart as an image and as an envelope
        env = self.jcall("GET", a + "/activities/last")
        self.assertEqual(env["data"]["sport"], "Run")
        env = self.jcall("PATCH", a + "/activities", body={"activity": ["last"], "set": ["EP CdA=0.3"]})
        self.assertEqual(env["data"]["updated"], 1)
        status, ctype, data = self.call("GET", a + "/activities/2020_01_26_13_00_38/chart?width=640&height=320")
        self.assertEqual(status, 200)
        self.assertEqual(ctype, "image/png")
        self.assertTrue(data.startswith(b"\x89PNG"))
        env = self.jcall("GET", a + "/activities/last/intervals?type=user&metric=average_power")
        lap = env["data"]["intervals"][0]
        env = self.jcall("GET", a + "/activities/last/intervals/" + urllib.parse.quote(lap["name"]))
        self.assertEqual(env["data"]["metrics"]["average_power"], lap["metrics"]["average_power"])
        self.jcall("GET", a + "/activities/last/intervals/999", expect=404)
        status, ctype, data = self.call("GET", a + "/activities/last/intervals?type=user&format=csv")
        self.assertEqual(status, 200)
        self.assertIn("text/csv", ctype)
        lines = data.splitlines()
        self.assertTrue(lines[0].startswith(b'"recorded laps:'), lines[0])
        self.assertTrue(lines[1].startswith(b"number,name,type"))
        self.jcall("GET", a + "/activities/last/intervals?format=xml", expect=400)
        self.jcall("GET", a + "/activities/nothing/intervals?format=csv", expect=404)   # errors stay JSON
        env = self.jcall("GET", a + "/activities/last/overview?tile=Intervals%20Data")
        self.assertIn("table", [t["kind"] for t in env["data"]["tiles"]])
        env = self.jcall("GET", a + "/charts/pmc?envelope=1")
        self.assertGreater(env["data"]["payload_bytes"], 1000)

        # generic command route
        env = self.jcall("POST", "/commands/metric.aggregate",
                         body={"athlete": self.athlete, "args": {"metric": ["workout_time"], "sport": "Run"}})
        self.assertEqual(env["data"]["activities"], 1)

        # processor run over http
        env = self.jcall("POST", a + "/processors/fixspikes/runs", body={"all": True})
        self.assertEqual(env["data"]["failed"], 0)

    def test_errors(self):
        env = self.jcall("GET", "/athletes/Nobody/activities", expect=404)
        self.assertEqual(env["status"], "not_found")
        self.jcall("GET", "/athletes/%s/activities?colour=red" % self.athlete, expect=400)
        self.jcall("GET", "/nothing/here", expect=404)
        status, _, _ = self.call("PUT", "/athletes")
        self.assertEqual(status, 405)
        self.jcall("POST", "/athletes/%s/imports" % self.athlete, raw=b"xx",
                   headers={"Content-Type": "application/octet-stream"}, expect=400)

    def test_cross_site_requests_are_refused(self):
        status, _, _ = self.call("GET", "/athletes", headers={"Origin": "http://evil.example"})
        self.assertEqual(status, 403)
        status, _, _ = self.call("GET", "/athletes", headers={"Origin": "http://127.0.0.1:%d" % self.port})
        self.assertEqual(status, 200)
        status, _, _ = self.call("GET", "/athletes", headers={"Host": "rebound.example:%d" % self.port})
        self.assertEqual(status, 403)
        # a JSON body must say so, a text/plain or form post is not JSON
        status, _, data = self.call("POST", "/commands/processor.install", raw=b'{"args":{"name":"x","source":"1"}}',
                                    headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(status, 400)
        self.assertIn(b"application/json", data)
        status, _, _ = self.call("POST", "/commands/processor.install", raw=b'{"args":{"name":"x","source":"1"}}',
                                 headers={"Content-Type": "text/plain"})
        self.assertNotEqual(status, 200)

    def test_raw_upload_of_a_json_activity(self):
        with open(os.path.join(TESTDATA, "rides", "2013_05_27_08_56_35.json"), "rb") as f:
            status, _, data = self.call("POST", "/athletes/%s/imports?filename=ride.json" % self.athlete, raw=f.read(),
                                        headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(status, 200, data)
        self.assertEqual(json.loads(data)["data"]["files"][0]["status"], "imported")

    def test_network_listening_needs_a_token(self):
        r = subprocess.run([BINARY, "--cli", "--home", self.home, "serve", "--host", "0.0.0.0", "--port", str(free_port())],
                           capture_output=True, timeout=60, env=self.env)
        self.assertEqual(r.returncode, 2)
        self.assertIn(b"needs a token", r.stderr)

    def test_athlete_is_closed_between_requests(self):
        self.jcall("GET", "/athletes/%s" % self.athlete)
        self.assertClosed()


ONES = "{\n    value { 1; }\n}\n"
TWOS = "{\n    value { 2; }\n}\n"


class TestUserMetricsAndZones(Headless):

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        r = cls.gc_class("--athlete", cls.athlete, "import", RIDE_POWER)
        assert r.code == 0, r

    def metrics_file(self):
        return os.path.join(self.home, "usermetrics.xml")

    def test_user_metric_is_checked_then_computed(self):
        path = self.metrics_file()
        self.assertFalse(os.path.exists(path))

        self.gcj("metric", "user", "add", "--symbol", "ones", "--name", "Ones",
                 "--program", "{ value { NotARealSymbol; } }", expect=2)
        self.gcj("metric", "user", "add", "--symbol", "ones", "--name", "Ones",
                 "--program", "this is not a formula", expect=2)
        self.gcj("metric", "user", "add", "--symbol", "ones", "--name", "Ones",
                 "--program", "{ relevant { 1; } }", expect=2)
        self.assertFalse(os.path.exists(path))

        env = self.gcj("metric", "user", "add", "--symbol", "ones", "--name", "Ones",
                       "--type", "average", "--units", "x", "--precision", "0", "--program", ONES)
        self.assertEqual(env["data"]["status"], "added")
        self.assertGreaterEqual(env["data"]["refreshed"], 1)
        self.assertEqual(self.gcj("metric", "user", "show", "ones")["data"]["program"], ONES)
        symbols = {m["symbol"] for m in self.gcj("metric", "list", "--search", "ones")["data"]["metrics"]}
        self.assertIn("ones", symbols)
        self.assertEqual(self.gcj("activity", "show", "last")["data"]["metrics"]["ones"], 1)

        with open(path, "rb") as f:
            before = f.read()
        self.gcj("metric", "user", "edit", "ones", "--program", "{ value { 1 + ; } }", expect=2)
        self.gcj("metric", "user", "edit", "ones", "--program", "{ relevant { 1; } }", expect=2)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), before)

        env = self.gcj("metric", "user", "edit", "ones", "--program", TWOS, "--precision", "1")
        self.assertEqual(env["data"]["precision"], 1)
        self.assertEqual(self.gcj("activity", "show", "last")["data"]["metrics"]["ones"], 2)

        self.gcj("activity", "column", "add", "ones")
        columns = self.gcj("activity", "column", "list")["data"]["columns"]
        self.assertIn("Ones", columns)
        self.assertIn("Date", columns)

        self.gcj("metric", "user", "remove", "ones")
        self.gcj("metric", "user", "show", "ones", expect=3)
        self.assertNotIn("ones", self.gcj("activity", "show", "last")["data"]["metrics"])

    def test_favourite_order_is_the_intervals_table_order(self):
        self.gcj("metric", "favourite", "set", "workout_time", "average_hr")
        listed = self.gcj("metric", "favourite", "list")["data"]["metrics"]
        self.assertEqual(listed[:2], ["workout_time", "average_hr"])

        r = self.gc("--athlete", self.athlete, "interval", "list", "last")
        self.assertEqual(r.code, 0, r)
        header = next(line for line in r.out.decode().splitlines() if "workout_time" in line and "average_hr" in line)
        self.assertLess(header.index("workout_time"), header.index("average_hr"))

        self.gcj("metric", "favourite", "add", "average_speed")
        listed = self.gcj("metric", "favourite", "list")["data"]["metrics"]
        self.assertEqual(listed.index("average_speed"), len(listed) - 1)
        self.gcj("metric", "favourite", "add", "average_speed")
        self.assertEqual(self.gcj("metric", "favourite", "list")["data"]["metrics"], listed)

        self.gcj("metric", "favourite", "remove", "average_speed")
        self.assertNotIn("average_speed", self.gcj("metric", "favourite", "list")["data"]["metrics"])
        self.gcj("metric", "favourite", "add", "not_a_metric", expect=2)

    def test_hr_range_keeps_other_anchors_and_pace_can_be_set(self):
        original = self.gcj("zones", "show")["data"]["hr"]["ranges"][0]
        env = self.gcj("zones", "set", "--type", "hr", "--from", "2026-01-01", "--resthr", "40")
        self.assertEqual(env["data"]["status"], "added")
        added = [r for r in env["data"]["hr"]["ranges"] if r["from"] == "2026-01-01"][0]
        self.assertEqual(added["resthr"], 40)
        self.assertEqual(added["lthr"], original["lthr"])
        self.assertEqual(added["maxhr"], original["maxhr"])

        env = self.gcj("zones", "set", "--type", "hr", "--from", original["from"], "--resthr", "42")
        self.assertEqual(env["data"]["status"], "updated")
        updated = [r for r in env["data"]["hr"]["ranges"] if r["from"] == original["from"]][0]
        self.assertEqual(updated["resthr"], 42)
        self.assertEqual(updated["lthr"], original["lthr"])

        env = self.gcj("zones", "set", "--type", "pace", "--sport", "Run", "--from", "2026-02-01", "--cv", "13.5")
        self.assertEqual(env["data"]["status"], "added")
        self.assertEqual(env["data"]["cv"], 13.5)
        shown = self.gcj("zones", "show", "--sport", "Run")["data"]["pace"]
        match = [r for r in shown if r["sport"] == "Run" and r["from"] == "2026-02-01"]
        self.assertEqual(match[0]["cv"], 13.5)
        self.assertClosed()


if __name__ == "__main__":
    unittest.main()
