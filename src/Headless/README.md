# Headless GoldenCheetah: design notes

User documentation is in [doc/user/Headless.md](../../doc/user/Headless.md). This file is for people changing the code.

## Layers

```
 argv ─► CliParser ─► CliMain ─┐                          ┌─► text / JSON / file
                               ├─► CommandRunner ─► handler ─► CommandResult
 HTTP ─► RestRouter ─► RestServer ┘        │                  └─► JSON / file (HTTP)
                                           ▼
                                    AthleteSession  (lock, open, refresh, close)
                                           │
                                   Context / Athlete / RideCache  (the existing core)
```

- **The command model** (`HeadlessCommand.h`, `CommandRegistry`). Each command is declared once, as a `CommandSpec`: its dotted name (`activity.list`), typed parameters, and REST method and path. It is implemented once, as a handler that takes a `CommandRequest` and returns a `CommandResult` (structured `data`, optional human `text`, optional binary `payload`). The registry validates and coerces arguments for every entry point, so a typo is reported the same way everywhere. None of this depends on GoldenCheetah internals.
- **The core**:
  - `CommandRunner` validates a request. For athlete commands it opens an `AthleteSession` around the handler, which is closed before the result is returned.
  - `HeadlessApp` does the process-wide start-up that `main()` does for the GUI: settings, metrics, colours, the train DB and Python.
  - `AthleteSession` takes the `AthleteLock`, opens `Context(nullptr)` and `Athlete`, and waits for the ride cache refresh.
  - The `*Commands.cpp` files hold the handlers, one file per area.
- **Entry points** only translate:
  - `CliParser` and `CliMain` turn argv into a request and print the result.
  - `RestRouter` maps HTTP method, path, query and body onto a request, using each spec's REST binding. `RestServer` runs requests one at a time on the main thread and writes the response.
  - The `serve` command belongs to the CLI entry point, not the registry.

## Headless changes in the existing core

A `Context` with a null main window is headless (`Context::isHeadless()`), and `GlobalContext::isHeadless()` is set for the whole process. The core checks these where it used to assume a window:

- Configuration problems are logged instead of shown as dialogs.
- The ride cache load blocks until it is done.
- Downloads and backup-on-close don't run.
- Upgrade questions are left for the GUI.
- The estimator is only started when a command asks for estimates.

Saving an activity lives in `RideCache::saveSilent` (formerly `MainWindow::saveSilent`), so the GUI and headless sessions save the same way.

`AthleteLock` (a `QLockFile`) is taken by `Athlete` itself, so the GUI and headless sessions exclude each other. Within one process the lock is shared. The lock file is kept on this machine (`$XDG_RUNTIME_DIR/GoldenCheetah/locks`, else the user's cache folder, named by a hash of the athlete folder's path), not in the athlete folder: athlete folders are often synced between machines, and a lock synced from another machine would never be stale. So it excludes processes of the same user on one machine, not two machines sharing a synced folder.

## Adding a command

1. Write a handler `CommandResult f(CommandEnvironment &env, const CommandRequest &request)` in the area's `*Commands.cpp`:
   - Read validated arguments from `request.args`.
   - For athlete commands, use `env.session`.
   - Return `CommandResult::success(data)`, or `failure(Status::..., message)`.
   - Put files in `payload`.
2. Register a `Command` with its spec in that file's `register...Commands()`:
   - `ParamSpec(...).req().pos().many().def(...).oneOf(...)`
   - `Scope::Athlete` or `Scope::Global`
   - `httpMethod` and `httpPath`
3. Add tests: the unit tests under `unittests/Headless` for anything that doesn't need an athlete, and `unittests/Headless/integration/test_headless.py` for behaviour end to end.

The command then appears in `--help`, `commands`, the REST API and `openapi.json` without further work.

## Tests

```sh
# unit tests (QtTest), after building GoldenCheetah
cp unittests/unittests.pri.in unittests/unittests.pri
cd unittests && qmake -recursive && make && make check

# end to end, needs only python3 (GC_BINARY to point at another build)
python3 unittests/Headless/integration/test_headless.py -v
```
