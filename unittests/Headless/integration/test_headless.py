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
import hashlib
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

    def lock_file(self, folder):
        """the athlete's lock, named by a hash of its canonical path"""
        key = os.path.realpath(folder).replace(os.sep, "/")
        if os.name == "nt":
            key = key.lower()
        return os.path.join(self.lock_dir, hashlib.sha1(key.encode("utf-8")).hexdigest()[:16] + ".lock")

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
            lock = self.lock_file(self.folder)
            deadline = time.time() + 20
            while time.time() < deadline and not os.path.exists(lock):
                time.sleep(0.05)
            self.assertTrue(os.path.exists(lock), "serve never took the athlete")

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
        r = self.gca("chart", "activity", "last", "--series", "nothing")
        self.assertEqual(r.code, 5, r)

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
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
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
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
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
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        self.gcj("metric", "user", "add", "--symbol", "keep_me", "--name", "Keep me", "--program", ONES)
        self.addCleanup(lambda: self.gca("metric", "user", "remove", "keep_me"))
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
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
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
                 "--metric", "vo2max", "--style", "dots", "--symbol", "none")
        text = self.gca("chart", "library", "show", "Estimated VO2max")
        self.assertEqual(text.code, 0, text)
        self.assertIn(b"metric  vo2max  dots  none", text.out)

        self.gcj("chart", "library", "add", "--name", "Two",
                 "--metric", "average_power", "--metric", "average_speed", "--style", "dots", expect=2)
        refused = self.gcj("chart", "library", "add", "--name", "PMC", "--pmc", expect=2)
        self.assertIn("not supported", refused["error"])
        refused = self.gcj("chart", "library", "add", "--name", "Bad",
                           "--estimate", "ftp", "--model", "cp2", expect=2)
        self.assertIn("ftp", refused["error"])

        best = self.gcj("chart", "library", "add", "--name", "Peak power", "--by", "day",
                        "--best", "45", "--unit", "min", "--series", "power",
                        "--style", "dots", "--symbol", "circle")["data"]["metrics"]
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

        self.gcj("chart", "library", "curve", "edit", "Peak power", "1", "--style", "line", "--symbol", "square")
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
        os.remove(self.charts_file())
        self.assertFalse(os.path.exists(self.charts_file()))
        self.assertClosed()


if __name__ == "__main__":
    unittest.main()
