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
# Without the binary the tests are skipped, and those needing embedded
# Python skip without it. CI sets GC_REQUIRE_BINARY=1 and
# GC_REQUIRE_PYTHON=1 to make those failures instead, so a green run
# means the tests ran.
#

import base64
import csv
import datetime
import html
import io
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile

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


# absolute: some tests run it from another working folder
BINARY = os.path.abspath(os.environ.get("GC_BINARY", default_binary()))
REQUIRE_BINARY = os.environ.get("GC_REQUIRE_BINARY") == "1"
REQUIRE_PYTHON = os.environ.get("GC_REQUIRE_PYTHON") == "1"

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
    imports = []        # activity files every test of the class starts with

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(BINARY):
            if REQUIRE_BINARY:
                raise AssertionError("GoldenCheetah binary not found at %s (GC_REQUIRE_BINARY is set)" % BINARY)
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
        # athlete locks are kept here, not in the athlete folder
        cls.env["XDG_RUNTIME_DIR"] = os.path.join(cls.tmp, "runtime")
        os.makedirs(cls.env["XDG_RUNTIME_DIR"], mode=0o700)
        r = cls.gc_class("athlete", "create", cls.athlete, "--cp", "250", "--weight", "70")
        assert r.code == 0, r
        if cls.imports:
            r = cls.gc_class("--athlete", cls.athlete, "import", *cls.imports)
            assert r.code == 0, r

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def gc_class(cls, *args, timeout=180, input=None, extra=None, home="", cwd=None, preexec_fn=None):
        """run the command line on the class's athletes folder, another
        one with home, or with home=None on none (the default is found)"""
        home = cls.home if home == "" else home
        cmd = [BINARY, "--cli"] + (["--home", home] if home is not None else []) + (extra or []) + [str(a) for a in args]
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout, env=cls.env, input=input,
                              cwd=cwd, preexec_fn=preexec_fn)
        return Result(proc)

    def gc(self, *args, **kw):
        return self.gc_class(*args, **kw)

    def gca(self, *args, **kw):
        """run on the class's athlete"""
        return self.gc_class("--athlete", self.athlete, *args, **kw)

    def csv_rows(self, *args, expect=0):
        r = self.gca("--format", "csv", *args)
        self.assertEqual(r.code, expect, r)
        return list(csv.reader(io.StringIO(r.out.decode())))

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

    def keep(self, path):
        """put a file or folder back as it is now when the test ends, pass or fail"""
        backup = os.path.join(tempfile.mkdtemp(dir=self.tmp), "kept")
        if os.path.isdir(path):
            shutil.copytree(path, backup)
        elif os.path.exists(path):
            shutil.copy2(path, backup)
        else:
            backup = None

        def restore():
            if os.path.isdir(path):
                shutil.rmtree(path)
            elif os.path.exists(path):
                os.remove(path)
            if backup and os.path.isdir(backup):
                shutil.copytree(backup, path)
            elif backup:
                shutil.copy2(backup, path)
        self.addCleanup(restore)

    def requirePython(self, available):
        """skip without embedded Python, or fail when CI requires it"""
        if available:
            return
        if REQUIRE_PYTHON:
            self.fail("embedded Python not available (GC_REQUIRE_PYTHON is set)")
        self.skipTest("embedded Python not available")

    @property
    def lock_dir(self):
        return os.path.join(self.env["XDG_RUNTIME_DIR"], "GoldenCheetah", "locks")

    def assertClosed(self):
        """a command must never leave the athlete open"""
        locks = os.listdir(self.lock_dir) if os.path.isdir(self.lock_dir) else []
        self.assertEqual([f for f in locks if f.endswith(".lock")], [], "athlete lock left behind")
        self.assertFalse(os.path.exists(os.path.join(self.folder, "athlete.lock")), "lock in the athlete folder")
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
        r = self.gca("activity", "list", "--from", "yesterday")
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"yyyy-mm-dd", r.err)

    def test_field_add_changes_summary_interval_and_values(self):
        def shown(name):
            return [f for f in self.gcj("field", "list")["data"]["fields"] if f["name"] == name][0]
        self.gcj("field", "add", "Kit", "--type", "text")
        self.assertEqual(self.gcj("field", "add", "Kit", "--type", "text")["data"]["unchanged"], 1)
        env = self.gcj("field", "add", "Kit", "--type", "text", "--summary", expect=5)
        self.assertIn("use --update", env["data"]["fields"][0]["message"])
        self.assertFalse(shown("Kit").get("summary", False))
        env = self.gcj("field", "add", "Kit", "--type", "text", "--summary", "--value", "Road", "--value", "TT", "--update")
        self.assertEqual(env["data"]["updated"], 1)
        self.assertTrue(shown("Kit")["summary"])
        # not given: left as it is
        self.assertEqual(self.gcj("field", "add", "Kit", "--type", "text")["data"]["unchanged"], 1)
        self.assertTrue(shown("Kit")["summary"])
        self.gcj("field", "remove", "Kit")

    def test_library_in_the_working_folder_like_the_gui(self):
        # a library on a USB stick: ./Library/GoldenCheetah, found as main() finds it
        stick = os.path.join(self.tmp, "stick")
        library = os.path.join(stick, "Library", "GoldenCheetah")
        os.makedirs(library)
        r = self.gc("athlete", "create", "Usb", home=library, timeout=60)
        self.assertEqual(r.code, 0, r)
        r = self.gc("--format", "json", "athlete", "list", home=None, cwd=stick, timeout=60)
        self.assertEqual(r.code, 0, r)
        data = r.json()["data"]
        self.assertEqual(os.path.realpath(data["home"]), os.path.realpath(library))
        self.assertEqual([a["name"] for a in data["athletes"]], ["Usb"])

    def test_athlete_dir_with_a_trailing_slash(self):
        r = self.gc("--athlete-dir", self.folder + os.sep, "--format", "json", "athlete", "show", home=None, timeout=60)
        self.assertEqual(r.code, 0, r)
        self.assertEqual(r.json()["data"]["name"], self.athlete)

    def test_version(self):
        env = self.gcj("version")
        self.assertTrue(env["ok"])
        self.assertGreater(env["data"]["metrics"], 100)
        self.assertGreater(env["data"]["import_formats"], 20)

    def test_commands_are_described(self):
        env = self.gcj("commands")
        names = {c["name"] for c in env["data"]["commands"]}
        for n in ("import", "field.add", "processor.install", "processor.run", "activity.list", "chart.pmc",
                  "chart.library.list", "chart.library.add", "chart.library.curve.add"):
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

    def test_only_the_first_cli_flag_is_taken(self):
        # after --, --cli is an argument: here an activity that isn't there
        r = self.gc("activity", "list", "--", "--cli")
        self.assertEqual(r.code, 3, r)
        self.assertIn(b"'--cli'", r.err)

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
        r = self.gc("field", "add", "Foo", home=missing, timeout=60)
        self.assertEqual(r.code, 3, r)
        r = self.gc("processor", "install", "x", "--source", "1", home=missing, timeout=60)
        self.assertEqual(r.code, 3, r)
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
        r = self.gca("import", broken, "/does/not/exist.fit")
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"file not found", r.out)

        # a mix of good and bad is a partial success
        r = self.gca("import", broken, RUN_STRYD)
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
        self.requirePython(self.python)

    def test_workflow(self):
        # each step builds on the ones before, so they run as one test
        for name, step in [
            ("1_once_per_athlete", self.step_1_once_per_athlete),
            ("2_each_new_ride", self.step_2_each_new_ride),
            ("3_rerun_the_estimate", self.step_3_rerun_the_estimate),
            ("4_errors_are_reported", self.step_4_errors_are_reported),
            ("4b_script_that_changes_nothing_is_skipped", self.step_4b_script_that_changes_nothing_is_skipped),
            ("5_dry_run_saves_nothing", self.step_5_dry_run_saves_nothing),
            ("6_builtin_processor", self.step_6_builtin_processor),
        ]:
            with self.subTest(step=name):
                step()

    def step_1_once_per_athlete(self):
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

    def step_2_each_new_ride(self):
        env = self.gcj("import", RIDE_POWER, RIDE_GPS, RUN_STRYD)
        self.assertEqual(env["data"]["imported"], 3)
        sports = sorted(f["sport"] for f in env["data"]["files"])
        self.assertEqual(sports, ["Bike", "Bike", "Run"])

    def step_3_rerun_the_estimate(self):
        run = [a["id"] for a in self.gcj("activity", "list", "--filter", "isRun=1")["data"]["activities"]][0]
        self.gcj("activity", "set", run, "--set", "EP Wind Speed=3.5")

        # choose like the GUI filter: outdoor rides only (not runs)
        r = self.gca("processor", "run", "estimate-power", "--filter", "isRun=0")
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

    def step_4_errors_are_reported(self):
        self.gcj("processor", "install", "broken", "--file", self.bad)
        r = self.gca("processor", "run", "broken", "last")
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"failed", r.out)
        self.assertIn(b"ValueError: boom", r.out)

        # a run on everything needs to be asked for
        r = self.gca("processor", "run", "estimate-power")
        self.assertEqual(r.code, 2, r)
        r = self.gca("processor", "run", "no-such-processor", "--all")
        self.assertEqual(r.code, 3, r)
        r = self.gca("processor", "run", "estimate-power", "--filter", "isRun=(")
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"bad filter", r.err)

    def step_4b_script_that_changes_nothing_is_skipped(self):
        self.gcj("processor", "install", "noop", "--source", 'print("looked")\n')
        env = self.gcj("processor", "run", "noop", "--all")
        self.assertEqual(env["data"]["processed"], 0)
        self.assertEqual(env["data"]["skipped"], 3)
        self.assertEqual(env["data"]["activities"][0]["output"], "looked")

    def step_5_dry_run_saves_nothing(self):
        path = os.path.join(self.folder, "activities", self.activity_files()[0])
        before = open(path, "rb").read()
        env = self.gcj("processor", "run", "estimate-power", "--all", "--dry-run")
        self.assertFalse(env["data"]["saved"])
        self.assertEqual(open(path, "rb").read(), before)

    def step_6_builtin_processor(self):
        env = self.gcj("processor", "run", "fixspikes", "--sport", "Bike")
        self.assertEqual(env["data"]["failed"], 0)
        self.assertEqual(len(env["data"]["activities"]), 2)



class TestProcessorFiles(Headless):

    def test_script_files_are_never_shared(self):
        self.requirePython(self.gcj("version")["data"]["python"])
        # "a 1" is stored as a_1.py, then "a" as a.py, and "A" also wants a.py:
        # one pass over [a_1.py, a.py] gave it a_1.py, over the first script
        sources = {"a 1": "print('a 1')\n", "a": "print('a')\n", "A": "print('A')\n"}
        for name, source in sources.items():
            self.gcj("processor", "install", name, "--source", source)
        files = set()
        for name, source in sources.items():
            shown = self.gcj("processor", "show", name)["data"]
            self.assertEqual(shown["source"], source)
            with open(shown["file"]) as f:
                self.assertEqual(f.read(), source)
            files.add(shown["file"])
        self.assertEqual(len(files), 3)

class TestLivesWithOtherTools(Headless):
    """fill-wind and fill-cp edit the folder while GoldenCheetah isn't running"""

    imports = [RIDE_POWER]

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.activity = "2020_01_26_13_00_38"

    def test_edited_activity_file_is_used(self):
        path = os.path.join(self.folder, "activities", self.activity + ".json")
        self.keep(path)
        saved = os.stat(path).st_mtime
        with open(path, encoding="utf-8-sig") as f:
            doc = json.load(f)
        doc["RIDE"]["TAGS"]["Notes"] = "edited by another tool"
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        # later than the cache's record, whatever the file system's mtime resolution
        os.utime(path, (saved + 2, saved + 2))

        env = self.gcj("activity", "list", self.activity, "--field", "Notes")
        self.assertEqual(env["data"]["activities"][0]["metadata"]["Notes"], "edited by another tool")

    def test_other_formats_are_converted_when_saved(self):
        # a file another tool dropped in activities/ is saved as JSON, as the
        # GUI does, and the original kept as a .bak
        activities = os.path.join(self.folder, "activities")
        self.keep(activities)
        shutil.copy(TCX_RIDE, activities)
        name = os.path.splitext(os.path.basename(TCX_RIDE))[0]
        env = self.gcj("activity", "set", name, "--set", "Notes=converted")
        self.assertEqual(env["data"]["updated"], 1)
        files = os.listdir(activities)
        self.assertIn(name + ".json", files)
        self.assertIn(name + ".tcx.bak", files)
        self.assertNotIn(name + ".tcx", files)
        item = self.gcj("activity", "list", name, "--field", "Notes")["data"]["activities"][0]
        self.assertEqual(item["file"], name + ".json")
        self.assertEqual(item["metadata"]["Notes"], "converted")

    def test_athlete_of_an_older_version_is_left_to_the_gui(self):
        # before 3.6 the GUI asks before upgrading; without anyone to ask,
        # the command fails at once rather than waiting for an answer
        config = os.path.join(self.folder, "config")
        self.keep(config)
        ini = os.path.join(config, "athlete-general.ini")
        text = open(ini).read()
        self.assertRegex(text, r"versionused=\d+")
        with open(ini, "w") as f:
            f.write(re.sub(r"versionused=\d+", "versionused=4000", text))
        r = self.gca("activity", "list", timeout=60)
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"could not be upgraded", r.err)

    def test_edited_zones_recompute_metrics(self):
        before = self.gcj("activity", "list", self.activity, "--metric", "coggan_tss")["data"]["activities"][0]
        zones = os.path.join(self.folder, "config", "power.zones")
        self.keep(zones)
        text = open(zones).read()
        self.assertIn("CP=250", text)
        with open(zones, "w") as f:
            f.write(text.replace("CP=250", "CP=200").replace("FTP=250", "FTP=200"))

        refreshed = self.gcj("athlete", "show")["data"]["refreshed"]
        self.assertGreaterEqual(refreshed, 1)
        after = self.gcj("activity", "list", self.activity, "--metric", "coggan_tss")["data"]["activities"][0]
        self.assertGreater(after["metrics"]["coggan_tss"], before["metrics"]["coggan_tss"])
        self.assertEqual(self.gcj("zones", "show")["data"]["power"]["ranges"][0]["cp"], 200)

    def test_refuses_while_another_process_holds_the_athlete(self):
        # a real holder: serve, busy with a slow request on the athlete
        self.requirePython(self.gcj("version")["data"]["python"])
        r = self.gc("processor", "install", "hold", "--source", "import time\ntime.sleep(4)\n")
        self.assertEqual(r.code, 0, r)
        self.addCleanup(self.gc, "processor", "remove", "hold")
        server, port = start_server(self.env, self.home, os.path.join(self.tmp, "hold.log"))
        try:
            answer = []
            slow = threading.Thread(target=lambda: answer.append(call_server(
                port, "POST", "/athletes/%s/processors/hold/runs" % self.athlete, {"activity": [self.activity]})))
            slow.start()
            # the only athlete, so the only lock in this test's lock folder
            def held():
                return os.path.isdir(self.lock_dir) and any(f.endswith(".lock") for f in os.listdir(self.lock_dir))
            deadline = time.time() + 20
            while time.time() < deadline and not held():
                time.sleep(0.05)
            self.assertTrue(held(), "serve never took the athlete")

            r = self.gca("activity", "list")
            self.assertEqual(r.code, 4, r)
            self.assertIn(b"in use", r.err)

            # another process waits its turn with --lock-wait
            r = self.gca("--lock-wait", "30", "activity", "list")
            self.assertEqual(r.code, 0, r)
            slow.join(timeout=30)
            self.assertEqual(answer[0][0], 200, answer)
        finally:
            stop_server(server)
        self.assertClosed()

    @unittest.skipIf(os.name == "nt", "fakes a dead process's lock with a POSIX 'true'")
    def test_lock_left_in_the_folder_by_older_builds(self):
        # the lock moved out of the athlete folder: one there (synced from
        # another machine, say) doesn't stop a command, and a stale one
        # from this machine is removed
        old = os.path.join(self.folder, "athlete.lock")
        try:
            with open(old, "w") as f:
                f.write("999999\nGoldenCheetah\nsome-other-machine\n")
            r = self.gca("activity", "list")
            self.assertEqual(r.code, 0, r)
            self.assertTrue(os.path.exists(old))

            gone = subprocess.Popen(["true"])
            gone.wait()
            with open(old, "w") as f:
                f.write("%d\nGoldenCheetah\n%s\n" % (gone.pid, self.gcj("version")["data"]["host"]))
            r = self.gca("activity", "list")
            self.assertEqual(r.code, 0, r)
            self.assertFalse(os.path.exists(old))
        finally:
            if os.path.exists(old):
                os.remove(old)

    def test_refuses_when_gui_did_not_close_cleanly(self):
        ini = os.path.join(self.folder, "config", "athlete-general.ini")
        text = open(ini).read()
        with open(ini, "w") as f:
            f.write(text.replace("safeexit=true", "safeexit=false"))
        try:
            r = self.gca("activity", "list")
            self.assertEqual(r.code, 4, r)
            self.assertIn(b"--force", r.err)
            r = self.gca("--force", "activity", "list")
            self.assertEqual(r.code, 0, r)
        finally:
            with open(ini, "w") as f:
                f.write(text)

    def test_zones_and_measures_commands(self):
        self.keep(os.path.join(self.folder, "config"))
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



class TestActivityFields(Headless):
    """activity set edits a field as the Details tab does"""

    imports = [RIDE_POWER, RUN_STRYD]

    def saved(self, activity):
        with open(os.path.join(self.folder, "activities", activity + ".json"), encoding="utf-8-sig") as f:
            return json.load(f)["RIDE"]

    def test_metric_fields_set_an_override(self):
        bike = "2020_01_26_13_00_38"
        computed = self.gcj("activity", "show", bike)["data"]["metrics"]["average_power"]
        self.gcj("activity", "set", bike, "--set", "Average Power=250")
        self.assertEqual(self.gcj("activity", "show", bike)["data"]["metrics"]["average_power"], 250)
        overrides = {k: v for o in self.saved(bike).get("OVERRIDES", []) for k, v in o.items()}
        self.assertEqual(overrides["average_power"]["value"], "250")
        self.assertNotIn("Average Power", self.saved(bike).get("TAGS", {}))

        self.gcj("activity", "set", bike, "--set", "Average Power=")
        self.assertEqual(self.gcj("activity", "show", bike)["data"]["metrics"]["average_power"], computed)
        self.assertNotIn("average_power", {k for o in self.saved(bike).get("OVERRIDES", []) for k in o})
        self.gcj("activity", "set", bike, "--set", "Average Power=lots", expect=2)

    def test_values_are_checked(self):
        for bad in ("Start Date=nonsense", "Start Time=25:61", "Summary=x", "Commute=maybe", "RPE=hard", "Start Date="):
            self.gcj("activity", "set", "last", "--set", bad, expect=2)
        self.gcj("activity", "set", "last", "--set", "Commute=yes", "--set", "Device=Test Device")
        run = self.gcj("activity", "list", "last", "--field", "Commute")["data"]["activities"][0]
        self.assertEqual(run["metadata"]["Commute"], "1")
        self.assertEqual(self.saved(run["id"])["DEVICETYPE"].strip(), "Test Device")   # the writer adds a space

    def test_start_time_renames_the_file(self):
        self.addCleanup(self.gc, "--athlete", self.athlete, "activity", "set", "2020_01_26_14_30_00",
                        "--set", "Start Time=13:00:38")
        env = self.gcj("activity", "set", "2020_01_26_13_00_38", "--set", "Start Time=14:30")
        self.assertEqual(env["data"]["activities"][0]["renamed"], "2020_01_26_14_30_00")
        files = self.activity_files()
        self.assertIn("2020_01_26_14_30_00.json", files)
        self.assertNotIn("2020_01_26_13_00_38.json", files)
        self.assertEqual(self.gcj("activity", "show", "2020_01_26_14_30_00")["data"]["start"], "2020-01-26T14:30:00")

        # never onto another activity
        run = self.gcj("activity", "show", "last")["data"]["start"]
        env = self.gcj("activity", "set", "2020_01_26_14_30_00", "--set", "Start Date=" + run[:10],
                       "--set", "Start Time=" + run[11:], expect=5)
        self.assertIn("another activity starts", env["data"]["activities"][0]["message"])
        self.assertEqual(self.activity_files(), files)

    def test_linked_defaults_are_filled_in(self):
        path = os.path.join(self.home, "metadata.xml")
        with open(path, encoding="utf-8") as f:
            original = f.read()
        self.addCleanup(lambda: open(path, "w", encoding="utf-8").write(original))
        linked = ('\t\t<default>\n\t\t\t<defaultfield>"Workout Code"</defaultfield>\n'
                  '\t\t\t<defaultvalue>"Z2"</defaultvalue>\n'
                  '\t\t\t<defaultlinkedfield>"Objective"</defaultlinkedfield>\n'
                  '\t\t\t<defaultlinkedvalue>"Endurance"</defaultlinkedvalue>\n\t\t</default>\n')
        self.assertIn("\t</defaults>", original)
        with open(path, "w", encoding="utf-8") as f:
            f.write(original.replace("\t</defaults>", linked + "\t</defaults>", 1))
        self.gcj("activity", "set", "last", "--set", "Workout Code=Z2")
        run = self.gcj("activity", "list", "last", "--field", "Objective", "--field", "Workout Code")["data"]["activities"][0]
        self.assertEqual(run["metadata"]["Workout Code"], "Z2")
        self.assertEqual(run["metadata"]["Objective"], "Endurance")


class TestTrends(Headless):
    """chart trend's periods have the values Trends and metric aggregate give"""

    imports = [RIDE_POWER, RIDE_GPS]

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # the GPS ride has no power: move it into the power ride's month
        r = cls.gc_class("--athlete", cls.athlete, "activity", "set", "2012_01_11_11_51_01",
                         "--set", "Start Date=2020-01-20")
        assert r.code == 0, r

    def trend(self, metric):
        out = os.path.join(self.tmp, "trend.png")
        env = self.gcj("-o", out, "chart", "trend", metric, "--by", "month", "--from", "2020-01-01", "--to", "2020-01-31")
        self.assertEqual(env["data"]["activities"], 2)
        self.assertEqual(len(env["data"]["buckets"]), 1)
        return env["data"]["buckets"][0]["value"]

    def aggregate(self, metric):
        env = self.gcj("metric", "aggregate", "--metric", metric, "--from", "2020-01-01", "--to", "2020-01-31")
        self.assertEqual(env["data"]["activities"], 2)
        return list(env["data"]["values"].values())[0]

    def test_month_matches_the_aggregate(self):
        power = self.trend("Average_Power")
        self.assertEqual(round(power), self.aggregate("Average_Power"))
        # the ride without power doesn't pull the average down
        ride = self.gcj("activity", "show", "2020_01_26_13_00_38")["data"]["metrics"]["average_power"]
        self.assertAlmostEqual(power, ride, places=3)
        self.assertAlmostEqual(self.trend("workout_time"), self.aggregate("workout_time"), places=0)
        self.assertAlmostEqual(self.trend("max_power"), self.aggregate("max_power"), places=0)

class TestActivitiesMetricsCharts(Headless):

    imports = [RIDE_POWER, RIDE_GPS, RUN_STRYD]

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

    def test_show_gives_intervals_as_interval_list(self):
        for activity in ("first", "last"):
            shown = self.gcj("activity", "show", activity)["data"]["intervals"]
            listed = self.gcj("interval", "list", activity)["data"]["intervals"]
            self.assertTrue(shown)
            self.assertEqual(len(shown), len(listed))
            for s, l in zip(shown, listed):
                self.assertEqual({k: v for k, v in s.items() if k != "distance"},
                                 {k: v for k, v in l.items() if k != "metrics"})

    def test_eval(self):
        env = self.gcj("activity", "eval", "--expression", "Duration / 60", "--sport", "Bike")
        self.assertEqual(len(env["data"]["activities"]), 2)
        self.assertTrue(all(a["value"] > 10 for a in env["data"]["activities"]))

    def test_set_validates_fields(self):
        self.gcj("activity", "set", "last", "--set", "Notes=hello")
        self.assertEqual(self.gcj("activity", "list", "last", "--field", "Notes")["data"]["activities"][0]["metadata"]["Notes"], "hello")
        self.gcj("activity", "set", "last", "--set", "Undefined Thing=1", expect=2)
        self.gcj("activity", "set", "--set", "Notes=x", expect=2)  # needs a selection

    def test_set_all_saves_every_activity(self):
        env = self.gcj("activity", "set", "--all", "--set", "Notes=everyone")
        self.assertEqual(env["data"]["updated"], 3)
        listed = self.gcj("activity", "list", "--field", "Notes")["data"]["activities"]
        self.assertEqual([a["metadata"]["Notes"] for a in listed], ["everyone"] * 3)
        for a in listed:
            self.assertEqual(self.gcj("activity", "show", a["id"])["data"]["metadata"]["Notes"], "everyone")
        env = self.gcj("activity", "set", "--all", "--set", "Notes=everyone")
        self.assertEqual(env["data"]["updated"], 0)

    def test_export(self):
        out = os.path.join(self.tmp, "export.tcx")
        env = self.gcj("--output", out, "activity", "export", "last", "--as", "tcx")
        self.assertEqual(env["data"]["output"], out)
        self.assertIn(b"TrainingCenterDatabase", open(out, "rb").read(2000))
        r = self.gca("-o", "-", "activity", "export", "last", "--as", "csv")
        self.assertEqual(r.code, 0)
        self.assertTrue(r.out.startswith(b"Minutes"))
        self.gcj("activity", "export", "last", "--as", "doc", expect=2)

        # a table is not a chart: -o writes the csv, and a bad path is an error
        table = os.path.join(self.tmp, "intervals.csv")
        r = self.gca("--format", "csv", "-o", table, "interval", "list", "last")
        self.assertEqual(r.code, 0, r)
        self.assertEqual(r.out, b"")
        self.assertIn(b"wrote", r.err)
        saved = open(table, "rb").read()
        self.assertTrue(saved.startswith(b"number,name,type"), saved[:80])
        missing = os.path.join(self.tmp, "no-such-dir", "intervals.csv")
        r = self.gca("--format", "csv", "-o", missing, "interval", "list", "last")
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
        text = self.gca("interval", "list", "2020_01_26_13_00_38")
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

    def test_csv(self):
        run = "2024_07_09_15_12_48"
        # a list is a table, with its nested metrics as columns
        rows = self.csv_rows("interval", "list", run, "--metric", "average_power,Duration")
        head, body = rows[0], rows[1:]
        self.assertEqual(head[:4], ["number", "name", "type", "start"])
        intervals = self.gcj("interval", "list", run, "--metric", "average_power")["data"]["intervals"]
        self.assertEqual(len(body), len(intervals))
        self.assertEqual(float(body[1][head.index("average_power")]), intervals[1]["metrics"]["average_power"])
        shown = self.csv_rows("interval", "list", run, "--metric", "workout_time", "--display")
        self.assertRegex(shown[1][shown[0].index("workout_time")], r"^\d+:\d\d")

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

        # an overview table is the column grid, including a single row.
        # several tiles stay one value a line, without the count sentence.
        rows = self.csv_rows("activity", "overview", run, "--tile", "Intervals Data")
        self.assertEqual(rows[0][:2], ["Name", "Pace (min/km)"])
        self.assertEqual(len(rows) - 1, len(intervals))
        self.assertFalse(any("recorded laps:" in cell for row in rows for cell in row))
        bike = "2020_01_26_13_00_38"
        listed = {(t["name"], t["kind"]): t
                  for t in self.gcj("activity", "overview", bike)["data"]["tiles"]}[("Intervals", "table")]
        self.assertEqual(listed["style"], "list")
        self.assertNotIn("_csv_columns", listed)
        one = self.csv_rows("activity", "overview", bike, "--tile", "Intervals")
        self.assertEqual(len(one), 2)
        self.assertNotEqual(one[0], ["name", "value", "units"])
        self.assertEqual(one[0], [r[0] if not r[2] else "%s (%s)" % (r[0], r[2]) for r in listed["rows"]])
        self.assertEqual(one[1], [r[1] for r in listed["rows"]])
        rows = self.csv_rows("activity", "overview", run)
        self.assertEqual(rows[0], ["tile", "kind", "row", "column", "units", "value"])
        self.assertFalse(any(r[1] == "summary" or "recorded laps:" in r[-1] for r in rows))
        self.assertIn(("Sport", "field", "", "value", "", "Run"), [tuple(r) for r in rows])

        # errors go to stderr only
        r = self.gca("--format", "csv", "activity", "show", "1999-01-01")
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
        text = self.gca("activity", "overview", "last", "--tile", "Intervals Data")
        self.assertEqual(text.code, 0, text)
        self.assertIn(b"min/km", text.out)
        self.assertIn(b"recorded laps:", text.out)
        bike = self.gca("activity", "overview", "2020_01_26_13_00_38", "--tile", "Intervals")
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
                r = self.gca("-o", out, "chart", *chart, "--as", fmt, "--width", "800", "--height", "400")
                self.assertEqual(r.code, 0, (chart, r))
                data = open(out, "rb").read()
                self.assertTrue(data.startswith(magic[fmt]), (chart, fmt, data[:20]))
        env = self.gcj("-o", os.path.join(self.tmp, "t.png"), "chart", "trend", "workout_time", "--by", "activity")
        self.assertEqual(env["data"]["periods"], env["data"]["activities"])
        r = self.gca("chart", "activity", "last", "--series", "watts,nothing")
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"unknown series 'nothing'", r.err)
        r = self.gca("chart", "pmc", "--from", "2024-01-01", "--to", "2023-01-01")
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"after --to", r.err)

    def test_delete(self):
        env = self.gcj("import", MULTI_TCX)
        others = [f["activity"] for f in env["data"]["files"][1:]]
        # the others go too, the activity count is as before for other tests
        self.addCleanup(lambda: self.gca("activity", "delete", *others))
        victim = env["data"]["files"][0]["activity"]
        self.gcj("activity", "delete", victim)
        self.assertNotIn(victim + ".json", self.activity_files())
        self.gcj("activity", "show", victim, expect=3)
        # kept as a backup
        self.assertTrue(any(victim in f for f in os.listdir(os.path.join(self.folder, "bak"))))



class TestDelete(Headless):

    imports = [RIDE_POWER, RIDE_GPS, RUN_STRYD]

    def test_several_in_one_call(self):
        ids = [a["id"] for a in self.gcj("activity", "list")["data"]["activities"]]
        self.assertEqual(len(ids), 3)
        env = self.gcj("activity", "delete", *ids)
        self.assertEqual(sorted(env["data"]["deleted"]), sorted(ids))
        self.assertEqual(env["data"]["failed"], [])
        self.assertEqual(self.activity_files(), [])
        self.assertEqual(self.gcj("activity", "list")["data"]["activities"], [])
        backups = os.listdir(os.path.join(self.folder, "bak"))
        self.assertTrue(all(any(i in f for f in backups) for i in ids))

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_server(env, home, log_path, *args):
    """serve on a free port: (process, port). Another process may take the
    port between choosing and binding it, so a server that can't listen is
    tried again on another."""
    for _ in range(3):
        port = free_port()
        with open(log_path, "wb") as log:
            server = subprocess.Popen([BINARY, "--cli", "--home", home, "serve", "--port", str(port)] + list(args),
                                      env=env, stdout=subprocess.DEVNULL, stderr=log)
        deadline = time.time() + 30
        while time.time() < deadline and server.poll() is None:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                    return server, port
            except OSError:
                time.sleep(0.2)
        if server.poll() is None:
            server.kill()
            server.wait()
        with open(log_path, "rb") as log:
            text = log.read().decode("utf-8", "replace")
        if "can't listen" not in text:
            break
    raise RuntimeError("REST server did not start:\n" + text)


def stop_server(server):
    if server.poll() is not None:
        return
    if os.name == "nt":
        server.terminate()      # no SIGINT for child processes on Windows
    else:
        server.send_signal(signal.SIGINT)
    try:
        server.wait(timeout=15)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()


def call_server(port, method, path, body=None, timeout=60):
    """(status, body) of a request to /v1, status None when the connection failed"""
    req = urllib.request.Request("http://127.0.0.1:%d/v1%s" % (port, path), method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except (OSError, ConnectionError) as e:
        return None, str(e).encode()


class TestRest(Headless):

    token = uuid.uuid4().hex

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.server, cls.port = start_server(cls.env, cls.home, os.path.join(cls.tmp, "server.log"), "--token", cls.token)

    @classmethod
    def tearDownClass(cls):
        stop_server(cls.server)
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
        self.assertTrue(lines[0].startswith(b"number,name,type"), lines[0])
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
        env = self.jcall("PUT", "/athletes", expect=405)
        self.assertEqual(env["status"], "method_not_allowed")
        # the athlete again in the query: fine if it's the same one
        self.jcall("GET", "/athletes/%s/activities?athlete=%s" % (self.athlete, self.athlete))
        env = self.jcall("GET", "/athletes/%s/activities?athlete=Other" % self.athlete, expect=400)
        self.assertIn("Other", env["error"])
        self.jcall("POST", "/athletes/%s/imports" % self.athlete, raw=b"xx",
                   headers={"Content-Type": "application/octet-stream"}, expect=400)

    def test_no_server_files_through_the_api(self):
        a = "/athletes/%s" % self.athlete
        for method, path, body in [("PUT", a + "/layouts/Run/tiles/Intervals%20Data", {"file": "/etc/passwd"}),
                                   ("POST", a + "/user-metrics", {"symbol": "x", "name": "X", "file": "-"}),
                                   ("PUT", "/processors/x", {"file": "/etc/passwd"})]:
            env = self.jcall(method, path, body=body, expect=400)
            self.assertIn("only for the command line", env["error"])
        doc = self.jcall("GET", "/openapi.json")
        tile = doc["paths"]["/v1/athletes/{athlete}/layouts/{layout}/tiles/{tile}"]["put"]
        props = tile["requestBody"]["content"]["application/json"]["schema"]["properties"]
        self.assertIn("program", props)
        self.assertNotIn("file", props)

    def test_user_metric_names_follow_edits(self):
        """one server, the metrics change between requests"""
        a = "/athletes/" + self.athlete
        # (not a ride the workflow test imports)
        with open(RIDE_GPS, "rb") as f:
            self.call("POST", a + "/imports?filename=gps.fit", raw=f.read(), headers={"Content-Type": "application/octet-stream"})
        self.jcall("POST", a + "/user-metrics", body={"symbol": "test_metric", "name": "Test Metric", "program": ONES})
        self.addCleanup(self.call, "DELETE", a + "/user-metrics/test_metric")
        self.addCleanup(self.call, "DELETE", a + "/user-metrics/renamed_metric")
        self.jcall("GET", a + "/charts/trend?metric=Test%20Metric&envelope=1")

        self.jcall("PATCH", a + "/user-metrics/test_metric", body={"rename": "renamed_metric", "name": "Renamed Metric"})
        env = self.jcall("GET", a + "/charts/trend?metric=Test%20Metric&envelope=1", expect=400)
        self.assertIn("unknown metric", env["error"])
        self.jcall("GET", a + "/charts/pmc?metric=Test%20Metric&envelope=1", expect=400)
        self.jcall("GET", a + "/charts/trend?metric=Renamed%20Metric&envelope=1")
        self.jcall("GET", a + "/charts/trend?metric=renamed_metric&envelope=1")

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

    def test_chart_library_routes(self):
        a = "/athletes/%s/charts" % self.athlete
        charts = os.path.join(self.folder, "config", "charts.xml")
        self.addCleanup(lambda: os.path.exists(charts) and os.remove(charts))
        self.jcall("POST", a, body={"name": "Rest", "metric": ["average_power"]})
        env = self.jcall("POST", a + "/Rest/curves", body={"estimate": "cp", "model": "cp2", "wpk": True})
        self.assertEqual([m["type"] for m in env["data"]["metrics"]], ["metric", "estimate"])
        env = self.jcall("PUT", a + "/Rest/curves/2", body={"color": "123456"})
        self.assertEqual(env["data"]["metrics"][1]["color"], "123456")
        self.assertTrue(env["data"]["metrics"][1]["wpk"])
        env = self.jcall("DELETE", a + "/Rest/curves/1")
        self.assertEqual([m["type"] for m in env["data"]["metrics"]], ["estimate"])
        self.jcall("PUT", a + "/Rest/curves/5", body={"color": "123456"}, expect=400)
        self.assertEqual(self.jcall("GET", a + "/Rest")["data"]["name"], "Rest")
        self.jcall("DELETE", a + "/Rest")
        self.jcall("GET", a + "/Rest", expect=400)

    def test_serve_options_are_range_checked(self):
        for args in (("--port", "0"), ("--port", "70000"), ("--max-upload", "4096")):
            r = self.gc("serve", *args, timeout=60)
            self.assertEqual(r.code, 2, (args, r))
            self.assertIn(args[0].encode(), r.err)

    def test_network_listening_needs_a_token(self):
        r = self.gc("serve", "--host", "0.0.0.0", "--port", free_port(), timeout=60)
        self.assertEqual(r.code, 2, r)
        self.assertIn(b"needs a token", r.err)

    def test_athlete_is_closed_between_requests(self):
        self.jcall("GET", "/athletes/%s" % self.athlete)
        self.assertClosed()



@unittest.skipIf(os.name == "nt", "no SIGINT for child processes on Windows")
class TestRestShutdown(Headless):
    """ctrl-c while requests wait their turn stops the server, and no client is told a dropped call succeeded"""

    def test_interrupt_with_requests_queued(self):
        self.requirePython(self.gcj("version")["data"]["python"])
        # a slow request: a processor that takes its time
        r = self.gca("import", RIDE_POWER)
        self.assertEqual(r.code, 0, r)
        r = self.gc("processor", "install", "slow", "--source", "import time\ntime.sleep(4)\n")
        self.assertEqual(r.code, 0, r)
        server, port = start_server(self.env, self.home, os.path.join(self.tmp, "shutdown.log"))
        try:
            answers = []

            def call(method, path, body=None):
                answers.append(call_server(port, method, path, body))

            a = "/athletes/" + self.athlete
            threads = [threading.Thread(target=call, args=("POST", a + "/processors/slow/runs", {"all": True}))]
            threads[0].start()
            time.sleep(0.5)
            for _ in range(3):
                threads.append(threading.Thread(target=call, args=("GET", a + "/activities")))
                threads[-1].start()
            time.sleep(0.5)
            server.send_signal(signal.SIGINT)
            server.wait(timeout=15)
            for t in threads:
                t.join(timeout=30)
        finally:
            if server.poll() is None:
                server.kill()
        self.assertEqual(len(answers), 4)
        for status, body in answers:
            self.assertIn(status, (200, 503, None), body)
            if status == 200:
                self.assertTrue(json.loads(body)["ok"], body)
        self.assertClosed()

ONES = "{\n    value { 1; }\n}\n"
TWOS = "{\n    value { 2; }\n}\n"


class TestUserMetricsAndZones(Headless):

    imports = [RIDE_POWER]

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

        r = self.gca("interval", "list", "last")
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

        # none left: the intervals table has no metric columns, as in the GUI
        listed = self.gcj("metric", "favourite", "list")["data"]["metrics"]
        self.addCleanup(self.gc, "--athlete", self.athlete, "metric", "favourite", "set", *listed)
        self.gcj("metric", "favourite", "remove", *listed)
        self.assertEqual(self.gcj("metric", "favourite", "list")["data"]["metrics"], [])
        intervals = self.gcj("interval", "list", "last")["data"]["intervals"]
        self.assertTrue(intervals)
        self.assertEqual([i.get("metrics", {}) for i in intervals], [{}] * len(intervals))

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



def running_as_root():
    return hasattr(os, "geteuid") and os.geteuid() == 0


@unittest.skipIf(os.name == "nt" or running_as_root(), "file permissions don't stop Windows or root")
class TestReadOnlyFolders(Headless):
    """a folder that can't be written: a command fails promptly and says why, and changes nothing"""

    imports = [RIDE_POWER]

    def read_only(self, *paths):
        """chmod a-w, undone when the test ends"""
        for path in paths:
            for root, dirs, files in os.walk(path, topdown=False):
                for name in files + dirs:
                    full = os.path.join(root, name)
                    mode = os.stat(full).st_mode
                    os.chmod(full, mode & ~0o222)
                    self.addCleanup(os.chmod, full, mode)
            mode = os.stat(path).st_mode
            os.chmod(path, mode & ~0o222)
            self.addCleanup(os.chmod, path, mode)

    def assertFails(self, *args, mentions):
        r = self.gca(*args, timeout=60)
        self.assertEqual(r.code, 5, r)
        self.assertIn(mentions.encode(), r.err + r.out, r)   # the envelope carries it with --format json
        return r

    def test_athlete_config(self):
        config = os.path.join(self.folder, "config")
        with open(os.path.join(config, "power.zones"), "rb") as f:
            zones = f.read()
        self.read_only(config)
        self.assertFails("zones", "set", "--from", "2026-01-01", "--cp", "260", mentions="power.zones")
        self.assertFails("zones", "set", "--type", "hr", "--from", "2026-01-01", "--lthr", "170", mentions="hr.zones")
        self.assertFails("measures", "add", "--when", "2026-01-01", "--set", "WEIGHTKG=70", mentions="bodymeasures.json")
        with open(os.path.join(config, "power.zones"), "rb") as f:
            self.assertEqual(f.read(), zones)

    def test_read_only_activities(self):
        activities = os.path.join(self.folder, "activities")
        before = {f: open(os.path.join(activities, f), "rb").read() for f in os.listdir(activities)}
        self.read_only(activities)
        r = self.assertFails("--format", "json", "activity", "set", "last", "--set", "Notes=can't be saved",
                             mentions="not updated")
        report = json.loads(r.out)["data"]["activities"]
        self.assertEqual(report[0]["status"], "failed")
        self.assertIn(".json", report[0]["message"])
        self.assertEqual({f: open(os.path.join(activities, f), "rb").read() for f in os.listdir(activities)}, before)

        env = json.loads(self.assertFails("--format", "json", "activity", "delete", "last",
                                          mentions="could not be moved").out)
        self.assertEqual(env["data"]["deleted"], [])
        self.assertEqual(len(env["data"]["failed"]), 1)
        self.assertEqual(sorted(os.listdir(activities)), sorted(before))

        self.assertFails("import", RIDE_GPS, mentions="could not move")
        self.assertEqual(sorted(os.listdir(activities)), sorted(before))

    def test_read_only_charts(self):
        charts = os.path.join(self.folder, "config", "charts.xml")
        self.addCleanup(lambda: os.path.exists(charts) and os.remove(charts))
        r = self.gca("chart", "library", "add", "--name", "Speed", "--metric", "average_speed")
        self.assertEqual(r.code, 0, r)
        self.read_only(charts)
        started = time.time()
        r = self.gca("activity", "list", timeout=60)
        self.assertEqual(r.code, 0, r)
        self.assertLess(time.time() - started, 30)
        self.assertFails("chart", "library", "add", "--name", "Cadence", "--metric", "average_cad", mentions="charts.xml")

    def test_athlete_create_leaves_nothing_behind(self):
        # with this umask a new folder can't take sub folders, so the zones can't be written
        r = self.gc("athlete", "create", "Half", timeout=60, preexec_fn=lambda: os.umask(0o577))
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"power.zones", r.err)
        self.assertFalse(os.path.exists(os.path.join(self.home, "Half")))

    def test_settings_shared_by_all_athletes(self):
        # the files the athletes folder holds, and the folder itself
        shared = [os.path.join(self.home, f) for f in os.listdir(self.home) if os.path.isfile(os.path.join(self.home, f))]
        self.read_only(*shared)
        mode = os.stat(self.home).st_mode
        os.chmod(self.home, mode & ~0o222)
        self.addCleanup(os.chmod, self.home, mode)
        self.assertFails("metric", "user", "add", "--symbol", "ones", "--name", "Ones", "--program", ONES,
                         mentions="usermetrics.xml")
        self.assertFails("field", "add", "Read Only Field", mentions="metadata.xml")
        self.assertFalse(os.path.exists(os.path.join(self.home, "usermetrics.xml")))

HEART_RATE_TABLE = """{
names {
    metricname(name, Average_Heart_Rate);
}
units {
    metricunit(name, Average_Heart_Rate);
}
values {
    c(intervalstrings(name), intervalstrings(Average_Heart_Rate));
}
i {
    intervalstrings(name);
}
}
"""


class TestLayoutTiles(Headless):

    imports = [RUN_STRYD]

    def perspectives_file(self):
        return os.path.join(self.folder, "config", "analysis-perspectives.xml")

    def test_tile_program_is_checked_then_shown(self):
        path = self.perspectives_file()
        self.assertFalse(os.path.exists(path))

        layouts = {l["name"]: l["expression"] for l in self.gcj("layout", "list")["data"]["layouts"]}
        self.assertEqual(layouts["Run"], "isRun")
        self.gcj("layout", "tile", "list", expect=2)

        tiles = self.gcj("layout", "tile", "list", "--layout", "Run")["data"]["tiles"]
        kinds = {t["name"]: t["kind"] for t in tiles}
        self.assertEqual(kinds["Intervals Data"], "table")
        self.assertEqual(kinds["Route"], "route")

        shown = self.gcj("layout", "tile", "show", "Intervals Data", "--layout", "Run")["data"]
        self.assertIn("Average_Heart_Rate", shown["program"])
        swim = self.gcj("layout", "tile", "show", "Intervals Data", "--layout", "Swim")["data"]["program"]
        self.assertIn("Pace_Swim", swim)

        self.gcj("layout", "tile", "set", "Intervals Data", "--layout", "Run",
                 "--program", "{ names { metricname(HRR/v); } }", expect=2)
        self.gcj("layout", "tile", "set", "Intervals Data", "--layout", "Run",
                 "--program", "{ names { metricname(Average_Heart_Rate)", expect=2)
        self.gcj("layout", "tile", "set", "Route", "--layout", "Run",
                 "--program", HEART_RATE_TABLE, expect=2)
        self.assertFalse(os.path.exists(path))

        env = self.gcj("layout", "tile", "set", "Intervals Data", "--layout", "Run",
                       "--program", HEART_RATE_TABLE)
        self.assertEqual(env["data"]["status"], "updated")
        self.assertTrue(os.path.exists(path))
        self.assertEqual(self.gcj("layout", "tile", "show", "Intervals Data", "--layout", "Run")["data"]["program"],
                         HEART_RATE_TABLE)
        self.assertEqual(self.gcj("layout", "tile", "show", "Intervals Data", "--layout", "Swim")["data"]["program"],
                         swim)

        table = [t for t in self.gcj("activity", "overview", "last", "--tile", "Intervals Data")["data"]["tiles"]][0]
        names = [c["name"] for c in table["columns"]]
        self.assertEqual(names, ["Name", "Average Heart Rate"])
        self.assertGreater(len(table["rows"]), 1)
        self.assertClosed()


class TestChartLibrary(Headless):

    def setUp(self):
        # every test starts from the built-in charts and no user metrics
        self.drop_charts()
        self.addCleanup(self.drop_charts)
        self.keep(os.path.join(self.home, "usermetrics.xml"))

    def drop_charts(self):
        if os.path.exists(self.charts_file()):
            os.remove(self.charts_file())

    def charts_file(self):
        return os.path.join(self.folder, "config", "charts.xml")

    def stored_chart(self, name):
        """one chart as charts.xml stores it: a base64 QDataStream, strings in UTF-16"""
        with open(self.charts_file(), encoding="utf-8") as f:
            xml = f.read()
        for chart, blob in re.findall(r'<chart name="(.*?)">"(.*?)"</chart>', xml):
            if html.unescape(chart) == name:
                return base64.b64decode(blob)
        self.fail("no chart %s in charts.xml" % name)

    def test_library_filters_are_stored_as_the_gui_stores_them(self):
        path = self.charts_file()
        charts = self.gcj("chart", "library", "list")["data"]["charts"]
        chart = next(c for c in charts if c["metrics"] and c["metrics"][0]["type"] == "metric")
        first = chart["metrics"][0]
        self.assertIsNone(first["filter"])

        edited = self.gcj("chart", "library", "curve", "edit", chart["name"], "1", "--color", "445566")["data"]["metrics"][0]
        self.assertEqual(edited["color"], "445566")
        self.assertEqual({k: v for k, v in edited.items() if k != "color"}, {k: v for k, v in first.items() if k != "color"})
        self.assertIn("search:".encode("utf-16-be"), self.stored_chart(chart["name"]))

        edited = self.gcj("chart", "library", "curve", "edit", chart["name"], "1", "--filter", "isRun")["data"]["metrics"][0]
        self.assertEqual(edited["filter"], "isRun")
        self.assertIn("filter:isRun".encode("utf-16-be"), self.stored_chart(chart["name"]))
        edited = self.gcj("chart", "library", "curve", "edit", chart["name"], "1", "--filter", "")["data"]["metrics"][0]
        self.assertIsNone(edited["filter"])
        self.assertNotIn("filter:isRun".encode("utf-16-be"), self.stored_chart(chart["name"]))
        self.assertIn("search:".encode("utf-16-be"), self.stored_chart(chart["name"]))

    def test_library_file_is_only_written_by_a_change(self):
        path = self.charts_file()
        self.gcj("chart", "library", "add", "--name", "Speed", "--metric", "average_speed")
        # a write on close would change the time stamp
        t = os.stat(path).st_mtime - 100
        os.utime(path, (t, t))
        with open(path, "rb") as f:
            saved = f.read()
        self.gcj("activity", "list")
        self.gcj("chart", "library", "list")
        self.assertEqual(os.stat(path).st_mtime, t)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), saved)

    def test_library_keeps_the_curves_of_a_removed_metric(self):
        path = self.charts_file()
        self.gcj("metric", "user", "add", "--symbol", "keep_me", "--name", "Keep me", "--program", ONES)
        self.gcj("chart", "library", "add", "--name", "Kept", "--metric", "keep_me", "--metric", "average_power")
        with open(path, "rb") as f:
            saved = f.read()

        self.gcj("metric", "user", "remove", "keep_me")
        self.gcj("activity", "list")
        with open(path, "rb") as f:
            self.assertEqual(f.read(), saved)

        r = self.gca("chart", "library", "add", "--name", "Other", "--metric", "average_speed")
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"'Kept' (keep_me)", r.err)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), saved)

        self.gcj("chart", "library", "add", "--name", "Other", "--metric", "average_speed", "--drop-unknown")
        shown = self.gcj("chart", "library", "show", "Kept")["data"]["metrics"]
        self.assertEqual([m["symbol"] for m in shown], ["average_power"])

    def test_library_names_curves_as_curve_settings(self):
        path = self.charts_file()
        shown = self.gcj("chart", "library", "add", "--name", "Names", "--best", "20", "--unit", "min",
                         "--series", "heartrate")["data"]["metrics"]
        self.assertEqual(shown[0]["detail"], "20 min heartrate")
        self.assertEqual(shown[0]["name"], "Peak 20 minute Heartrate")
        shown = self.gcj("chart", "library", "curve", "add", "Names", "--estimate", "best", "--duration", "5",
                         "--unit", "min", "--model", "cp3")["data"]["metrics"]
        self.assertEqual(shown[1]["detail"], "Estimate 5 minutes Power (cp3)")
        refused = self.gcj("chart", "library", "curve", "add", "Names", "--estimate", "pmax", "--model", "cp2", expect=2)
        self.assertIn("it offers wprime, cp, best, ei, vo2max", refused["error"])

    def test_library_round_trip_is_checked(self):
        path = self.charts_file()
        self.assertFalse(os.path.exists(path))

        listed = self.gcj("chart", "library", "list")["data"]["charts"]
        names = [c["name"] for c in listed]
        self.assertIn("PMC (Coggan)", names)
        self.assertNotIn("P v", names)
        self.assertFalse(os.path.exists(path))

        self.gcj("chart", "library", "add", "--name", "P v", "--metric", "p_v", expect=2)
        self.gcj("chart", "library", "add", "--name", "   ", "--metric", "average_power", expect=2)
        self.gcj("chart", "library", "add", "--name", "Nope", "--metric", "average_power", "--by", "fortnight", expect=2)
        self.gcj("chart", "library", "remove", "P v", expect=2)
        self.assertFalse(os.path.exists(path))

        self.gcj("metric", "user", "add", "--symbol", "p_v", "--name", "P v",
                 "--type", "average", "--units", "W/kph", "--precision", "2",
                 "--program", "{\n    value { 1; }\n}\n")

        env = self.gcj("chart", "library", "add", "--name", "P v", "--metric", "p_v")
        self.assertEqual(env["data"]["status"], "added")
        self.assertEqual(env["data"]["by"], "week")
        self.assertEqual(env["data"]["metrics"], [{
            "index": 1,
            "type": "metric", "symbol": "p_v", "formula": "P_v", "name": "P v", "detail": "p_v",
            "style": "line", "marker": "circle", "color": "0078d4", "fill": False, "filter": None,
            "units": "W/kph",
        }])
        self.assertTrue(os.path.exists(path))
        shown = self.gcj("chart", "library", "show", "P v")["data"]
        self.assertEqual(shown["by"], "week")
        self.assertEqual(shown["metrics"][0]["symbol"], "p_v")
        self.assertEqual(shown["metrics"][0]["type"], "metric")

        with open(path, "rb") as f:
            saved = f.read()
        self.assertIn(b"<charts", saved)
        self.gcj("chart", "library", "add", "--name", "P v", "--metric", "average_power", expect=2)
        self.gcj("chart", "library", "edit", "P v", "--metric", "not_a_metric", expect=2)
        self.gcj("chart", "library", "edit", "P v", expect=2)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), saved)

        env = self.gcj("chart", "library", "edit", "P v", "--name", "Power and speed",
                       "--metric", "average_power", "--metric", "Average_Speed", "--by", "month")
        self.assertEqual(env["data"]["status"], "updated")
        self.assertEqual(env["data"]["name"], "Power and speed")
        self.assertEqual(env["data"]["by"], "month")
        self.assertEqual([m["symbol"] for m in env["data"]["metrics"]], ["average_power", "average_speed"])
        self.gcj("chart", "library", "show", "P v", expect=2)
        names = [c["name"] for c in self.gcj("chart", "library", "list")["data"]["charts"]]
        self.assertIn("Power and speed", names)
        self.assertIn("PMC (Coggan)", names)

        with open(path, "rb") as f:
            edited = f.read()
        self.gcj("chart", "library", "remove", "Power and speed")
        self.gcj("chart", "library", "remove", "Power and speed", expect=2)
        with open(path, "rb") as f:
            self.assertNotEqual(f.read(), edited)
        names = [c["name"] for c in self.gcj("chart", "library", "list")["data"]["charts"]]
        self.assertNotIn("Power and speed", names)
        self.assertIn("PMC (Coggan)", names)
        self.assertClosed()

    def test_library_curves_keep_their_own_drawing(self):
        self.gcj("chart", "library", "add", "--name", "Estimated VO2max", "--by", "day",
                 "--metric", "vo2max", "--style", "dots", "--marker", "none")
        text = self.gca("chart", "library", "show", "Estimated VO2max")
        self.assertEqual(text.code, 0, text)
        self.assertIn(b"metric  vo2max  dots  none", text.out)

        self.gcj("chart", "library", "add", "--name", "Two",
                 "--metric", "average_power", "--metric", "average_speed", "--style", "dots", expect=2)
        refused = self.gcj("chart", "library", "add", "--name", "Banister", "--banister", expect=2)
        self.assertIn("not supported", refused["error"])
        refused = self.gcj("chart", "library", "add", "--name", "Bad",
                           "--estimate", "ftp", "--model", "cp2", expect=2)
        self.assertIn("ftp", refused["error"])

        best = self.gcj("chart", "library", "add", "--name", "Peak power", "--by", "day",
                        "--best", "45", "--unit", "min", "--series", "power",
                        "--style", "dots", "--marker", "circle")["data"]["metrics"]
        self.assertEqual(best[0]["type"], "best")
        self.assertEqual(best[0]["detail"], "45 min power")
        self.assertEqual(best[0]["series"], "power")
        self.assertEqual(best[0]["style"], "dots")
        self.assertEqual(best[0]["marker"], "circle")

        self.gcj("chart", "library", "curve", "add", "Peak power", "--estimate", "cp", "--model", "cp2")
        self.gcj("chart", "library", "curve", "add", "Peak power",
                 "--metric", "average_power", "--filter", "isRun", "--fill", "--color", "112233")
        shown = self.gcj("chart", "library", "show", "Peak power")["data"]["metrics"]
        self.assertEqual(shown[1]["type"], "estimate")
        self.assertEqual(shown[1]["detail"], "CP (cp2)")
        self.assertEqual(shown[1]["model"], "cp2")
        self.assertEqual(shown[1]["estimate"], "cp")
        self.assertEqual(shown[1]["style"], "line")
        self.assertEqual(shown[1]["marker"], "none")
        self.assertEqual(shown[2]["filter"], "isRun")
        self.assertIn("filter:isRun".encode("utf-16-be"), self.stored_chart("Peak power"))
        self.assertEqual(shown[2]["fill"], True)
        self.assertEqual(shown[2]["color"], "112233")
        self.assertEqual(shown[2]["style"], "line")
        self.assertEqual(shown[2]["marker"], "circle")

        self.gcj("chart", "library", "curve", "edit", "Peak power", "1", "--style", "line", "--marker", "square")
        shown = self.gcj("chart", "library", "show", "Peak power")["data"]["metrics"]
        self.assertEqual(shown[0]["style"], "line")
        self.assertEqual(shown[0]["marker"], "square")
        self.assertEqual(shown[0]["detail"], "45 min power")
        self.assertEqual(shown[1]["detail"], "CP (cp2)")
        self.assertEqual(shown[2]["filter"], "isRun")

        shown = self.gcj("chart", "library", "edit", "Peak power", "--by", "month")["data"]
        self.assertEqual(shown["by"], "month")
        self.assertEqual([m["type"] for m in shown["metrics"]], ["best", "estimate", "metric"])

        path = self.charts_file()
        with open(path, "rb") as f:
            saved = f.read()
        self.gcj("chart", "library", "curve", "add", "Peak power",
                 "--metric", "average_speed", "--filter", "this is not a filter !!!", expect=2)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), saved)

        self.gcj("chart", "library", "curve", "remove", "Peak power", "3")
        shown = self.gcj("chart", "library", "show", "Peak power")["data"]["metrics"]
        self.assertEqual([m["type"] for m in shown], ["best", "estimate"])
        self.gcj("chart", "library", "remove", "Estimated VO2max")
        self.gcj("chart", "library", "remove", "Peak power")
        self.assertClosed()

    def test_library_built_in_charts_are_the_ground_truth(self):
        curves = {m["detail"]: m for m in self.gcj("chart", "library", "show", "CP History")["data"]["metrics"]}
        self.assertEqual({d: curves[d]["units"] for d in ("CP (ext)", "W' (ext)", "p-Max (ext)")},
                         {"CP (ext)": "Watts", "W' (ext)": "Joules", "p-Max (ext)": "Watts"})
        self.assertFalse(curves["CP (ext)"]["wpk"])
        best = next(m for m in self.gcj("chart", "library", "show", "CP Analysis")["data"]["metrics"] if m["type"] == "best")
        self.assertEqual((best["detail"], best["units"]), ("30 min power", "Watts"))
        self.assertFalse(os.path.exists(self.charts_file()))

        # made here, they are as the built-in ones
        shown = self.gcj("chart", "library", "add", "--name", "Mine", "--best", "30", "--unit", "min",
                         "--series", "power")["data"]["metrics"]
        self.assertEqual(shown[0]["units"], "Watts")
        for estimate, units in (("cp", "Watts"), ("wprime", "Joules"), ("pmax", "Watts"), ("vo2max", "ml/min/kg")):
            shown = self.gcj("chart", "library", "curve", "add", "Mine", "--estimate", estimate, "--model", "ext")["data"]["metrics"]
            self.assertEqual(shown[-1]["units"], units, estimate)

    def test_library_estimates_per_kilogram(self):
        shown = self.gcj("chart", "library", "add", "--name", "Per kg", "--estimate", "cp", "--model", "cp2",
                         "--wpk")["data"]["metrics"]
        self.assertTrue(shown[0]["wpk"])
        self.assertEqual(shown[0]["units"], "Watts/kg")
        # a drawing, or another estimate, keeps it per kilogram
        shown = self.gcj("chart", "library", "curve", "edit", "Per kg", "1", "--color", "aabbcc")["data"]["metrics"]
        self.assertTrue(shown[0]["wpk"])
        shown = self.gcj("chart", "library", "curve", "edit", "Per kg", "1", "--estimate", "cp", "--model", "cp3")["data"]["metrics"]
        self.assertTrue(shown[0]["wpk"])
        self.assertEqual(shown[0]["model"], "cp3")
        shown = self.gcj("chart", "library", "curve", "edit", "Per kg", "1", "--estimate", "cp", "--model", "cp3",
                         "--wpk=false")["data"]["metrics"]
        self.assertFalse(shown[0]["wpk"])
        shown = self.gcj("chart", "library", "curve", "edit", "Per kg", "1", "--units", "W")["data"]["metrics"]
        self.assertEqual(shown[0]["units"], "W")

    def test_library_refuses_what_it_would_drop(self):
        self.gcj("chart", "library", "add", "--name", "Refusals", "--metric", "average_power")
        with open(self.charts_file(), "rb") as f:
            saved = f.read()
        for args in (("chart", "library", "edit", "Refusals", "--name", "X", "--series", "power"),
                     ("chart", "library", "edit", "Refusals", "--by", "day", "--model", "cp2"),
                     ("chart", "library", "edit", "Refusals", "--name", "X", "--wpk"),
                     ("chart", "library", "curve", "add", "Refusals", "--metric", "average_speed", "--wpk"),
                     ("chart", "library", "curve", "add", "Refusals", "--metric", "average_speed", "--series", "power"),
                     ("chart", "library", "curve", "edit", "Refusals", "9", "--color", "112233"),
                     ("chart", "library", "curve", "remove", "Refusals", "0")):
            self.gcj(*args, expect=2)
        with open(self.charts_file(), "rb") as f:
            self.assertEqual(f.read(), saved)

        # a curve can become another kind
        shown = self.gcj("chart", "library", "curve", "edit", "Refusals", "1", "--best", "5", "--unit", "min",
                         "--series", "heartrate")["data"]["metrics"]
        self.assertEqual((shown[0]["type"], shown[0]["detail"]), ("best", "5 min heartrate"))



def seasons_xml(folder):
    """the athlete's seasons.xml as the GUI's parser sees it: name -> element"""
    import xml.etree.ElementTree as ET
    path = os.path.join(folder, "config", "seasons.xml")
    if not os.path.exists(path):
        return {}
    return {e.findtext("name").strip(): e for e in ET.parse(path).getroot().findall("season")}


def day(offset=0):
    return (datetime.date.today() + datetime.timedelta(days=offset)).isoformat()


class TestSeasons(Headless):
    """season, phase and event commands write seasons.xml as the Trends sidebar does"""

    imports = [RIDE_POWER, RUN_STRYD]   # bike 2020-01-26, run 2024-07-09

    def setUp(self):
        self.keep(os.path.join(self.folder, "config", "seasons.xml"))

    def seasons(self):
        return {s["name"]: s for s in self.gcj("season", "list")["data"]["seasons"] if s["kind"] == "season"}

    def test_add_list_show_absolute_and_relative(self):
        env = self.gcj("season", "add", "2020 Season", "--from", "2020-01-01", "--to", "2020-12-31",
                       "--seed", "40", "--low", "-30")
        self.assertEqual(env["data"]["status"], "added")
        self.assertTrue(env["data"]["file"].endswith("seasons.xml"))
        self.gcj("season", "add", "Rolling", "--start-ago", "3", "--start-unit", "months", "--length", "2m")
        self.gcj("season", "add", "Block", "--type", "cycle", "--from", "2024-07-01", "--to", "2024-07-31")
        self.gcj("season", "add", "So far", "--from", "2026-01-01", "--ytd")
        self.gcj("season", "add", "Taper", "--end-ago", "0", "--length", "10d")

        xml = seasons_xml(self.folder)
        self.assertEqual(xml["2020 Season"].findtext("startdate"), "2020-01-01")
        self.assertEqual(xml["2020 Season"].findtext("enddate"), "2020-12-31")
        self.assertEqual((xml["2020 Season"].findtext("seed"), xml["2020 Season"].findtext("low")), ("40", "-30"))
        self.assertEqual(xml["Rolling"].findtext("startoffset"), "1-3")     # months, 3 ago
        self.assertEqual(xml["Rolling"].findtext("length"), "0-2-0")
        self.assertIsNone(xml["Rolling"].find("startdate"))
        self.assertEqual(xml["Block"].findtext("type"), "1")
        self.assertIsNotNone(xml["So far"].find("ytd"))
        self.assertEqual(xml["Taper"].findtext("endoffset"), "2-0")         # weeks, 0 ago
        self.assertEqual(xml["Taper"].findtext("length"), "0-0-10")

        # read back by the GUI's parser in a new process; the sidebar adds at the top
        listed = self.gcj("season", "list")["data"]["seasons"]
        self.assertEqual(listed[0]["name"], "Taper")
        seasons = self.seasons()
        self.assertEqual((seasons["2020 Season"]["start"], seasons["2020 Season"]["end"]), ("2020-01-01", "2020-12-31"))
        self.assertFalse(seasons["2020 Season"]["builtin"])
        self.assertTrue(seasons["This Year"]["builtin"])
        self.assertEqual(seasons["Taper"]["end"], day())
        self.assertEqual(seasons["Taper"]["start"], day(-9))
        self.assertEqual(seasons["Block"]["type"], "cycle")

        shown = self.gcj("season", "show", "rolling")["data"]   # names are case-insensitive
        self.assertEqual(shown["definition"]["start"], {"kind": "ago", "ago": 3, "unit": "months"})
        self.assertEqual(shown["definition"]["end"], {"kind": "length after start", "length": "2m"})
        self.assertFalse(shown["absolute"])
        self.assertEqual(self.gcj("season", "show", shown["id"])["data"]["name"], "Rolling")
        self.assertEqual(self.gcj("season", "show", shown["id"].strip("{}"))["data"]["name"], "Rolling")
        self.assertEqual(self.gcj("season", "show", "Taper")["data"]["definition"]["start"],
                         {"kind": "length before end", "length": "10d"})
        r = self.gca("season", "show", "2020 Season")
        self.assertIn(b"seed   40", r.out)
        self.gcj("season", "show", "Nope", expect=3)
        self.assertClosed()

    def test_refusals(self):
        for args in (("X", "--from", "2020-01-01"),                              # no end
                     ("X", "--to", "2020-01-01"),                                # no start
                     ("X", "--from", "2020-03-01", "--to", "2020-02-01"),        # backwards
                     ("X", "--from", "2020-01-01", "--to", "2020-02-01", "--length", "1m"),
                     ("X", "--start-ago", "2", "--from", "2020-01-01", "--to", "2020-02-01"),
                     ("X", "--length", "1m", "--ytd"),
                     ("X", "--length", "13m", "--from", "2020-01-01"),
                     ("X", "--start-ago", "60", "--length", "1m"),
                     ("X", "--type", "adhoc", "--start-ago", "2", "--length", "1m"),
                     ("X", "--from", "2020-01-01", "--to", "2020-02-01", "--seed", "301"),
                     ("This Year", "--from", "2020-01-01", "--to", "2020-02-01"),  # the name is taken
                     ("", "--from", "2020-01-01", "--to", "2020-02-01")):
            self.gcj("season", "add", *args, expect=2)
        self.assertFalse(os.path.exists(os.path.join(self.folder, "config", "seasons.xml")))

        # built-in ranges can't be changed
        self.gcj("season", "edit", "This Year", "--name", "Mine", expect=2)
        self.gcj("season", "remove", "Last 6 weeks", expect=2)
        self.gcj("season", "phase", "add", "This Year", "Base", expect=2)
        self.gcj("event", "add", "This Year", "Race", expect=2)
        self.assertFalse(os.path.exists(os.path.join(self.folder, "config", "seasons.xml")))

    def test_edit_and_remove(self):
        self.gcj("season", "add", "Spring", "--from", "2026-03-01", "--to", "2026-05-31")
        env = self.gcj("season", "edit", "Spring", "--name", "Early", "--to", "2026-06-30", "--seed", "20")
        self.assertEqual((env["data"]["name"], env["data"]["start"], env["data"]["end"], env["data"]["seed"]),
                         ("Early", "2026-03-01", "2026-06-30", 20))
        # --length alone replaces the end, a new start keeps the end
        self.assertEqual(self.gcj("season", "edit", "Early", "--length", "1m")["data"]["end"], "2026-03-31")
        self.assertEqual(seasons_xml(self.folder)["Early"].findtext("length"), "0-1-0")
        env = self.gcj("season", "edit", "Early", "--from", "2026-03-10")
        self.assertEqual((env["data"]["start"], env["data"]["end"]), ("2026-03-10", "2026-04-09"))
        env = self.gcj("season", "edit", "Early", "--type", "adhoc", "--to", "2026-04-30")
        self.assertEqual(env["data"]["type"], "adhoc")
        self.gcj("season", "edit", "Early", "--start-ago", "2", expect=2)     # adhoc has fixed dates
        self.gcj("season", "edit", "Early", expect=2)                         # nothing to change
        self.assertEqual(seasons_xml(self.folder)["Early"].findtext("type"), "2")

        self.gcj("season", "add", "Other", "--from", "2026-03-01", "--to", "2026-05-31")
        self.gcj("season", "edit", "Other", "--name", "early", expect=2)      # taken
        self.assertEqual(self.gcj("season", "remove", "Early")["data"]["status"], "removed")
        self.assertEqual(list(seasons_xml(self.folder)), ["Other"])
        self.gcj("season", "remove", "Early", expect=3)

    def test_ambiguous_names_list_the_ids(self):
        # two seasons with one name, as the GUI allows
        self.gcj("season", "add", "Twin", "--from", "2026-01-01", "--to", "2026-02-01")
        path = os.path.join(self.folder, "config", "seasons.xml")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        block = text[text.index("\t<season>"):text.index("</season>") + len("</season>")]
        twin = re.sub(r"\{[0-9a-f-]+\}", "{11111111-2222-3333-4444-555555555555}", block)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace("</seasons>", twin + "\n</seasons>"))
        r = self.gcj("season", "show", "twin", expect=2)
        self.assertIn("{11111111-2222-3333-4444-555555555555}", r["error"])
        self.assertEqual(self.gcj("season", "show", "{11111111-2222-3333-4444-555555555555}")["data"]["name"], "Twin")
        self.gcj("activity", "list", "--season", "Twin", expect=2)

    def test_phases(self):
        self.gcj("season", "add", "2020", "--from", "2020-01-01", "--to", "2020-06-30")
        env = self.gcj("season", "phase", "add", "2020", "Base", "--type", "base", "--to", "2020-02-29", "--seed", "10")
        self.assertEqual((env["data"]["start"], env["data"]["end"], env["data"]["type"]), ("2020-01-01", "2020-02-29", "base"))
        self.gcj("season", "phase", "add", "2020", "Whole")                   # the season's dates
        for args in (("Late", "--from", "2020-06-01", "--to", "2020-07-31"),  # outside the season
                     ("Short", "--from", "2020-02-01", "--to", "2020-02-01"), # no day long
                     ("Peaky", "--type", "peak"),                             # the dialog doesn't offer it
                     ("base",)):                                              # taken
            self.gcj("season", "phase", "add", "2020", *args, expect=2)

        phases = seasons_xml(self.folder)["2020"].findall("phase")
        self.assertEqual([(p.findtext("name"), p.findtext("type"), p.findtext("startdate"), p.findtext("enddate"))
                          for p in phases],
                         [("Base", "102", "2020-01-01", "2020-02-29"), ("Whole", "100", "2020-01-01", "2020-06-30")])

        listed = [s for s in self.gcj("season", "list")["data"]["seasons"] if s["season"] == "2020"]
        self.assertEqual([(s["kind"], s["name"]) for s in listed], [("phase", "Base"), ("phase", "Whole")])
        env = self.gcj("season", "phase", "edit", "2020", "base", "--name", "Build", "--type", "build", "--from", "2020-02-01")
        self.assertEqual((env["data"]["name"], env["data"]["start"]), ("Build", "2020-02-01"))
        self.gcj("season", "phase", "edit", "2020", "Build", "--to", "2020-12-31", expect=2)

        # the activities and the PMC of a phase
        self.assertEqual(len(self.gcj("activity", "list", "--season", "2020/Whole")["data"]["activities"]), 1)
        self.assertEqual(len(self.gcj("activity", "list", "--season", "2020/Build")["data"]["activities"]), 0)
        days = self.gcj("pmc", "--season", "2020/build")["data"]["days"]
        self.assertEqual((days[0]["date"], days[-1]["date"]), ("2020-02-01", "2020-02-29"))

        # a season with phases can't move with today
        self.gcj("season", "edit", "2020", "--start-ago", "4", expect=2)
        self.gcj("season", "edit", "2020", "--ytd", expect=2)
        self.gcj("season", "phase", "remove", "2020", "Build")
        self.gcj("season", "phase", "remove", "2020", "Build", expect=3)
        self.assertEqual([p.findtext("name") for p in seasons_xml(self.folder)["2020"].findall("phase")], ["Whole"])

        # phases go on seasons with fixed dates
        self.gcj("season", "add", "Rolling", "--start-ago", "3", "--length", "1m")
        self.gcj("season", "phase", "add", "Rolling", "Base", expect=2)

    def test_events(self):
        self.gcj("season", "add", "2026", "--from", "2026-01-01", "--to", "2026-12-31")
        self.gcj("season", "add", "2027", "--from", "2027-01-01", "--to", "2027-12-31")
        env = self.gcj("event", "add", "2026", "Club race", "--date", "2026-05-01", "--priority", "b",
                       "--description", "local <crit> & \"sprint\"")
        race = env["data"]
        self.assertEqual((race["priority"], race["season"]), ("B", "2026"))
        self.assertRegex(race["id"], r"^\{[0-9a-f-]{36}\}$")
        last = self.gcj("event", "add", "2026", "Goal")["data"]                # the season's last day
        self.assertEqual((last["date"], last["priority"]), ("2026-12-31", ""))
        self.gcj("event", "add", "2027", "Club race", "--date", "2027-05-01", "--priority", "C")
        self.gcj("event", "add", "2026", "Out", "--date", "2027-01-05", expect=2)
        self.gcj("event", "add", "2026", "Bad", "--priority", "F", expect=2)

        xml = seasons_xml(self.folder)["2026"].findall("event")
        self.assertEqual([(e.get("date"), e.get("priority"), e.get("id")) for e in xml],
                         [("2026-05-01", "2", race["id"]), ("2026-12-31", "0", last["id"])])
        listed = self.gcj("event", "list")["data"]["events"]
        self.assertEqual([(e["date"], e["name"]) for e in listed],
                         [("2026-05-01", "Club race"), ("2026-12-31", "Goal"), ("2027-05-01", "Club race")])
        self.assertEqual(listed[0]["description"], "local <crit> & \"sprint\"")
        self.assertEqual(len(self.gcj("event", "list", "--season", "2027")["data"]["events"]), 1)
        self.assertEqual(len(self.gcj("event", "list", "--from", "2026-06-01", "--to", "2027-01-01")["data"]["events"]), 1)

        # one name in two seasons: give the id or the season
        self.assertIn(race["id"], self.gcj("event", "edit", "Club race", "--priority", "A", expect=2)["error"])
        env = self.gcj("event", "edit", "Club race", "--season", "2026", "--priority", "A", "--date", "2026-05-02")
        self.assertEqual((env["data"]["priority"], env["data"]["date"]), ("A", "2026-05-02"))
        self.gcj("event", "edit", race["id"], "--date", "2027-05-02", expect=2)   # outside its season
        self.gcj("event", "edit", race["id"], "--name", "Crit")
        self.assertEqual(self.gcj("season", "show", "2026")["data"]["events"][0]["name"], "Crit")
        self.gcj("event", "remove", race["id"])
        self.gcj("event", "remove", race["id"], expect=3)
        self.assertEqual([e["name"] for e in self.gcj("event", "list")["data"]["events"]], ["Goal", "Club race"])

    def test_season_option(self):
        self.gcj("season", "add", "Bike year", "--from", "2020-01-01", "--to", "2020-12-31")
        self.gcj("season", "add", "Run year", "--from", "2024-01-01", "--to", "2024-12-31")
        acts = self.gcj("activity", "list", "--season", "bike year")["data"]["activities"]
        self.assertEqual([a["id"] for a in acts], ["2020_01_26_13_00_38"])
        acts = self.gcj("activity", "list", "--season", "Run year")["data"]["activities"]
        self.assertEqual([a["sport"] for a in acts], ["Run"])
        self.assertEqual(self.gcj("activity", "list", "--season", "Last 7 days")["data"]["activities"], [])
        self.gcj("activity", "list", "--season", "Bike year", "--from", "2020-01-01", expect=2)
        self.gcj("activity", "list", "--season", "No such", expect=3)

        days = self.gcj("pmc", "--season", "Bike year")["data"]["days"]
        # the PMC starts the day before the first activity
        self.assertEqual((days[0]["date"], days[-1]["date"]), ("2020-01-25", "2020-12-31"))
        self.gcj("pmc", "--season", "Bike year", "--to", "2020-02-01", expect=2)
        env = self.gcj("chart", "pmc", "--season", "Bike year", "-o", os.path.join(self.tmp, "season-pmc.png"))
        self.assertEqual((env["data"]["from"], env["data"]["to"]), ("2020-01-25", "2020-12-31"))
        self.gcj("cp", "estimates", "--season", "Bike year")
        self.gcj("cp", "estimates", "--season", "Bike year", "--from", "2020-01-01", expect=2)
        self.gcj("measures", "list", "--season", "Bike year")
        self.assertEqual(self.gcj("metric", "aggregate", "--metric", "workout_time", "--season", "Run year")
                         ["data"]["activities"], 1)

    def test_seed_changes_the_pmc(self):
        def ctl(on):
            days = self.gcj("pmc", "--from", on, "--to", on)["data"]["days"]
            return days[0]["ctl"] if days else None
        self.assertIsNone(ctl("2020-01-01"))       # before the first activity: no PMC yet
        self.gcj("season", "add", "Seeded", "--from", "2020-01-01", "--to", "2020-12-31", "--seed", "50")
        self.assertEqual(ctl("2020-01-01"), 50)
        self.assertGreater(ctl("2020-01-20"), 30)
        self.gcj("season", "edit", "Seeded", "--seed", "0")
        self.assertIsNone(ctl("2020-01-01"))

        # a seeded season after the end of the data doesn't break the PMC
        self.gcj("season", "add", "Far", "--from", "2040-01-01", "--to", "2040-12-31", "--seed", "50")
        self.gcj("season", "edit", "Seeded", "--seed", "50")
        self.assertEqual(ctl("2020-01-01"), 50)


class TestExpectedPMC(Headless):
    """pmc and chart pmc --series planned and expected, with --sport and --filter"""

    imports = [RIDE_POWER, RUN_STRYD]
    bike = "2020_01_26_13_00_38"

    @classmethod
    def plan(cls, offset, stress, linked=None):
        when = datetime.date.today() + datetime.timedelta(days=offset)
        tags = {"Sport": "Bike ", "Workout Code": "Plan "}
        if linked:
            tags["Linked Filename"] = linked + " "
        ride = {"RIDE": {"STARTTIME": when.strftime("%Y/%m/%d") + " 12:00:00 UTC ", "RECINTSECS": 1,
                         "DEVICETYPE": "Manual ", "IDENTIFIER": " ", "TAGS": tags,
                         "OVERRIDES": [{"coggan_tss": {"value": str(stress)}}, {"workout_time": {"value": "3600"}}]}}
        folder = os.path.join(cls.home, cls.athlete, "planned")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, when.strftime("%Y_%m_%d") + "_12_00_00.json"), "w") as f:
            json.dump(ride, f)

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # the plan stream adds 'plan add'; written here as the GUI writes a plan
        cls.plan(5, 100)
        cls.plan(10, 80, linked=cls.bike + ".json")    # already done: not expected again

    def days(self, *args):
        return {d["date"]: d for d in self.gcj("pmc", *args)["data"]["days"]}

    def test_expected_after_today(self):
        actual = self.days("--from", day(-1))
        self.assertEqual(max(actual), day())                       # actual stops today
        expected = self.days("--series", "expected", "--from", day(-1))
        self.assertEqual(max(expected), day(10))                   # expected runs to the last plan
        self.assertEqual(expected[day(5)]["stress"], 100)
        self.assertEqual(expected[day(10)]["stress"], 0)           # the linked plan isn't counted
        self.assertGreater(expected[day(6)]["ctl"], actual[day()]["ctl"])
        self.assertEqual(expected[day()]["ctl"], actual[day()]["ctl"])

        planned = self.days("--series", "planned", "--from", day(1))
        self.assertEqual((planned[day(5)]["stress"], planned[day(10)]["stress"]), (100, 80))
        self.assertEqual(self.days("--from", day(1), "--to", day(10))[day(5)]["stress"], 0)

        # the past is what was done
        past = self.days("--series", "expected", "--from", "2020-01-26", "--to", "2020-01-26")
        self.assertGreater(past["2020-01-26"]["stress"], 0)

    def test_all_series(self):
        env = self.gcj("pmc", "--series", "all", "--from", day(4), "--to", day(6))
        row = env["data"]["days"][1]
        self.assertEqual((row["stress"], row["planned"]["stress"], row["expected"]["stress"]), (0, 100, 100))
        rows = self.csv_rows("pmc", "--series", "all", "--from", day(4), "--to", day(6))
        self.assertEqual(rows[0][:7], ["date", "stress", "ctl", "atl", "tsb", "rr", "planned.stress"])
        self.assertEqual(len(rows), 4)
        self.gcj("chart", "pmc", "--series", "all", expect=2)

    def test_sport_and_filter(self):
        on = ("--from", "2020-01-26", "--to", "2020-01-26")
        self.assertGreater(self.days(*on)["2020-01-26"]["stress"], 0)
        self.assertEqual(self.days("--sport", "Run", *on)["2020-01-26"]["stress"], 0)
        self.assertGreater(self.days("--sport", "bike", *on)["2020-01-26"]["stress"], 0)
        self.assertEqual(self.days("--filter", "isRun", *on)["2020-01-26"]["stress"], 0)
        run = self.days("--filter", "isRun", "--from", "2024-07-09", "--to", "2024-07-09")["2024-07-09"]
        self.assertEqual(run["stress"], self.days("--from", "2024-07-09", "--to", "2024-07-09")["2024-07-09"]["stress"])
        # planned activities pass the same filter
        self.assertEqual(self.days("--series", "planned", "--sport", "Bike", "--from", day(5))[day(5)]["stress"], 100)
        self.assertEqual(self.days("--series", "planned", "--sport", "Run", "--from", day(5), "--to", day(5))[day(5)]["stress"], 0)
        self.gcj("pmc", "--filter", "this is not a filter (", expect=2)

    def test_chart(self):
        out = os.path.join(self.tmp, "expected.png")
        env = self.gcj("chart", "pmc", "--series", "expected", "-o", out)
        self.assertEqual((env["data"]["series"], env["data"]["to"]), ("expected", day(10)))
        self.assertTrue(env["data"]["today"])
        with open(out, "rb") as f:
            self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")
        env = self.gcj("chart", "pmc", "--series", "planned", "--sport", "Bike", "--as", "svg", "-o", out + ".svg")
        self.assertEqual(env["data"]["format"], "svg")
        # an athlete without recent activities still gets the half year up to the end of its data
        env = self.gcj("chart", "pmc", "--sport", "Run", "-o", out)
        self.assertEqual(env["data"]["to"], day())


@unittest.skipIf(os.name == "nt" or running_as_root(), "file permissions don't stop Windows or root")
class TestSeasonsReadOnly(Headless):
    """a seasons.xml that can't be written: the command fails promptly, names it, and changes nothing"""

    def test_read_only_config(self):
        self.gcj("season", "add", "Kept", "--from", "2026-01-01", "--to", "2026-02-01")
        config = os.path.join(self.folder, "config")
        path = os.path.join(config, "seasons.xml")
        with open(path, "rb") as f:
            before = f.read()
        for name in (path, config):
            mode = os.stat(name).st_mode
            os.chmod(name, mode & ~0o222)
            self.addCleanup(os.chmod, name, mode)
        started = time.time()
        for args in (("season", "add", "New", "--from", "2026-01-01", "--to", "2026-02-01"),
                     ("season", "edit", "Kept", "--name", "Renamed"),
                     ("season", "phase", "add", "Kept", "Base"),
                     ("event", "add", "Kept", "Race"),
                     ("season", "remove", "Kept")):
            r = self.gca(*args, timeout=60)
            self.assertEqual(r.code, 5, r)
            self.assertIn(b"seasons.xml", r.err, r)
        self.assertLess(time.time() - started, 60)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), before)


class TestSeasonsRest(Headless):
    """the season routes, with ids in the path"""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.server, cls.port = start_server(cls.env, cls.home, os.path.join(cls.tmp, "server.log"))

    @classmethod
    def tearDownClass(cls):
        stop_server(cls.server)
        super().tearDownClass()

    def call(self, method, path, body=None):
        url = "http://127.0.0.1:%d/v1/athletes/%s%s" % (self.port, self.athlete, path)
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method,
                                     headers={"Content-Type": "application/json"} if data else {})
        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_routes(self):
        status, env = self.call("POST", "/seasons", {"name": "Spring", "from": "2026-03-01", "to": "2026-05-31"})
        self.assertEqual(status, 200, env)
        sid = urllib.parse.quote(env["data"]["id"])
        status, env = self.call("POST", "/seasons/Spring/phases", {"name": "Base", "type": "base", "to": "2026-03-31"})
        self.assertEqual(status, 200, env)
        status, env = self.call("POST", "/seasons/%s/events" % sid, {"name": "Race", "date": "2026-05-01", "priority": "A"})
        self.assertEqual(status, 200, env)
        eid = urllib.parse.quote(env["data"]["id"])
        status, env = self.call("GET", "/seasons/" + sid)
        self.assertEqual((status, env["data"]["phases"][0]["name"], env["data"]["events"][0]["name"]), (200, "Base", "Race"))
        status, env = self.call("PUT", "/events/" + eid, {"priority": "C"})
        self.assertEqual((status, env["data"]["priority"]), (200, "C"))
        status, env = self.call("GET", "/events?season=Spring")
        self.assertEqual(len(env["data"]["events"]), 1)
        status, env = self.call("GET", "/activities?season=Spring")
        self.assertEqual(status, 200, env)
        self.assertEqual(self.call("DELETE", "/seasons/This%20Year")[0], 400)
        self.assertEqual(self.call("DELETE", "/seasons/Spring/phases/Base")[0], 200)
        self.assertEqual(self.call("DELETE", "/events/" + eid)[0], 200)
        self.assertEqual(self.call("DELETE", "/seasons/" + sid)[0], 200)
        self.assertEqual(self.call("GET", "/seasons/Spring")[0], 404)



def ini_values(path):
    """a QSettings ini file as {"section/key": raw value}, General keys without a section"""
    values, section = {}, "General"
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
            elif "=" in line:
                key, value = line.split("=", 1)
                values[key if section == "General" else section + "/" + key] = value
    return values


def ini_date(value):
    """a QDate as QSettings writes it: @Variant(...), a type id and a julian day, Qt 4 data stream"""
    m = re.fullmatch(r"@Variant\((.*)\)", value)
    text, raw, i = m.group(1), bytearray(), 0
    while i < len(text):
        if text[i] == "\\" and text[i + 1] == "x":
            j = i + 2
            while j < len(text) and text[j] in "0123456789abcdefABCDEF":
                j += 1
            raw.append(int(text[i + 2:j], 16))
            i = j
        elif text[i] == "\\":
            raw.append(0 if text[i + 1] == "0" else ord(text[i + 1]))
            i += 2
        else:
            raw.append(ord(text[i]))
            i += 1
    julian = int.from_bytes(raw[4:8], "big")
    return datetime.date.fromordinal(julian - 1721425).isoformat()


class TestAthleteSettings(Headless):
    """athlete set writes what the About and Model tabs write, and recomputes as the GUI"""

    imports = [RIDE_POWER]

    def prefs(self):
        return ini_values(os.path.join(self.folder, "config", "athlete-preferences.ini"))

    def metrics(self):
        return self.gcj("activity", "show", "last")["data"]["metrics"]

    def test_every_property_round_trips(self):
        shown = self.gcj("athlete", "show")["data"]
        for key, value in (("nickname", ""), ("crank_length", 175), ("wheel_size", 2100), ("wbal_tau", 300),
                           ("sts_days", 7), ("lts_days", 42), ("sb_today", False), ("weight", 70),
                           ("weight_today", 70), ("weight_source", "setting")):
            self.assertEqual(shown[key], value, key)

        self.addCleanup(self.gc, "--athlete", self.athlete, "athlete", "set", "--weight", "70", "--height", "175",
                        "--sts-days", "7", "--lts-days", "42", "--sb-today", "false", "--sex", "male")
        env = self.gcj("athlete", "set", "--nickname", "Jojo", "--dob", "1975-05-05", "--sex", "female",
                       "--height", "181.5", "--weight", "68.44", "--crank-length", "172.5", "--wheel-size", "2096",
                       "--wbal-tau", "420", "--sts-days", "10", "--lts-days", "50", "--sb-today", "true")
        data = env["data"]
        self.assertGreaterEqual(data["refreshed"], 1)     # weight changed
        expected = {"nickname": "Jojo", "dob": "1975-05-05", "sex": "female", "height": 181.5, "weight": 68.4,
                    "crank_length": 172.5, "wheel_size": 2096, "wbal_tau": 420, "sts_days": 10, "lts_days": 50,
                    "sb_today": True, "weight_today": 68.4, "weight_source": "setting"}
        shown = self.gcj("athlete", "show")["data"]
        for key, value in expected.items():
            self.assertEqual(data[key], value, key)
            self.assertEqual(shown[key], value, key)

        # the keys, units and types the About and Model pages use
        prefs = self.prefs()
        self.assertEqual(prefs["nickname"], "Jojo")
        self.assertEqual(ini_date(prefs["dob"]), "1975-05-05")
        self.assertEqual(prefs["sex"], "1")
        self.assertAlmostEqual(float(prefs["height"]), 1.815)
        self.assertAlmostEqual(float(prefs["weight"]), 68.4)
        self.assertEqual(prefs["crankLength"], "172.5")
        self.assertEqual(prefs["wheelsize"], "2096")
        self.assertEqual(prefs["wbaltau"], "420")
        self.assertEqual(prefs["STSdays"], "10")
        self.assertEqual(prefs["LTSdays"], "50")
        self.assertEqual(prefs["PMshowSBtoday"], "1")

        # only what is passed changes
        self.gcj("athlete", "set", "--sb-today", "false")
        prefs = self.prefs()
        self.assertEqual(prefs["PMshowSBtoday"], "0")
        self.assertEqual(prefs["STSdays"], "10")
        self.assertClosed()

    def test_weight_changes_per_kg_metrics_without_a_measure(self):
        before = self.metrics()
        self.assertEqual(before["athlete_weight"], 70)
        self.addCleanup(self.gc, "--athlete", self.athlete, "athlete", "set", "--weight", "70")
        env = self.gcj("athlete", "set", "--weight", "80")
        self.assertEqual(env["data"]["refreshed"], 1)
        after = self.metrics()
        self.assertEqual(after["athlete_weight"], 80)
        self.assertAlmostEqual(after["average_wpk"], before["average_wpk"] * 70 / 80, places=3)

        # a Body measure for the day wins over the setting, as Athlete::getWeight
        self.gcj("measures", "add", "--when", "2020-01-01", "--set", "WEIGHTKG=75")
        self.addCleanup(self.gc, "--athlete", self.athlete, "measures", "remove", "--when", "2020-01-01")
        self.assertEqual(self.metrics()["athlete_weight"], 75)
        self.assertEqual(self.gcj("athlete", "set", "--weight", "90")["data"]["refreshed"], 0)
        self.assertEqual(self.metrics()["athlete_weight"], 75)
        shown = self.gcj("athlete", "show")["data"]
        self.assertEqual((shown["weight"], shown["weight_today"], shown["weight_source"]), (90, 75, "measure"))

    def test_wbal_tau_is_train_only(self):
        # activity W' metrics work tau out from the ride, as in the GUI, so
        # the setting recomputes nothing
        before = self.metrics()
        self.addCleanup(self.gc, "--athlete", self.athlete, "athlete", "set", "--wbal-tau", "300")
        env = self.gcj("athlete", "set", "--wbal-tau", "900")
        self.assertEqual(env["data"]["refreshed"], 0)
        self.assertEqual(self.prefs()["wbaltau"], "900")
        after = self.metrics()
        for symbol in ("skiba_wprime_tau", "skiba_wprime_low", "skiba_wprime_exp"):
            self.assertEqual(after[symbol], before[symbol], symbol)

    def test_bad_values_refused(self):
        path = os.path.join(self.folder, "config", "athlete-preferences.ini")
        with open(path, "rb") as f:
            saved = f.read()
        for args in (("--weight", "-1"), ("--weight", "1000"), ("--height", "1000"), ("--crank-length", "171"),
                     ("--wheel-size", "0"), ("--wheel-size", "10000"), ("--wbal-tau", "29"), ("--wbal-tau", "1201"),
                     ("--sts-days", "0"), ("--sts-days", "22"), ("--lts-days", "6"), ("--lts-days", "57"),
                     ("--sb-today", "maybe"), ("--sex", "other"), ("--dob", "1975-13-01"),
                     ("--nickname", "ok", "--weight", "-5"), ("--weight", "nan"), ("--height", "inf"), ()):
            self.gcj("athlete", "set", *args, expect=2)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), saved)


class TestZoneEditing(Headless):
    """zone ranges removed, the power options and the default zones, as the zones pages"""

    imports = [RIDE_POWER]

    def config(self, name):
        return os.path.join(self.folder, "config", name)

    def starts(self, kind, sport="Bike"):
        data = self.gcj("zones", "show", "--sport", sport)["data"]
        if kind == "pace":
            return [r["from"] for r in data["pace"] if r["sport"] == sport]
        return [r["from"] for r in data[kind]["ranges"]]

    def test_range_added_then_removed(self):
        first = self.starts("power")
        with open(self.config("power.zones"), "rb") as f:
            original = f.read()
        before = self.gcj("activity", "show", "last")["data"]["metrics"]["coggan_if"]

        # a range covering the ride changes its metrics, removing it puts them back
        self.gcj("zones", "set", "--from", "2020-01-01", "--cp", "320", "--ftp", "320")
        self.assertNotEqual(self.gcj("activity", "show", "last")["data"]["metrics"]["coggan_if"], before)
        env = self.gcj("zones", "remove", "--from", "2020-01-01")
        self.assertEqual(env["data"]["status"], "removed")
        self.assertEqual(env["data"]["ranges"], first)
        self.assertEqual(env["data"]["refreshed"], 1)
        self.assertEqual(self.starts("power"), first)
        self.assertEqual(self.gcj("activity", "show", "last")["data"]["metrics"]["coggan_if"], before)
        with open(self.config("power.zones"), "rb") as f:
            self.assertEqual(f.read(), original)

        # none that day, the only range, and a sport without zones of its own
        self.gcj("zones", "remove", "--from", "2020-01-02", expect=3)
        self.gcj("zones", "remove", "--from", first[0], expect=2)
        self.gcj("zones", "remove", "--sport", "Run", "--from", first[0], expect=3)
        self.gcj("zones", "remove", "--type", "pace", "--from", first[0], expect=2)

        for kind, args in (("hr", ["--lthr", "170"]), ("pace", ["--sport", "Run", "--cv", "13"])):
            sport = "Run" if kind == "pace" else "Bike"
            first = self.starts(kind, sport)
            self.gcj("zones", "set", "--type", kind, "--from", "2026-01-01", *args)
            self.assertEqual(self.starts(kind, sport), first + ["2026-01-01"])
            self.gcj("zones", "remove", "--type", kind, "--sport", sport, "--from", "2026-01-01")
            self.assertEqual(self.starts(kind, sport), first)
        self.assertClosed()

    def test_power_options(self):
        power = self.gcj("zones", "show")["data"]["power"]
        self.assertEqual((power["cp_model"], power["coggan_metrics"]), ("manual", "cp"))
        self.gcj("zones", "options", expect=2)
        self.gcj("zones", "options", "--cp-model", "cp4", expect=2)
        self.gcj("zones", "options", "--sport", "Nope", "--cp-model", "cp2", expect=3)

        # FTP below CP: the Coggan metrics follow the choice
        self.keep(self.config("power.zones"))
        start = self.starts("power")[0]
        self.gcj("zones", "set", "--from", start, "--ftp", "200")
        self.addCleanup(self.gc, "--athlete", self.athlete, "zones", "options", "--cp-model", "manual",
                        "--coggan-metrics", "cp")
        with_cp = self.gcj("activity", "show", "last")["data"]["metrics"]["coggan_if"]
        env = self.gcj("zones", "options", "--cp-model", "ext", "--coggan-metrics", "ftp")
        self.assertEqual((env["data"]["cp_model"], env["data"]["coggan_metrics"]), ("ext", "ftp"))
        self.assertEqual(env["data"]["refreshed"], 1)
        self.assertAlmostEqual(self.gcj("activity", "show", "last")["data"]["metrics"]["coggan_if"],
                               with_cp * 250 / 200, places=3)
        prefs = ini_values(self.config("athlete-preferences.ini"))
        self.assertEqual((prefs["cp/useModel"], prefs["cp/useforftp"]), ("3", "1"))
        power = self.gcj("zones", "show")["data"]["power"]
        self.assertEqual((power["cp_model"], power["coggan_metrics"]), ("ext", "ftp"))

        # per sport, with the sport in the key as the page saves it
        self.addCleanup(self.gc, "--athlete", self.athlete, "zones", "options", "--sport", "Run", "--cp-model", "manual")
        self.gcj("zones", "options", "--sport", "Run", "--cp-model", "cp2")
        self.assertEqual(ini_values(self.config("athlete-preferences.ini"))["cp/useModelrun"], "1")
        self.assertEqual(self.gcj("zones", "show", "--sport", "Run")["data"]["power"]["cp_model"], "cp2")

    def test_default_zones(self):
        self.keep(self.config("power.zones"))
        self.keep(self.config("hr.zones"))
        shown = self.gcj("zones", "scheme", "show")["data"]
        self.assertEqual(shown["of"], "CP")
        self.assertEqual(shown["zones"][0]["percent"], 0)
        self.assertGreater(len(shown["zones"]), 3)

        # sorted by the lower bound; the ranges using the default follow
        env = self.gcj("zones", "scheme", "set", "--zone", "Z3,Hard,100", "--zone", "Z1,Easy,0", "--zone", "Z2,Steady,60%")
        self.assertEqual([z["name"] for z in env["data"]["zones"]], ["Z1", "Z2", "Z3"])
        self.assertEqual(env["data"]["refreshed"], 1)
        with open(self.config("power.zones")) as f:
            self.assertIn("DEFAULTS:\nZ1,Easy,0%\nZ2,Steady,60%\nZ3,Hard,100%\n", f.read())
        zones = self.gcj("zones", "show")["data"]["power"]["ranges"][0]["zones"]
        self.assertEqual([(z["name"], z["low"]) for z in zones], [("Z1", 0), ("Z2", 150), ("Z3", 250)])
        self.assertEqual(len(self.gcj("activity", "show", "last")["data"]["zones"]["power"]["zones"]), 3)

        env = self.gcj("zones", "scheme", "set", "--type", "hr", "--zone", "Z1,Easy,0,1", "--zone", "Z2,Hard,90,2.5")
        self.assertEqual(env["data"]["zones"][1], {"name": "Z2", "description": "Hard", "percent": 90, "trimp_k": 2.5})

        with open(self.config("power.zones"), "rb") as f:
            saved = f.read()
        for args in (("--zone", "Z1,Easy"), ("--zone", "Z1,Easy,abc"), ("--zone", "Z1,Easy,1001"),
                     ("--zone", ",Easy,0"), ("--zone", "Z1#,Easy,0"), ("--type", "hr", "--zone", "Z1,Easy,0"),
                     ("--type", "hr", "--zone", "Z1,Easy,0,11"), ("--type", "pace", "--zone", "Z1,Easy,0")):
            self.gcj("zones", "scheme", "set", *args, expect=2)
        self.gcj("zones", "scheme", "set", "--sport", "Run", "--zone", "Z1,Easy,0", expect=3)
        with open(self.config("power.zones"), "rb") as f:
            self.assertEqual(f.read(), saved)
        self.assertEqual(self.gcj("zones", "scheme", "show", "--sport", "Run")["data"]["uses"], "Bike")


class TestMeasureEditing(Headless):
    """a reading edited or removed as in the GUI's measures table, and what depends on it recomputed"""

    imports = [RIDE_POWER]     # starts 2020-01-26

    def readings(self):
        return self.gcj("measures", "list", "--group", "Body")["data"]["measures"]

    def weight(self):
        return self.gcj("activity", "show", "last")["data"]["metrics"]["athlete_weight"]

    def test_add_edit_remove(self):
        self.gcj("measures", "add", "--when", "2020-01-20", "--set", "WEIGHTKG=72", "--set", "FATKG=9", "--comment", "scale")
        self.assertEqual(self.weight(), 72)

        env = self.gcj("measures", "edit", "--when", "2020-01-20", "--set", "WEIGHTKG=74")
        self.assertEqual(env["data"]["refreshed"], 1)
        self.assertEqual(self.weight(), 74)
        reading = self.readings()[0]
        self.assertEqual((reading["WEIGHTKG"], reading["FATKG"], reading["comment"]), (74, 9, "scale"))
        self.gcj("measures", "edit", "--when", "2020-01-20T00:00:00", "--comment", "new scale")
        self.assertEqual(self.readings()[0]["comment"], "new scale")
        with open(os.path.join(self.folder, "config", "bodymeasures.json")) as f:
            stored = json.load(f)["measures"]
        self.assertEqual((stored[0]["weightkg"], stored[0]["fatkg"], stored[0]["comment"]), (74, 9, "new scale"))

        # two on a day: the time is needed
        self.gcj("measures", "add", "--when", "2020-01-20T08:00:00", "--set", "WEIGHTKG=73")
        self.gcj("measures", "edit", "--when", "2020-01-20", "--set", "WEIGHTKG=70", expect=2)
        self.gcj("measures", "remove", "--when", "2020-01-20", expect=2)
        self.gcj("measures", "edit", "--when", "2020-01-20T09:00:00", "--set", "WEIGHTKG=70", expect=3)
        self.gcj("measures", "edit", "--when", "2020-01-20T08:00:00", expect=2)
        self.gcj("measures", "edit", "--when", "2020-01-20T08:00:00", "--set", "NOPE=1", expect=2)
        self.gcj("measures", "edit", "--when", "2020-01-20T08:00:00", "--set", "WEIGHTKG=nan", expect=2)
        self.gcj("measures", "add", "--when", "2020-01-21", "--set", "WEIGHTKG=NaN", expect=2)
        self.gcj("measures", "edit", "--when", "2020-01-20T08:00:00", "--set", "WEIGHTKG=10000", expect=2)
        self.gcj("measures", "remove", "--group", "Nope", "--when", "2020-01-20", expect=3)
        self.assertEqual(self.weight(), 73)

        env = self.gcj("measures", "remove", "--when", "2020-01-20T08:00:00")
        self.assertEqual(env["data"]["refreshed"], 1)
        self.assertEqual(self.weight(), 74)
        self.gcj("measures", "remove", "--when", "2020-01-20")
        self.assertEqual(self.readings(), [])
        self.assertEqual(self.weight(), 70)       # the setting again
        self.gcj("measures", "remove", "--when", "2020-01-20", expect=3)
        self.assertClosed()

    def test_source(self):
        path = os.path.join(self.folder, "config", "bodymeasures.json")
        self.keep(path)
        self.gcj("measures", "add", "--when", "2021-03-01T07:00:00", "--set", "WEIGHTKG=70")
        self.assertEqual(self.readings()[-1]["source"], "manual")

        # as if downloaded from Withings
        with open(path) as f:
            content = json.load(f)
        content["measures"][-1]["source"] = 1
        content["measures"][-1]["originalsource"] = "scale"
        with open(path, "w") as f:
            json.dump(content, f)
        self.assertEqual(self.readings()[-1]["source"], "withings")

        # added again at the same time it keeps its source, as the GUI's Add
        self.gcj("measures", "add", "--when", "2021-03-01T07:00:00", "--set", "WEIGHTKG=71")
        self.assertEqual(self.readings()[-1]["source"], "withings")

        # edited, it becomes a manual entry
        self.gcj("measures", "edit", "--when", "2021-03-01", "--set", "WEIGHTKG=72")
        self.assertEqual(self.readings()[-1]["source"], "manual")
        with open(path) as f:
            stored = json.load(f)["measures"][-1]
        self.assertEqual((stored["source"], stored["originalsource"], stored["weightkg"]), (0, "scale", 72))


@unittest.skipIf(os.name == "nt" or running_as_root(), "file permissions don't stop Windows or root")
class TestAthleteSettingsReadOnly(Headless):
    """athlete, zone and measure changes in a config folder that can't be written fail promptly and change nothing"""

    imports = [RIDE_POWER]

    def test_read_only_config(self):
        self.gcj("zones", "set", "--from", "2026-01-01", "--cp", "260")
        self.gcj("measures", "add", "--when", "2026-01-01", "--set", "WEIGHTKG=70")
        config = os.path.join(self.folder, "config")
        before = {f: open(os.path.join(config, f), "rb").read() for f in os.listdir(config)}
        for root, dirs, files in os.walk(config, topdown=False):
            for name in files + dirs:
                full = os.path.join(root, name)
                mode = os.stat(full).st_mode
                os.chmod(full, mode & ~0o222)
                self.addCleanup(os.chmod, full, mode)
        mode = os.stat(config).st_mode
        os.chmod(config, mode & ~0o222)
        self.addCleanup(os.chmod, config, mode)

        for args, mentions in ((("athlete", "set", "--weight", "60"), "athlete-preferences.ini"),
                               (("athlete", "set", "--wbal-tau", "500"), "athlete-preferences.ini"),
                               (("zones", "options", "--coggan-metrics", "ftp"), "athlete-preferences.ini"),
                               (("zones", "remove", "--from", "2026-01-01"), "power.zones"),
                               (("zones", "scheme", "set", "--zone", "Z1,Easy,0"), "power.zones"),
                               (("measures", "edit", "--when", "2026-01-01", "--set", "WEIGHTKG=60"), "bodymeasures.json"),
                               (("measures", "remove", "--when", "2026-01-01"), "bodymeasures.json")):
            started = time.time()
            r = self.gca(*args, timeout=60)
            self.assertEqual(r.code, 5, r)
            self.assertIn(mentions.encode(), r.err + r.out, r)
            self.assertLess(time.time() - started, 30)
        self.assertEqual({f: open(os.path.join(config, f), "rb").read() for f in os.listdir(config)}, before)
        shown = self.gcj("athlete", "show")["data"]
        self.assertEqual((shown["weight"], shown["wbal_tau"]), (70, 300))
        self.assertEqual(self.gcj("zones", "show")["data"]["power"]["coggan_metrics"], "cp")
        self.assertIn("2026-01-01", [r["from"] for r in self.gcj("zones", "show")["data"]["power"]["ranges"]])



class TestAthleteSettingsRest(Headless):
    """the athlete, zone and measure changes over REST"""

    imports = [RIDE_POWER]

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.server, cls.port = start_server(cls.env, cls.home, os.path.join(cls.tmp, "server.log"))

    @classmethod
    def tearDownClass(cls):
        stop_server(cls.server)
        super().tearDownClass()

    def jcall(self, method, path, body=None, expect=200):
        url = "http://127.0.0.1:%d/v1/athletes/%s%s" % (self.port, self.athlete, path)
        data, headers = None, {}
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                status, out = resp.status, resp.read()
        except urllib.error.HTTPError as e:
            status, out = e.code, e.read()
        self.assertEqual(status, expect, out[:500])
        return json.loads(out)

    def test_changes_over_rest(self):
        data = self.jcall("PUT", "", {"weight": 72, "sb-today": "true", "wheel-size": 2105})["data"]
        self.assertEqual((data["weight"], data["sb_today"], data["wheel_size"]), (72, True, 2105))
        self.assertEqual(self.jcall("GET", "")["data"]["weight"], 72)
        self.jcall("PUT", "", {"sts-days": 30}, expect=400)

        data = self.jcall("PUT", "/zones/options", {"coggan-metrics": "ftp"})["data"]
        self.assertEqual(data["coggan_metrics"], "ftp")
        self.assertEqual(self.jcall("GET", "/zones/scheme?type=hr")["data"]["of"], "LT")
        self.jcall("PUT", "/zones/scheme", {"type": "pace", "sport": "Run", "zone": ["Z1,Easy,0", "Z2,Fast,90"]})
        self.assertEqual(len(self.jcall("GET", "/zones/scheme?type=pace&sport=Run")["data"]["zones"]), 2)

        self.jcall("PUT", "/zones", {"type": "power", "from": "2026-01-01", "cp": 280})
        self.assertEqual(self.jcall("DELETE", "/zones?from=2026-01-01")["data"]["status"], "removed")

        self.jcall("POST", "/measures", {"when": "2026-01-01", "set": ["WEIGHTKG=70"]})
        self.jcall("PUT", "/measures", {"when": "2026-01-01", "comment": "rest"})
        self.assertEqual(self.jcall("GET", "/measures?group=Body")["data"]["measures"][0]["comment"], "rest")
        self.jcall("DELETE", "/measures?when=2026-01-01")
        self.jcall("DELETE", "/measures?when=2026-01-01", expect=404)


def png_colours(data, limit=64):
    """how many different pixel values a PNG has (up to limit): a blank image has one"""
    import struct
    import zlib
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width, height, kind = 8, b"", 0, 0, 0
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack(">IIBB", body[:10])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    bpp = {2: 3, 6: 4}[kind]
    stride = width * bpp + 1
    # unfiltered or not, rows of a drawn chart differ; count raw row bytes
    seen = set()
    for y in range(0, height, max(1, height // 50)):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(0, len(row) - bpp, bpp * 7):
            seen.add(row[x:x + bpp])
            if len(seen) >= limit:
                return len(seen)
    return len(seen)


class TestTrendsLibraryCharts(Headless):
    """chart library render and data: the Trends sidebar charts drawn by the GUI's own plot"""

    imports = [RIDE_POWER, RIDE_GPS, RUN_STRYD]

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # the GPS ride into the power ride's week (Monday 20 to Sunday 26 January 2020)
        r = cls.gc_class("--athlete", cls.athlete, "activity", "set", "2012_01_11_11_51_01",
                         "--set", "Start Date=2020-01-20")
        assert r.code == 0, r

    def setUp(self):
        self.keep(os.path.join(self.folder, "config", "charts.xml"))

    def charts(self):
        return [c["name"] for c in self.gcj("chart", "library", "list")["data"]["charts"]]

    def test_every_built_in_chart_draws_in_every_format(self):
        names = self.charts()
        self.assertGreater(len(names), 30)
        server, port = start_server(self.env, self.home, os.path.join(self.tmp, "render.log"))
        try:
            for name in names:
                path = "/athletes/%s/charts/%s/image" % (self.athlete, urllib.parse.quote(name, safe=""))
                for fmt, magic in (("png", b"\x89PNG"), ("svg", b"<?xml"), ("pdf", b"%PDF")):
                    status, body = call_server(port, "GET", path + "?as=" + fmt + "&width=800&height=400")
                    self.assertEqual(status, 200, (name, fmt, body[:300]))
                    self.assertTrue(body.startswith(magic), (name, fmt, body[:20]))
                    self.assertGreater(len(body), 2000, (name, fmt))
                    if fmt == "png":
                        self.assertGreater(png_colours(body), 3, name)
                    if fmt == "svg":
                        self.assertIn(b"<svg", body[:2000])
        finally:
            stop_server(server)
        self.assertClosed()

    def test_render_on_the_command_line(self):
        out = os.path.join(self.tmp, "pmc.png")
        env = self.gcj("-o", out, "chart", "library", "render", "PMC (Coggan)", "--from", "2020-01-01",
                       "--to", "2020-03-31", "--by", "week", "--width", "900", "--height", "500")
        self.assertEqual(env["data"]["name"], "PMC (Coggan)")
        self.assertEqual(env["data"]["by"], "week")
        self.assertEqual((env["data"]["width"], env["data"]["height"]), (900, 500))
        # a weekly chart starts on a Monday
        self.assertEqual(env["data"]["from"], "2019-12-30")
        with open(out, "rb") as f:
            image = f.read()
        self.assertTrue(image.startswith(b"\x89PNG"))
        self.assertGreater(png_colours(image), 3)

        refused = self.gcj("chart", "library", "render", "No such chart", expect=3)
        self.assertIn("no chart called", refused["error"])
        self.gcj("chart", "library", "data", "No such chart", expect=3)
        self.gcj("chart", "library", "render", "PMC (Coggan)", "--from", "2020-02-01", "--to", "2020-01-01", expect=2)
        self.gcj("chart", "library", "render", "PMC (Coggan)", "--filter", "this is not a filter !!!", expect=2)
        self.gcj("chart", "library", "render", "PMC (Coggan)", "--by", "fortnight", expect=2)
        self.assertClosed()

    def test_season_dates(self):
        self.gcj("season", "add", "Winter20", "--from", "2020-01-01", "--to", "2020-03-31")
        by_season = self.gcj("chart", "library", "data", "PMC (Coggan)", "--season", "winter20", "--by", "week")["data"]
        by_dates = self.gcj("chart", "library", "data", "PMC (Coggan)", "--from", "2020-01-01", "--to", "2020-03-31",
                            "--by", "week")["data"]
        self.assertEqual(by_season["rows"], by_dates["rows"])
        self.assertEqual(by_season["from"], "2019-12-30")

        out = os.path.join(self.tmp, "winter.png")
        self.gcj("-o", out, "chart", "library", "render", "PMC (Coggan)", "--season", "Winter20")
        with open(out, "rb") as f:
            self.assertGreater(png_colours(f.read()), 3)

        self.gcj("chart", "library", "render", "PMC (Coggan)", "--season", "Winter20", "--from", "2020-01-01", expect=2)
        self.gcj("chart", "library", "data", "PMC (Coggan)", "--season", "No such season", expect=3)
        self.gcj("season", "remove", "Winter20")
        self.assertClosed()

    def test_data_matches_metric_aggregate_per_week(self):
        self.gcj("chart", "library", "add", "--name", "Weekly", "--by", "week",
                 "--metric", "Average_Speed", "--metric", "total_distance")
        data = self.gcj("chart", "library", "data", "Weekly", "--from", "2020-01-01", "--to", "2020-02-29")["data"]
        self.assertEqual(data["by"], "week")
        self.assertEqual([c["detail"] for c in data["columns"]], ["average_speed", "total_distance"])
        # as the GUI's table: from the first week with data to the last
        self.assertGreater(len(data["rows"]), 0)
        busy = 0
        for row in data["rows"]:
            start = row["date"]
            self.assertRegex(start, r"^\d{4}-\d\d-\d\d$")
            to = (datetime.date.fromisoformat(start) + datetime.timedelta(days=6)).isoformat()
            agg = self.gcj("metric", "aggregate", "--metric", "Average_Speed,total_distance",
                           "--from", start, "--to", to)["data"]
            if agg["activities"] == 0:
                self.assertEqual(row["values"], [0, 0], start)
                continue
            busy += 1
            self.assertEqual(agg["activities"], 2, start)
            # aggregate rounds to the metric's precision, the table has it unrounded and as text
            self.assertAlmostEqual(row["values"][0], agg["values"]["average_speed"], delta=0.051)
            self.assertAlmostEqual(row["values"][1], agg["values"]["total_distance"], delta=0.0051)
            self.assertEqual(float(row["text"][0]), agg["values"]["average_speed"])
        self.assertEqual(busy, 1)

        # text and CSV are the same table, a column per curve
        rows = self.csv_rows("chart", "library", "data", "Weekly", "--from", "2020-01-01", "--to", "2020-02-29")
        self.assertEqual(rows[0][0], "Date")
        self.assertEqual(len(rows), len(data["rows"]) + 1)
        self.assertEqual(len(rows[0]), 3)
        text = self.gca("chart", "library", "data", "Weekly", "--from", "2020-01-20", "--to", "2020-01-26")
        self.assertEqual(text.code, 0, text)
        self.assertIn(b"2020-01-20", text.out)

        # an extra filter, as the filter box: the run is in no week here
        filtered = self.gcj("chart", "library", "data", "Weekly", "--from", "2020-01-01", "--to", "2020-02-29",
                            "--filter", "isRun")["data"]
        self.assertTrue(all(r["values"] == [0, 0] for r in filtered["rows"]), filtered["rows"])
        # REST gives the same rows
        server, port = start_server(self.env, self.home, os.path.join(self.tmp, "data.log"))
        try:
            status, body = call_server(port, "GET", "/athletes/%s/charts/Weekly/data?from=2020-01-01&to=2020-02-29" % self.athlete)
        finally:
            stop_server(server)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)["data"]["rows"], data["rows"])

    def test_pmc_chart_made_here_matches_pmc(self):
        self.gcj("chart", "library", "add", "--name", "My PMC", "--by", "day", "--pmc", "lts")
        self.gcj("chart", "library", "curve", "add", "My PMC", "--pmc", "sts", "--stress", "BikeStress")
        self.gcj("chart", "library", "curve", "add", "My PMC", "--pmc", "sb")
        shown = self.gcj("chart", "library", "curve", "add", "My PMC", "--pmc", "expected-sts")["data"]["metrics"]
        self.assertEqual([m["type"] for m in shown], ["stress"] * 4)
        self.assertEqual([m["pmc"] for m in shown], ["lts", "sts", "sb", "expected-sts"])
        self.assertEqual({m["stress"] for m in shown}, {"coggan_tss"})
        self.assertEqual([m["name"] for m in shown], ["BikeStress"] * 4)
        self.assertEqual([m["units"] for m in shown], ["Stress"] * 4)
        # read back from the file
        again = self.gcj("chart", "library", "show", "My PMC")["data"]["metrics"]
        self.assertEqual([m["detail"] for m in again],
                         ["lts coggan_tss", "sts coggan_tss", "sb coggan_tss", "expected-sts coggan_tss"])

        out = os.path.join(self.tmp, "mypmc.svg")
        self.gcj("-o", out, "chart", "library", "render", "My PMC", "--as", "svg", "--from", "2020-01-01", "--to", "2020-03-31")
        with open(out, "rb") as f:
            self.assertIn(b"<svg", f.read(2000))

        data = self.gcj("chart", "library", "data", "My PMC", "--from", "2020-01-20", "--to", "2020-02-20")["data"]
        self.assertEqual([c["name"] for c in data["columns"]], ["CTL", "ATL", "TSB", "Expected ATL"])
        pmc = {d["date"]: d for d in self.gcj("pmc", "--from", "2020-01-20", "--to", "2020-02-20")["data"]["days"]}
        # grouped by day the GUI's table leaves out days whose values are all below 1
        self.assertGreater(len(data["rows"]), 5)
        for row in data["rows"]:
            day = pmc[row["date"]]
            self.assertAlmostEqual(row["values"][0], day["ctl"], places=3)
            self.assertAlmostEqual(row["values"][1], day["atl"], places=3)
            self.assertAlmostEqual(row["values"][2], day["tsb"], places=3)

        refused = self.gcj("chart", "library", "curve", "add", "My PMC", "--pmc", "lts", "--stress", "nope", expect=2)
        self.assertIn("nope", refused["error"])
        self.gcj("chart", "library", "curve", "add", "My PMC", "--stress", "coggan_tss", "--metric", "average_power", expect=2)
        self.gcj("chart", "library", "curve", "add", "My PMC", "--pmc", "lts", "--metric", "average_power", expect=2)

    def test_measure_curves(self):
        shown = self.gcj("chart", "library", "add", "--name", "Weight", "--by", "day",
                         "--measure", "WEIGHTKG")["data"]["metrics"]
        self.assertEqual((shown[0]["type"], shown[0]["detail"], shown[0]["units"]), ("measure", "Body - Weight", "kg"))
        self.gcj("measures", "add", "--when", "2020-01-22", "--set", "WEIGHTKG=71.5")
        data = self.gcj("chart", "library", "data", "Weight", "--from", "2020-01-20", "--to", "2020-01-24")["data"]
        values = {r["date"]: r["values"][0] for r in data["rows"]}
        self.assertAlmostEqual(values["2020-01-22"], 71.5, places=3)
        self.gcj("chart", "library", "curve", "add", "Weight", "--measure", "nope", expect=2)
        self.gcj("chart", "library", "curve", "add", "Weight", "--measure", "weight", "--group", "nope", expect=2)
        self.gcj("chart", "library", "curve", "add", "Weight", "--metric", "average_power", "--group", "Body", expect=2)

    def test_a_gui_made_banister_chart_draws(self):
        # a chart with a Banister curve, as Curve Settings saves one: the
        # command line can't make it, so a PMC curve is made and its type
        # changed in the file to METRIC_BANISTER (11)
        self.gcj("chart", "library", "add", "--name", "Banister", "--by", "day", "--pmc", "lts")
        path = os.path.join(self.folder, "config", "charts.xml")
        with open(path, encoding="utf-8") as f:
            xml = f.read()
        found = False
        for chart, blob in re.findall(r'<chart name="(.*?)">"(.*?)"</chart>', xml):
            if html.unescape(chart) != "Banister":
                continue
            data = base64.b64decode(blob)
            marker = b"\xff\xff\xff\xff\x00\x00\x00\x16\x00\x00\x00\x01\x00\x00\x00\x07"
            self.assertIn(marker, data)
            data = data.replace(marker, marker[:-1] + b"\x0b")
            xml = xml.replace(blob, base64.b64encode(data).decode())
            found = True
        self.assertTrue(found)
        with open(path, "w", encoding="utf-8") as f:
            f.write(xml)
        self.assertEqual(self.gcj("chart", "library", "show", "Banister")["data"]["metrics"][0]["type"], "banister")

        out = os.path.join(self.tmp, "banister.png")
        self.gcj("-o", out, "chart", "library", "render", "Banister", "--from", "2020-01-01", "--to", "2020-03-31")
        with open(out, "rb") as f:
            self.assertGreater(png_colours(f.read()), 3)
        rows = self.gcj("chart", "library", "data", "Banister", "--from", "2020-01-01", "--to", "2020-03-31",
                        "--by", "week")["data"]["rows"]
        self.assertGreater(len(rows), 0)
        self.assertClosed()


# manual entry and planned activities (activity add, plan ...)


def ride_id(date, time="16:00:00"):
    return date.replace("-", "_") + "_" + time.replace(":", "_")


class PlanHelpers:
    def ride(self, rid, planned=False):
        """the RIDE object of an activity file"""
        path = os.path.join(self.folder, "planned" if planned else "activities", rid + ".json")
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)["RIDE"]

    def tags(self, rid, planned=False):
        return {k: v.strip() for k, v in self.ride(rid, planned).get("TAGS", {}).items()}

    def overrides(self, rid, planned=False):
        return {k: v["value"] for o in self.ride(rid, planned).get("OVERRIDES", []) for k, v in o.items()}

    def planned_files(self):
        folder = os.path.join(self.folder, "planned")
        return sorted(f[:-5] for f in os.listdir(folder) if f.endswith(".json")) if os.path.isdir(folder) else []

    def plan(self, *args, expect=0):
        return self.gcj("plan", *args, expect=expect)["data"]


# what the Manual entry wizard wrote on Xvfb for: 2026-10-02 16:57:58, Bike,
# workout code WC1, notes "hello", 150 bpm, 200 W, 90 rpm, 30.5 km, 1:02:00,
# estimate by duration (nothing to estimate from: the athlete's only ride had
# no distance)
WIZARD_TAGS = {"Device": "Manual", "Notes": "hello", "Sport": "Bike", "Workout Code": "WC1"}
WIZARD_OVERRIDES = {"average_cad": "90", "average_hr": "150", "average_power": "200", "time_riding": "3720",
                    "total_distance": "30.5", "workout_time": "3720"}


def qround(x):
    return int(x + 0.5) if x > 0 else 0


class TestManualEntry(Headless, PlanHelpers):
    """activity add: Activity > Manual entry"""

    imports = [RUN_STRYD]

    def test_same_file_as_the_wizard(self):
        r = self.gcj("activity", "add", "--date", "2026-10-02", "--time", "16:57:58", "--sport", "Bike",
                     "--workout-code", "WC1", "--notes", "hello", "--avg-hr", "150", "--avg-power", "200",
                     "--avg-cadence", "90", "--distance", "30.5", "--duration", "1:02:00", "--estimate", "time")["data"]
        self.assertEqual(r["id"], "2026_10_02_16_57_58")
        ride = self.ride("2026_10_02_16_57_58")
        self.assertEqual(ride["DEVICETYPE"].strip(), "Manual")
        self.assertEqual(ride["RECINTSECS"], 0)
        self.assertEqual(self.tags("2026_10_02_16_57_58"), WIZARD_TAGS)
        self.assertEqual(self.overrides("2026_10_02_16_57_58"), WIZARD_OVERRIDES)
        self.assertNotIn("SAMPLES", ride)
        # it is an activity like any other
        shown = self.gcj("activity", "show", "2026_10_02_16_57_58")["data"]
        self.assertEqual(shown["metrics"]["total_distance"], 30.5)
        self.assertEqual(shown["metadata"]["Workout Code"], "WC1")
        listed = self.gcj("activity", "list", "2026_10_02_16_57_58", "--metric", "average_hr,workout_time")["data"]
        self.assertEqual(listed["activities"][0]["metrics"], {"average_hr": 150, "workout_time": 3720})

    def expected(self, sport, days, by, duration, distance):
        """the wizard's estimate, from the activities as activity list has them"""
        symbols = ["time_riding", "total_distance", "total_work", "coggan_tss", "skiba_bike_score", "swimscore", "triscore"]
        rides = self.gcj("activity", "list", "--sport", sport, "--metric", ",".join(symbols))["data"]["activities"]
        rides = [r for r in rides if r["sport"] == sport and r["metrics"]["time_riding"] and r["metrics"]["total_distance"]]
        today = datetime.date.today()
        recent = [r for r in rides if 0 <= (today - datetime.date.fromisoformat(r["start"][:10])).days < days]

        def total(rs, s):
            return sum(r["metrics"][s] for r in rs)
        names = ["total_work", "coggan_tss", "skiba_bike_score", "swimscore", "triscore"]
        t, d = total(rides, "time_riding"), total(rides, "total_distance")
        by_time = {n: total(rides, n) * 3600 / t for n in names}
        by_dist = {n: total(rides, n) / d for n in names}
        if recent:
            t, d = total(recent, "time_riding"), total(recent, "total_distance")
            if t:
                by_time = {n: total(recent, n) * 3600 / t for n in names}
            if d:
                by_dist = {n: total(recent, n) / d for n in names}
        values = {n: (duration * by_time[n] / 3600 if by == "time" else distance * by_dist[n]) for n in names}
        maxima = {"total_work": 9999}
        return {n: min(qround(v), maxima.get(n, 999)) for n, v in values.items()}

    def check_estimate(self, data, expected):
        o = data["overrides"]
        for name, value in expected.items():
            self.assertEqual(o.get(name, 0), value, (name, o, expected))

    def test_estimate_by_time_and_distance(self):
        # entered by hand: stress given means no estimate
        a = self.gcj("activity", "add", "--date", day(-3), "--time", "07:00", "--sport", "Row", "--duration", "3600",
                     "--distance", "10", "--work", "600", "--bikestress", "50", "--bikescore", "40", "--triscore", "45")["data"]
        self.assertEqual(a["estimate"]["by"], "none")
        self.assertEqual(a["overrides"]["total_work"], 600)
        self.gcj("activity", "add", "--date", day(-2), "--time", "07:00", "--sport", "Row", "--duration", "0:30:00",
                 "--distance", "5", "--work", "300", "--bikestress", "25", "--bikescore", "20", "--triscore", "30")

        expected = self.expected("Row", 30, "time", 5400, 12)
        self.assertEqual(expected["total_work"], 900)
        self.assertEqual(expected["coggan_tss"], 75)
        c = self.gcj("activity", "add", "--date", day(-1), "--time", "07:00", "--sport", "Row", "--duration", "1:30:00",
                     "--distance", "12", "--estimate", "time", "--estimate-days", "30")["data"]
        self.assertEqual(c["estimate"], {"by": "time", "days": 30})
        self.check_estimate(c, expected)

        expected = self.expected("Row", 30, "distance", 3600, 9)
        d = self.gcj("activity", "add", "--date", day(-1), "--time", "18:00", "--sport", "Row", "--duration", "1:00:00",
                     "--distance", "9", "--estimate", "distance")["data"]
        self.check_estimate(d, expected)
        self.assertGreater(d["overrides"]["total_work"], 0)

    def test_estimate_falls_back_to_all_activities(self):
        # the only run is from 2024: none in the last 30 days, so all of them count
        expected = self.expected("Run", 30, "time", 2700, 8)
        self.assertGreater(expected["triscore"] + expected["total_work"], 0)
        r = self.gcj("activity", "add", "--date", day(-1), "--time", "06:00", "--sport", "Run", "--duration", "0:45:00",
                     "--distance", "8", "--estimate", "time")["data"]
        self.check_estimate(r, expected)

    def test_refusals(self):
        before = self.activity_files()
        for args in (("--date", day(1), "--sport", "Bike"),                       # in the future
                     ("--date", "1999-12-31", "--sport", "Bike"),
                     ("--date", day(-1)),                                          # no sport
                     ("--date", day(-1), "--sport", "Bike", "--duration", "1:5"),
                     ("--date", day(-1), "--sport", "Bike", "--time", "25:00"),
                     ("--date", day(-1), "--sport", "Bike", "--rpe", "11"),
                     ("--date", day(-1), "--sport", "Bike", "--avg-hr", "300"),
                     ("--date", day(-1), "--sport", "Bike", "--distance", "-1"),
                     ("--date", day(-1), "--sport", "Bike", "--distance", "nan"),
                     ("--date", day(-1), "--sport", "Bike", "--distance", "inf"),
                     ("--date", day(-1), "--sport", "Bike", "--estimate", "time", "--bikestress", "50"),
                     ("--date", day(-1), "--sport", "Bike", "--estimate", "sometimes"),
                     ("--date", day(-1), "--sport", "Bike", "--estimate-days", "0")):
            self.gcj("activity", "add", *args, expect=2)
        # objective and workouts are for plans
        self.assertEqual(self.gca("activity", "add", "--date", day(-1), "--sport", "Bike", "--objective", "x").code, 2)
        # a start that is taken
        self.gcj("activity", "add", "--date", day(-5), "--time", "05:00", "--sport", "Bike")
        r = self.gcj("activity", "add", "--date", day(-5), "--time", "05:00", "--sport", "Run", expect=5)
        self.assertIn("already starts", r["error"])
        self.assertEqual(len(self.activity_files()), len(before) + 1)
        self.assertClosed()


class TestPlannedActivities(Headless, PlanHelpers):
    """plan add, list, move, copy, link, shift, repeat, export and import"""

    imports = [RIDE_POWER]

    def test_add_and_list(self):
        d1, d2 = day(30), day(32)
        a = self.plan("add", "--date", d1, "--sport", "Run", "--duration", "0:45:00", "--distance", "9",
                      "--title", "Easy run", "--workout-code", "E1", "--objective", "aerobic", "--notes", "flat")
        self.assertTrue(a["planned"])
        self.assertEqual(a["id"], ride_id(d1))                        # 16:00, as the wizard
        tags = self.tags(ride_id(d1), planned=True)
        self.assertEqual(tags["Original Date"], d1.replace("-", "/"))
        self.assertEqual((tags["Sport"], tags["Route"], tags["Workout Code"], tags["Objective"], tags["Notes"]),
                         ("Run", "Easy run", "E1", "aerobic", "flat"))
        self.assertEqual(self.overrides(ride_id(d1), planned=True),
                         {"total_distance": "9", "workout_time": "2700", "time_riding": "2700"})
        self.plan("add", "--date", d2, "--time", "07:30", "--sport", "Bike", "--duration", "5400", "--bikestress", "80")
        self.assertFalse(os.path.exists(os.path.join(self.folder, "activities", ride_id(d1) + ".json")))

        listed = {p["id"]: p for p in self.plan("list")["planned"]}
        run = listed[ride_id(d1)]
        self.assertEqual((run["date"], run["time"], run["sport"], run["title"], run["workout_code"], run["duration"],
                          run["distance"], run["linked"], run["original_date"]),
                         (d1, "16:00:00", "Run", "Easy run", "E1", 2700, 9, None, None))
        self.assertEqual(listed[ride_id(d2, "07:30:00")]["coggan_tss"], 80)
        only = self.plan("list", "--from", d2, "--to", d2)["planned"]
        self.assertEqual([p["id"] for p in only], [ride_id(d2, "07:30:00")])
        rows = self.csv_rows("plan", "list", "--to", d2)
        self.assertIn("workout_code", rows[0])
        self.assertIn(ride_id(d1), [r[rows[0].index("id")] for r in rows[1:]])
        text = self.gca("plan", "list", "--to", d2).out.decode()
        self.assertIn("Easy run (E1)", text)
        # planned activities are not activities
        ids = [a["id"] for a in self.gcj("activity", "list")["data"]["activities"]]
        self.assertNotIn(ride_id(d1), ids)

    def test_move_copy_keep_original_date(self):
        d, later, past = day(40), day(43), "2026-01-15"
        self.plan("add", "--date", d, "--sport", "Bike", "--duration", "3600", "--title", "Move me")
        moved = self.plan("move", ride_id(d), "--to", later)
        self.assertEqual((moved["id"], moved["moved_from"], moved["original_date"]), (ride_id(later), ride_id(d), d))
        self.assertEqual(self.tags(ride_id(later), planned=True)["Original Date"], d.replace("-", "/"))
        self.assertNotIn(ride_id(d), self.planned_files())
        # moving on keeps the first day planned for, and a move can go back in time
        self.plan("move", ride_id(later), "--to", past, "--time", "09:15")
        self.assertEqual(self.tags(ride_id(past, "09:15:00"), planned=True)["Original Date"], d.replace("-", "/"))
        self.assertNotIn(ride_id(past, "09:15:00"), [p["id"] for p in self.plan("list")["planned"]])
        self.assertIn(ride_id(past, "09:15:00"), [p["id"] for p in self.plan("list", "--all")["planned"]])

        copy = self.plan("copy", ride_id(past, "09:15:00"), "--to", later)
        self.assertEqual((copy["id"], copy["copied_from"]), (ride_id(later, "09:15:00"), ride_id(past, "09:15:00")))
        tags = self.tags(ride_id(later, "09:15:00"), planned=True)
        self.assertEqual((tags["Route"], tags["Original Date"]), ("Move me", later.replace("-", "/")))
        # at another time
        other = self.plan("copy", ride_id(past, "09:15:00"), "--to", later, "--time", "18:00")
        self.assertEqual(other["id"], ride_id(later, "18:00:00"))
        self.assertEqual(self.gcj("activity", "show", "--planned", ride_id(later, "18:00:00"))["data"]["start"],
                         later + "T18:00:00")
        # taken
        r = self.gcj("plan", "copy", ride_id(past, "09:15:00"), "--to", later, expect=5)
        self.assertIn("already exists", r["error"])
        self.gcj("plan", "move", ride_id(later, "18:00:00"), "--to", later, "--time", "09:15", expect=5)
        self.gcj("plan", "move", "nonesuch", "--to", later, expect=3)

    def test_link_unlink(self):
        d = day(50)
        self.plan("add", "--date", d, "--sport", "Bike", "--duration", "1800")
        planned, actual = ride_id(d), "2020_01_26_13_00_38"
        linked = self.plan("link", planned, actual)
        self.assertEqual((linked["planned"], linked["actual"]), (planned, actual))
        self.assertEqual(self.tags(planned, planned=True)["Linked Filename"], actual + ".json")
        self.assertEqual(self.tags(actual)["Linked Filename"], planned + ".json")
        self.assertEqual([p["linked"] for p in self.plan("list", "--from", d, "--to", d)["planned"]], [actual])
        # the GUI's refusals
        self.plan("add", "--date", d, "--time", "08:00", "--sport", "Bike")
        r = self.gcj("plan", "link", ride_id(d, "08:00:00"), actual, expect=5)
        self.assertIn("already linked", r["error"])
        r = self.gcj("plan", "link", ride_id(d, "08:00:00"), planned, expect=5)
        self.assertIn("same type", r["error"])
        self.gcj("plan", "link", actual, actual, expect=3)            # not a planned activity
        # either side unlinks both
        self.plan("unlink", actual)
        self.assertNotIn("Linked Filename", self.tags(planned, planned=True))
        self.assertNotIn("Linked Filename", self.tags(actual))
        r = self.gcj("plan", "unlink", planned, expect=5)
        self.assertIn("not linked", r["error"])

    def test_shift(self):
        # far beyond the other tests' plans: it moves every planned activity from --from on
        for i, n in enumerate((900, 901, 904)):
            self.plan("add", "--date", day(n), "--sport", "Bike", "--title", "S%d" % i)
        shifted = self.plan("shift", "--from", day(901), "--days", "2")
        self.assertEqual((shifted["count"], shifted["shifted_by"]), (2, 2))
        self.assertEqual(sorted(m["to"] for m in shifted["moved"]), [ride_id(day(903)), ride_id(day(906))])
        self.assertIn(ride_id(day(900)), self.planned_files())
        self.assertEqual(self.tags(ride_id(day(903)), planned=True)["Original Date"], day(901).replace("-", "/"))
        # back, as deleting rest days: never to before --from
        shifted = self.plan("shift", "--from", day(902), "--days", "-5")
        self.assertEqual(shifted["shifted_by"], -1)
        self.assertEqual([p["title"] for p in self.plan("list", "--from", day(900))["planned"]], ["S0", "S1", "S2"])
        self.assertIn(ride_id(day(902)), self.planned_files())
        self.assertIn(ride_id(day(905)), self.planned_files())
        self.gcj("plan", "shift", "--from", day(900), "--days", "0", expect=2)

    def test_repeat(self):
        src = [day(70), day(70), day(72)]
        self.plan("add", "--date", src[0], "--sport", "Bike", "--title", "R0", "--time", "06:00")
        self.plan("add", "--date", src[1], "--sport", "Run", "--title", "R1", "--time", "18:00")
        self.plan("add", "--date", src[2], "--sport", "Bike", "--title", "R2", "--time", "06:00")
        start = day(77)
        r = self.plan("repeat", "--from", day(70), "--to", day(76), "--start", start)
        self.assertEqual((r["from"], r["to"], r["count"]), (start, day(83), 3))
        made = sorted(c["id"] for c in r["copies"])
        self.assertEqual(made, [ride_id(day(77), "06:00:00"), ride_id(day(77), "18:00:00"), ride_id(day(79), "06:00:00")])
        for rid in made:
            self.assertIn(rid, self.planned_files())
        tags = self.tags(ride_id(day(79), "06:00:00"), planned=True)
        self.assertEqual((tags["Route"], tags["Original Date"]), ("R2", day(79).replace("-", "/")))
        # again: what is in the way and not linked is replaced
        r = self.plan("repeat", "--from", day(70), "--to", day(76), "--start", start)
        self.assertEqual(sorted(r["deleted"]), made)
        self.assertEqual(r["count"], 3)
        # without the gaps, from the first activity on
        r = self.plan("repeat", "--from", day(69), "--to", day(76), "--start", day(90), "--no-gaps")
        self.assertEqual((r["to"]), day(92))
        self.gcj("plan", "repeat", "--from", day(70), "--to", day(76), "--start", day(76), expect=2)
        self.gcj("plan", "repeat", "--from", day(200), "--to", day(201), "--start", day(210), expect=5)

    def test_repeat_refuses_before_deleting(self):
        # every copy left out (a linked planned activity starts then): nothing is deleted
        self.gcj("activity", "add", "--date", day(-20), "--time", "06:00", "--sport", "Bike")
        self.plan("add", "--date", day(300), "--time", "06:00", "--sport", "Bike", "--title", "Source")
        self.plan("add", "--date", day(310), "--time", "06:00", "--sport", "Bike", "--title", "Linked")
        self.plan("link", ride_id(day(310), "06:00:00"), ride_id(day(-20), "06:00:00"))
        self.plan("add", "--date", day(310), "--time", "09:00", "--sport", "Bike", "--title", "In the way")
        r = self.gcj("plan", "repeat", "--from", day(300), "--to", day(300), "--start", day(310), expect=5)
        self.assertIn("nothing to copy", r["error"])
        self.assertIn(ride_id(day(310), "09:00:00"), self.planned_files())

        # a source moved into the target period would be deleted to make room for its copy
        self.plan("add", "--date", day(320), "--time", "06:00", "--sport", "Bike", "--title", "Moved on")
        self.plan("move", ride_id(day(320), "06:00:00"), "--to", day(331))
        r = self.gcj("plan", "repeat", "--from", day(320), "--to", day(325), "--start", day(330), expect=5)
        self.assertIn("would be deleted", r["error"])
        self.assertIn(ride_id(day(331), "06:00:00"), self.planned_files())
        self.assertNotIn(ride_id(day(330), "06:00:00"), self.planned_files())
        # by the current dates it isn't a source
        self.gcj("plan", "repeat", "--from", day(320), "--to", day(325), "--start", day(330), "--current", expect=5)

    def test_import_refuses_a_linked_conflict(self):
        # the bundle's dates are compared once shifted to the target period
        self.gcj("activity", "add", "--date", day(-21), "--time", "06:00", "--sport", "Bike")
        self.plan("add", "--date", day(400), "--time", "06:00", "--sport", "Bike", "--title", "Bundled")
        bundle = os.path.join(self.tmp, "linked.gcplan")
        self.gcj("plan", "export", "--from", day(400), "--to", day(400), "--name", "Linked", "-o", bundle)
        self.plan("add", "--date", day(410), "--time", "06:00", "--sport", "Bike", "--title", "Linked")
        self.plan("link", ride_id(day(410), "06:00:00"), ride_id(day(-21), "06:00:00"))
        self.plan("add", "--date", day(410), "--time", "09:00", "--sport", "Bike", "--title", "Unlinked")
        r = self.gcj("plan", "import", bundle, "--start", day(410), "--no-gap-days", expect=5)
        self.assertIn("linked planned activity", r["error"])
        self.assertIn(ride_id(day(410), "09:00:00"), self.planned_files())
        self.assertEqual(self.tags(ride_id(day(410), "06:00:00"), planned=True)["Route"], "Linked")

    def test_export_import(self):
        self.plan("add", "--date", day(100), "--sport", "Bike", "--title", "E0", "--duration", "3600", "--avg-power", "200")
        self.plan("add", "--date", day(102), "--sport", "Run", "--title", "E1", "--duration", "1800")
        bundle = os.path.join(self.tmp, "week.gcplan")
        r = self.gcj("plan", "export", "--from", day(99), "--to", day(105), "--name", "Test week",
                     "--description", "$NAME by $AUTHOR", "-o", bundle)["data"]
        self.assertEqual((r["name"], r["author"], r["sport"]), ("Test week", self.athlete, "Bike, Run"))
        with zipfile.ZipFile(bundle) as z:
            names = z.namelist()
            manifest = json.loads(z.read("manifest.json"))
            readme = z.read("README.md").decode()
        self.assertEqual(sorted(n for n in names if n.startswith("planned/") and n.endswith(".json")),
                         ["planned/" + ride_id(day(100)) + ".json", "planned/" + ride_id(day(102)) + ".json"])
        self.assertEqual((manifest["name"], manifest["durationDays"], manifest["frontGapDays"], manifest["backGapDays"]),
                         ("Test week", 7, 1, 3))
        self.assertIn("Test week by " + self.athlete, readme)

        r = self.gcj("plan", "import", bundle, "--start", day(120))["data"]
        # the front gap is kept: the first activity is a day after the start
        self.assertEqual(sorted(r["imported"]), [ride_id(day(121)), ride_id(day(123))])
        tags = self.tags(ride_id(day(121)), planned=True)
        self.assertEqual((tags["Route"], tags["Plan"]), ("E0", "Test week"))
        self.assertEqual(self.overrides(ride_id(day(121)), planned=True)["average_power"], "200")
        r = self.gcj("plan", "import", bundle, "--start", day(130), "--no-gap-days")["data"]
        self.assertEqual(sorted(r["imported"]), [ride_id(day(130)), ride_id(day(132))])
        # importing again replaces the unlinked planned activities of the period
        r = self.gcj("plan", "import", bundle, "--start", day(130), "--no-gap-days")["data"]
        self.assertEqual(sorted(r["replaced"]), [ride_id(day(130)), ride_id(day(132))])
        self.assertEqual(sorted(r["imported"]), [ride_id(day(130)), ride_id(day(132))])

        broken = os.path.join(self.tmp, "broken.gcplan")
        with open(broken, "wb") as f:
            f.write(b"not a zip")
        self.gcj("plan", "import", broken, "--start", day(140), expect=5)
        self.gcj("plan", "export", "--from", day(300), "--to", day(301), "--name", "Empty", "-o", bundle, expect=5)
        self.gcj("plan", "export", "--from", day(99), "--to", day(105), "--name", " ", "-o", bundle, expect=2)

    def test_workout_from_the_library(self):
        # a plan with a workout, imported, puts the workout in the library
        erg = os.path.join(TESTDATA, "workouts", "FTPCheckup.erg")
        self.plan("add", "--date", day(150), "--sport", "Bike", "--duration", "3600")
        self.gcj("activity", "set", "--planned", ride_id(day(150)), "--set", "WorkoutFilename=" + erg, "--allow-undefined")
        bundle = os.path.join(self.tmp, "ftp.gcplan")
        self.gcj("plan", "export", "--from", day(150), "--to", day(150), "--name", "FTP", "-o", bundle)
        with zipfile.ZipFile(bundle) as z:
            self.assertTrue(any(n.startswith("workouts/") and n.endswith("FTPCheckup.erg") for n in z.namelist()))
        self.gcj("plan", "import", bundle, "--start", day(155))
        self.assertTrue(os.path.exists(self.tags(ride_id(day(155)), planned=True)["WorkoutFilename"]))

        p = self.plan("add", "--date", day(160), "--workout", "FTPCheckup", "--avg-hr", "140")
        tags = self.tags(p["id"], planned=True)
        self.assertEqual((tags["Sport"], tags["Route"], tags["Notes"]), ("Bike", "FTPCheckup", "FTP Checkup"))
        o = self.overrides(p["id"], planned=True)
        self.assertEqual((o["workout_time"], o["average_hr"]), ("3600", "140"))
        for name in ("average_power", "coggan_np", "coggan_tss", "skiba_bike_score", "skiba_xpower"):
            self.assertGreater(int(o[name]), 0, name)
        self.assertEqual(p["estimate"]["by"], "none")              # erg workouts are not estimated
        # what the workout decides can't be given
        self.gcj("plan", "add", "--date", day(161), "--workout", "FTPCheckup", "--duration", "1:00:00", expect=2)
        self.gcj("plan", "add", "--date", day(161), "--workout", "FTPCheckup", "--sport", "Run", expect=2)
        self.gcj("plan", "add", "--date", day(161), "--workout", "No such workout", expect=3)

    def test_planned_activities_as_activities(self):
        d = day(170)
        self.plan("add", "--date", d, "--sport", "Bike", "--duration", "3600")
        rid = ride_id(d)
        r = self.gcj("activity", "set", "--planned", d, "--set", "Notes=edited", "--set", "Average Power=210")["data"]
        self.assertEqual(r["updated"], 1)
        self.assertEqual(self.tags(rid, planned=True)["Notes"], "edited")
        self.assertEqual(self.overrides(rid, planned=True)["average_power"], "210")
        shown = self.gcj("activity", "show", "--planned", d)["data"]
        self.assertEqual((shown["id"], shown["planned"], shown["metrics"]["average_power"]), (rid, True, 210))
        self.gcj("activity", "show", d, expect=3)                      # without --planned: completed ones
        out = os.path.join(self.tmp, "planned.json")
        self.gcj("activity", "export", "--planned", rid, "-o", out)
        self.assertTrue(os.path.getsize(out) > 0)
        deleted = self.gcj("activity", "delete", "--planned", rid)["data"]
        self.assertEqual(deleted["deleted"], [rid])
        self.assertNotIn(rid, self.planned_files())
        self.assertTrue(os.path.exists(os.path.join(self.folder, "bak", rid + ".json.bak")))

    def test_same_name_planned_and_completed(self):
        # the ride cache adds and deletes by file name: neither kind may take the other's
        when = day(-1)
        self.gcj("activity", "add", "--date", when, "--time", "05:30", "--sport", "Run")
        rid = ride_id(when, "05:30:00")
        self.gcj("plan", "add", "--date", day(0), "--time", "23:59", "--sport", "Run")
        pid = ride_id(day(0), "23:59:00")
        r = self.gcj("plan", "move", pid, "--to", when, "--time", "05:30", expect=5)
        self.assertIn("already called", r["error"])
        self.gcj("plan", "copy", pid, "--to", when, "--time", "05:30", expect=5)
        r = self.gcj("activity", "add", "--date", day(0), "--time", "23:59", "--sport", "Run", expect=5)
        self.assertIn("a planned activity is already called", r["error"])
        self.gcj("activity", "add", "--date", day(0), "--time", "00:00:01", "--sport", "Run")
        r = self.gcj("plan", "add", "--date", day(0), "--time", "00:00:01", "--sport", "Run", expect=5)
        self.assertIn("a completed activity is already called", r["error"])
        self.assertIn(pid, self.planned_files())
        self.assertNotIn(rid, self.planned_files())
        self.assertNotIn(ride_id(day(0), "00:00:01"), self.planned_files())

        # such a pair made outside: nothing that would delete the planned one by name
        shutil.copy(os.path.join(self.folder, "activities", rid + ".json"), os.path.join(self.folder, "planned", rid + ".json"))
        self.assertFalse(self.gcj("activity", "show", rid)["data"].get("planned", False))
        self.assertTrue(self.gcj("activity", "show", "--planned", rid)["data"]["planned"])
        self.plan("move", pid, "--to", day(-8), "--time", "07:00")
        r = self.gcj("plan", "repeat", "--from", day(-8), "--to", day(-8), "--start", when, "--current", expect=5)
        self.assertIn("can't tell them apart", r["error"])
        bundle = os.path.join(self.tmp, "pair.gcplan")
        self.gcj("plan", "export", "--from", day(-8), "--to", day(-8), "--name", "Pair", "--current", "-o", bundle)
        r = self.gcj("plan", "import", bundle, "--start", when, "--no-gap-days", expect=5)
        self.assertIn("can't tell them apart", r["error"])
        self.assertTrue(os.path.exists(os.path.join(self.folder, "activities", rid + ".json")))
        self.assertIn(rid, self.planned_files())
        self.plan("link", rid, rid)
        r = self.gcj("activity", "delete", "--planned", rid, expect=5)
        self.assertIn("can't tell them apart", r["error"])

    def test_import_links_to_the_plan(self):
        self.plan("add", "--date", day(1), "--sport", "Bike", "--title", "Planned ride")
        self.plan("move", ride_id(day(1)), "--to", "2012-01-11", "--time", "10:00")
        r = self.gcj("import", RIDE_GPS)["data"]
        self.assertEqual(r["imported"], 1)
        self.assertEqual(self.tags("2012_01_11_11_51_01")["Linked Filename"], "2012_01_11_10_00_00.json")
        listed = self.plan("list", "--from", "2012-01-11", "--to", "2012-01-11")["planned"]
        self.assertEqual([p["linked"] for p in listed], ["2012_01_11_11_51_01"])

    def test_calendar_summary(self):
        start = day(180)
        self.plan("add", "--date", start, "--sport", "Bike", "--duration", "3600", "--distance", "30", "--bikestress", "60")
        self.plan("add", "--date", day(182), "--sport", "Bike", "--duration", "1800", "--distance", "15", "--bikestress", "30")
        s = self.gcj("calendar", "summary", "--from", start, "--to", day(193))["data"]
        weeks = s["summaries"]
        self.assertEqual([w["from"] for w in weeks], [start, day(187)])
        self.assertEqual(weeks[0]["metrics"], {"ride_count": 2, "total_distance": 45, "coggan_tss": 90, "workout_time": 5400})
        self.assertEqual(weeks[1]["metrics"]["ride_count"], 0)
        never = self.gcj("calendar", "summary", "--from", start, "--to", day(186), "--planned", "never")["data"]
        self.assertEqual(never["summaries"][0]["metrics"]["ride_count"], 0)
        days = self.gcj("calendar", "summary", "--from", start, "--to", day(182), "--days", "1", "--metric", "coggan_tss")["data"]
        self.assertEqual([d["metrics"]["coggan_tss"] for d in days["summaries"]], [60, 0, 30])
        self.gcj("calendar", "summary", "--from", start, "--to", day(179), expect=2)
        # the last period ends with --to
        self.plan("add", "--date", day(191), "--sport", "Bike", "--duration", "1800", "--bikestress", "40")
        short = self.gcj("calendar", "summary", "--from", start, "--to", day(190))["data"]["summaries"]
        self.assertEqual([(w["from"], w["to"]) for w in short], [(start, day(186)), (day(187), day(190))])
        self.assertEqual(short[1]["metrics"]["ride_count"], 0)

    def test_refusals(self):
        before = self.planned_files()
        for args in (("--date", day(-1), "--sport", "Bike"),            # plans start today or later
                     ("--date", day(5)),                                 # no sport
                     ("--date", day(5), "--sport", "Bike", "--duration", "an hour")):
            self.gcj("plan", "add", *args, expect=2)
        self.assertEqual(self.gca("plan", "add", "--date", day(5), "--sport", "Bike", "--rpe", "5").code, 2)  # for activities
        self.assertEqual(self.planned_files(), before)
        self.gcj("plan", "list", "--metric", "nonesuch", expect=2)
        self.assertClosed()



class TestPlanAdherence(Headless, PlanHelpers):
    """plan adherence: the Plan Adherence chart's entries and totals"""

    imports = [RIDE_POWER]

    def test_adherence(self):
        self.plan("add", "--date", day(5), "--sport", "Bike", "--title", "A")
        # copies are plans of their own day, so they can be in the past
        self.plan("copy", ride_id(day(5)), "--to", "2020-01-26", "--time", "13:00")
        self.plan("link", "2020_01_26_13_00_00", "2020_01_26_13_00_38")
        self.plan("copy", ride_id(day(5)), "--to", day(-10))
        self.plan("add", "--date", day(3), "--sport", "Bike", "--title", "M")
        self.plan("move", ride_id(day(3)), "--to", day(4))
        self.gcj("activity", "add", "--date", day(-2), "--time", "06:00", "--sport", "Run")

        a = self.plan("adherence", "--from", "2020-01-01", "--to", day(10))
        rows = {(e["planned"], e["actual"]): e for e in a["entries"]}
        done = rows[("2020_01_26_13_00_00", "2020_01_26_13_00_38")]
        self.assertEqual((done["status"], done["done_after"], done["date"]), ("on time", 0, "2020-01-26"))
        self.assertEqual(rows[(ride_id(day(-10)), None)]["status"], "missed")
        moved = rows[(ride_id(day(4)), None)]
        self.assertEqual((moved["status"], moved["moved_by"], moved["date"], moved["title"]), ("upcoming", 1, day(3), "M"))
        self.assertEqual(rows[(ride_id(day(5)), None)]["status"], "upcoming")
        self.assertEqual(rows[(None, ride_id(day(-2), "06:00:00"))]["status"], "unplanned")
        self.assertEqual([e["date"] for e in a["entries"]], sorted(e["date"] for e in a["entries"]))
        t = a["totals"]
        self.assertEqual((t["total"], t["planned"], t["on_time"], t["moved"], t["missed"], t["unplanned"]), (5, 4, 1, 1, 1, 1))
        self.assertEqual((t["on_time_percent"], t["unplanned_percent"], t["average_move_days"]), (25, 20, 1))
        # by the day planned for: the moved one is in its old day's range
        a = self.plan("adherence", "--from", day(3), "--to", day(3))
        self.assertEqual([e["planned"] for e in a["entries"]], [ride_id(day(4))])
        self.gcj("plan", "adherence", "--from", day(3), "--to", day(2), expect=2)


@unittest.skipIf(os.name == "nt" or running_as_root(), "file permissions don't stop Windows or root")
class TestPlanReadOnly(Headless, PlanHelpers):
    """activity add and plan add in folders that can't be written"""

    def read_only(self, path):
        mode = os.stat(path).st_mode
        os.chmod(path, mode & ~0o222)
        self.addCleanup(os.chmod, path, mode)

    def test_read_only(self):
        activities = os.path.join(self.folder, "activities")
        planned = os.path.join(self.folder, "planned")
        self.read_only(activities)
        self.read_only(planned)
        r = self.gca("activity", "add", "--date", day(-1), "--time", "10:00", "--sport", "Bike", timeout=60)
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"activities", r.err)
        r = self.gca("plan", "add", "--date", day(3), "--sport", "Bike", timeout=60)
        self.assertEqual(r.code, 5, r)
        self.assertIn(b"planned", r.err)
        self.assertEqual(os.listdir(activities), [])
        self.assertEqual(os.listdir(planned), [])
        self.assertClosed()

    def read_only_tree(self, path):
        for name in os.listdir(path):
            self.read_only(os.path.join(path, name))
        self.read_only(path)

    def test_save_failures_after_calendar_actions(self):
        # the planned side can be written, the completed one can't
        self.gcj("activity", "add", "--date", day(-2), "--time", "10:00", "--sport", "Bike")
        self.gcj("activity", "add", "--date", day(-3), "--time", "10:00", "--sport", "Bike")
        actual, other = ride_id(day(-2), "10:00:00"), ride_id(day(-3), "10:00:00")
        self.plan("add", "--date", day(5), "--sport", "Bike")
        self.plan("add", "--date", day(6), "--sport", "Bike")
        planned, linked = ride_id(day(5)), ride_id(day(6))
        self.plan("link", linked, other)
        self.read_only_tree(os.path.join(self.folder, "activities"))

        # a link is in both files or in neither
        r = self.gcj("plan", "link", planned, actual, expect=5)
        self.assertIn(actual, r["error"])
        self.assertNotIn("Linked Filename", self.tags(planned, planned=True))
        self.assertNotIn("Linked Filename", self.tags(actual))
        r = self.gcj("plan", "unlink", linked, expect=5)
        self.assertIn(other, r["error"])
        self.assertEqual(self.tags(linked, planned=True)["Linked Filename"], other + ".json")
        self.assertEqual(self.tags(other)["Linked Filename"], linked + ".json")
        # a move that can't update the linked activity fails, naming it
        r = self.gcj("plan", "move", linked, "--to", day(7), expect=5)
        self.assertIn(other, r["error"])
        self.assertClosed()


class TestPlansSeasonsAndExpectedPMC(Headless):
    """the streams together: a plan made with plan add feeds the expected PMC, and
    the plan commands take --season"""

    imports = [RIDE_POWER]

    def test_plan_add_feeds_the_expected_pmc(self):
        self.gcj("plan", "add", "--date", day(4), "--time", "09:00", "--sport", "Bike",
                 "--duration", "1:00:00", "--bikestress", "90")
        days = {d["date"]: d for d in self.gcj("pmc", "--series", "expected", "--from", day(3))["data"]["days"]}
        self.assertEqual(max(days), day(4))
        self.assertEqual(days[day(4)]["stress"], 90)
        self.assertGreater(days[day(4)]["ctl"], days[day(3)]["ctl"])
        actual = {d["date"]: d for d in self.gcj("pmc", "--from", day(-1))["data"]["days"]}
        self.assertEqual(max(actual), day())

    def test_plan_commands_take_a_season(self):
        self.gcj("season", "add", "Block", "--from", day(10), "--to", day(16))
        self.gcj("plan", "add", "--date", day(12), "--sport", "Run", "--duration", "0:40:00")
        self.gcj("plan", "add", "--date", day(20), "--sport", "Run", "--duration", "0:30:00")
        listed = [p["date"] for p in self.gcj("plan", "list", "--season", "Block")["data"]["planned"]]
        self.assertEqual(listed, [day(12)])
        bundle = os.path.join(self.tmp, "block.gcplan")
        self.gcj("-o", bundle, "plan", "export", "--season", "Block", "--name", "Block")
        self.assertTrue(zipfile.is_zipfile(bundle))
        summary = self.gcj("calendar", "summary", "--season", "Block", "--days", "7")["data"]
        self.assertEqual(summary["summaries"][0]["from"], day(10))
        self.gcj("plan", "adherence", "--season", "Block")
        self.gcj("plan", "list", "--season", "Block", "--from", day(10), expect=2)
        self.gcj("plan", "export", "--name", "Block", expect=2)
        self.gcj("calendar", "summary", "--season", "No such season", expect=3)


@unittest.skipIf(os.name == "nt" or running_as_root(), "file permissions don't stop Windows or root")
class TestAthleteSettingsReadOnlyRest(Headless):
    """in the server, a setting that could not be written is undone: the next request doesn't see it"""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.server, cls.port = start_server(cls.env, cls.home, os.path.join(cls.tmp, "server.log"))

    @classmethod
    def tearDownClass(cls):
        stop_server(cls.server)
        super().tearDownClass()

    def call(self, method, path, body=None):
        url = "http://127.0.0.1:%d/v1/athletes/%s%s" % (self.port, self.athlete, path)
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method,
                                     headers={"Content-Type": "application/json"} if data else {})
        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_failed_athlete_set(self):
        status, env = self.call("GET", "")
        self.assertEqual(status, 200, env)
        before = env["data"]
        config = os.path.join(self.folder, "config")
        for name in os.listdir(config) + [""]:
            full = os.path.join(config, name)
            mode = os.stat(full).st_mode
            os.chmod(full, mode & ~0o222)
            self.addCleanup(os.chmod, full, mode)
        # a key with a value and keys without one
        status, env = self.call("PUT", "", {"weight": 60, "nickname": "Zed", "sts-days": 10})
        self.assertNotEqual(status, 200, env)
        self.assertIn("athlete-preferences.ini", env["error"])
        status, env = self.call("GET", "")
        self.assertEqual(status, 200, env)
        for key in ("weight", "nickname", "sts_days"):
            self.assertEqual(env["data"][key], before[key], key)


if __name__ == "__main__":
    unittest.main()
