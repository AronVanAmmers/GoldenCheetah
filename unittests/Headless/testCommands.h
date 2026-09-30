// A small command table shared by the headless unit tests. It mirrors the
// shapes the real commands use (positional repeated files, flags, typed
// options, REST routes) without needing an athlete.

#ifndef TEST_COMMANDS_H
#define TEST_COMMANDS_H

#include "CommandRegistry.h"

using namespace Headless;

inline CommandResult noop(CommandEnvironment &, const CommandRequest &) { return CommandResult::success(); }

inline CommandRegistry testRegistry()
{
    CommandRegistry r;

    Command import;
    import.spec.name = "import";
    import.spec.summary = "import files";
    import.spec.params << ParamSpec("file", ParamType::Path, "files").req().pos().many();
    import.spec.params << ParamSpec("dry-run", ParamType::Bool, "change nothing");
    import.spec.params << ParamSpec("recursive", ParamType::Bool, "recurse");
    import.spec.httpMethod = "POST";
    import.spec.httpPath = "/athletes/{athlete}/imports";
    import.handler = noop;
    r.add(import);

    Command list;
    list.spec.name = "activity.list";
    list.spec.summary = "list activities";
    list.spec.params << ParamSpec("activity", ParamType::String, "ids").pos().many();
    list.spec.params << ParamSpec("filter", ParamType::String, "filter");
    list.spec.params << ParamSpec("from", ParamType::Date, "from");
    list.spec.params << ParamSpec("limit", ParamType::Int, "limit");
    list.spec.params << ParamSpec("metric", ParamType::String, "metrics").many();
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/activities";
    list.handler = noop;
    r.add(list);

    Command show;
    show.spec.name = "activity.show";
    show.spec.summary = "show an activity";
    show.spec.params << ParamSpec("activity", ParamType::String, "id").req().pos();
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/activities/{activity}";
    show.handler = noop;
    r.add(show);

    Command del;
    del.spec.name = "activity.delete";
    del.spec.summary = "delete activities";
    del.spec.params << ParamSpec("activity", ParamType::String, "ids").req().pos().many();
    del.spec.httpMethod = "DELETE";
    del.spec.httpPath = "/athletes/{athlete}/activities/{activity}";
    del.handler = noop;
    r.add(del);

    Command chart;
    chart.spec.name = "chart.activity";
    chart.spec.summary = "draw";
    chart.spec.params << ParamSpec("activity", ParamType::String, "id").req().pos();
    chart.spec.params << ParamSpec("as", ParamType::String, "format").def("png").oneOf({ "png", "svg", "pdf" });
    chart.spec.params << ParamSpec("width", ParamType::Int, "width").def(1200);
    chart.spec.params << ParamSpec("smooth", ParamType::Double, "smoothing");
    chart.spec.httpMethod = "GET";
    chart.spec.httpPath = "/athletes/{athlete}/activities/{activity}/chart";
    chart.handler = noop;
    r.add(chart);

    Command cp;
    cp.spec.name = "cp";
    cp.spec.summary = "fit";
    cp.spec.httpMethod = "GET";
    cp.spec.httpPath = "/athletes/{athlete}/cp";
    cp.handler = noop;
    r.add(cp);

    Command est;
    est.spec.name = "cp.estimates";
    est.spec.summary = "estimates";
    est.spec.httpMethod = "GET";
    est.spec.httpPath = "/athletes/{athlete}/cp/estimates";
    est.handler = noop;
    r.add(est);

    Command metric;
    metric.spec.name = "metric.user.add";
    metric.spec.summary = "add a user metric";
    metric.spec.scope = Scope::Global;
    metric.spec.params << ParamSpec("program", ParamType::String, "formula");
    metric.spec.params << ParamSpec("file", ParamType::Path, "formula file").cliOnly();
    metric.spec.httpMethod = "POST";
    metric.spec.httpPath = "/metrics/user";
    metric.handler = noop;
    r.add(metric);

    Command fields;
    fields.spec.name = "field.list";
    fields.spec.summary = "fields";
    fields.spec.scope = Scope::Global;
    fields.spec.httpMethod = "GET";
    fields.spec.httpPath = "/fields";
    fields.handler = noop;
    r.add(fields);

    return r;
}

#endif
