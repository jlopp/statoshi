#!/usr/bin/env python3
"""Convert a classic Grafana dashboard JSON to the v2 resource form.

Pasting a classic dashboard into the new-layout dashboard editor fails with:

    The required property `metadata` is missing at `#`
    The required property `spec` is missing at `#`

The new-layout editor (feature toggle `dashboardNewLayouts`, enabled on
statoshi.info) edits the dashboard.grafana.app/v2 resource shape, so it needs:

    { "apiVersion": "dashboard.grafana.app/v2", "kind": "Dashboard",
      "metadata": { "name": "<uid>" }, "spec": { "elements": {...}, ... } }

This script performs that conversion without a running Grafana, so you can
paste the result straight into the editor.

Usage:
    ./to-v2.py ../../blocks.json > /tmp/blocks-v2.json
    ./to-v2.py --paste ../../blocks.json    # copy to the clipboard

Conversion is layout-aware: rows become RowsLayoutRow containers, panel
gridPos becomes GridLayoutItem coordinates, and panel targets become
PanelQuery entries. The result is validated structurally before it is printed.
"""

import argparse
import json
import shutil
import subprocess
import sys

ROW_HEIGHT = 1  # gridPos units are already 24-wide grid units


# Every field the v2 DashboardLink schema marks as required. Classic
# dashboards only set a subset, so missing ones are filled from here.
LINK_DEFAULTS = {
    "title": "",
    "type": "link",
    "icon": "",
    "tooltip": "",
    "tags": [],
    "asDropdown": False,
    "targetBlank": False,
    "includeVars": False,
    "keepTime": False,
}

# Panel links use the much narrower DataLink schema, which defines only these
# three fields, so dashboard-link defaults must not be added to them.
DATA_LINK_FIELDS = ("title", "url", "targetBlank")


def _convert_panel_links(links):
    """Reduce panel links to the v2 DataLink fields."""
    out = []
    for link in links or []:
        out.append({
            "title": link.get("title", ""),
            "url": link.get("url", ""),
            "targetBlank": bool(link.get("targetBlank", False)),
        })
    return out


def _fix_thresholds(defaults):
    """Coerce null threshold step values to numbers.

    Classic dashboards use `value: null` for the base (negative infinity)
    threshold step. The v2 resource schema types that field as `number`, so a
    pasted dashboard fails with:

        Expected `null` (null) in
        `/spec/elements/<panel>/spec/vizConfig/spec/fieldConfig/defaults/
         thresholds/steps/0/value` to be of type `number`

    Grafana's own server-side serializer also emits null here, so this is a
    frontend-schema strictness issue rather than a conversion bug. Emitting 0
    for the base step is what the editor produces when you create a threshold
    in the UI, and what the live statoshi dashboards store.
    """
    thresholds = defaults.get("thresholds")
    if not isinstance(thresholds, dict):
        return defaults
    steps = thresholds.get("steps")
    if not isinstance(steps, list):
        return defaults
    fixed = []
    for i, step in enumerate(steps):
        if not isinstance(step, dict):
            fixed.append(step)
            continue
        step = dict(step)
        if step.get("value") is None:
            # Only the first (base) step may legitimately be null. Any later
            # null would break ordering, so seed it from the previous step.
            step["value"] = 0 if i == 0 else fixed[-1].get("value", 0)
        fixed.append(step)
    thresholds = dict(thresholds)
    thresholds["steps"] = fixed
    defaults = dict(defaults)
    defaults["thresholds"] = thresholds
    return defaults


class Panel:
    """A classic panel, with its absolute gridPos."""

    def __init__(self, raw, parent_id, row_title):
        self.raw = raw
        self.parent_id = parent_id      # which row this came from (or None)
        self.row_title = row_title
        self.gridPos = raw.get("gridPos", {})


def split_rows(panels):
    """Group classic panels the way Grafana lays them out.

    In the classic model a panel of type "row" opens a collapsible section and
    every following panel belongs to it until the next row panel. Panels keep
    their own absolute gridPos; the row-relative y used by the v2 layout is
    computed at conversion time.
    """
    groups = []
    current = None
    for p in panels:
        if p.get("type") == "row":
            title = (p.get("title") or "").strip()
            # Rows are the v2 grouping primitive, so use the title verbatim.
            # Untitled row panels still start a group; the v2 row just gets an
            # empty title and a hidden header, which matches Grafana's own
            # serialization of a row separator.
            current = {"title": title, "panels": [], "row": p}
            groups.append(current)
            continue
        if current is None:
            groups.append({"title": "", "panels": [], "row": None})
            current = groups[-1]
        current["panels"].append(p)
    if not groups:
        groups.append({"title": "", "panels": [], "row": None})
    return groups


def _convert_links(links):
    """Fill in the fields the v2 DashboardLink schema requires.

    Classic dashboards store a minimal link object:

        {"title": "Blocks", "url": "/d/000000002", "type": "link",
         "keepTime": true, "includeVars": true, "tags": [], "targetBlank": false}

    but the v2 resource schema marks nine fields as required, so a passthrough
    is rejected by the editor with:

        The required property `icon` is missing at ...
        The required property `tooltip` is missing at ...
        The required property `asDropdown` is missing at ...
    """
    out = []
    for link in links or []:
        merged = dict(LINK_DEFAULTS)
        merged.update({k: v for k, v in link.items() if v is not None})
        out.append(merged)
    return out


def convert_dashboard(classic, uid=None):
    """Build a v2 resource document from a classic dashboard dict."""
    name = uid or classic.get("uid")
    if not name:
        raise SystemExit("error: dashboard has no uid; pass --uid explicitly")
    if "spec" in classic and "panels" not in classic:
        raise SystemExit(
            "error: this looks like a v2 resource already, not a classic dashboard"
        )

    elements = {}
    rows = []
    panel_id = 0

    for group in split_rows(classic.get("panels", [])):
        items = []
        # v2 row layouts use y coordinates relative to the row, while classic
        # gridPos is absolute. The row's origin is its first panel's y (a row
        # panel's own gridPos.y is 1 above that, so it is not usable here).
        ys = [int(p.get("gridPos", {}).get("y", 0)) for p in group["panels"]]
        origin_y = min(ys) if ys else 0
        for p in group["panels"]:
            panel_id += 1
            el_name = f"panel-{panel_id}"
            gp = p.get("gridPos", {})
            x = int(gp.get("x", 0))
            y = int(gp.get("y", 0)) - origin_y
            w = int(gp.get("w", 24))
            h = int(gp.get("h", 8))

            queries = []
            for t in p.get("targets", []):
                qspec = {k: v for k, v in t.items()
                         if k not in ("refId", "hide", "datasource")}
                ds = t.get("datasource") or p.get("datasource") or {}
                queries.append({
                    "kind": "PanelQuery",
                    "spec": {
                        "query": {
                            "kind": "DataQuery",
                            "group": (ds.get("type") or "graphite"),
                            "version": "v0",
                            "datasource": {"name": ds.get("uid") or ""},
                            "spec": qspec,
                        },
                        "refId": t.get("refId", "A"),
                        "hidden": bool(t.get("hide", False)),
                    },
                })

            # Classic transformations are {id, options} objects, but the v2
            # resource schema wants a typed resource: the transformer id moves
            # to "group" and the rest goes under "spec". Passing the classic
            # object through verbatim fails with:
            #   required fields of "kind", "group" and "spec" are missing
            #   from "transformations"
            transformations = []
            for t in p.get("transformations") or []:
                spec = {k: v for k, v in t.items() if k != "id"}
                spec.setdefault("disabled", False)
                transformations.append({
                    "kind": "Transformation",
                    "group": t.get("id", ""),
                    "spec": spec,
                })

            # The v2 schema requires fieldConfig.overrides to be present, but
            # classic dashboards routinely omit it (123 panels here have only
            # "defaults"). Pasting without it fails schema validation with
            # "missing required property 'overrides'". Grafana's own v2 output
            # always serializes both keys, so normalize to defaults+overrides.
            field_config = dict(p.get("fieldConfig") or {})
            field_config.setdefault("defaults", {})
            field_config.setdefault("overrides", [])
            field_config["defaults"] = _fix_thresholds(field_config["defaults"])

            datasource = p.get("datasource") or {}
            element = {
                "kind": "Panel",
                "spec": {
                    "id": p.get("id", panel_id),
                    "title": p.get("title", ""),
                    "links": _convert_panel_links(p.get("links")),
                    "data": {
                        "kind": "QueryGroup",
                        "spec": {
                            "queries": queries,
                            "transformations": transformations,
                            "queryOptions": {},
                        },
                    },
                    "vizConfig": {
                        "kind": "VizConfig",
                        "group": p.get("type", "timeseries"),
                        "version": p.get("pluginVersion", "13.3.1"),
                        "spec": {
                            "options": p.get("options", {}),
                            "fieldConfig": field_config,
                        },
                    },
                },
            }
            # Only emit description when set; the v2 schema treats an empty
            # string as a difference from absent, and Grafana omits it.
            if p.get("description"):
                element["spec"]["description"] = p["description"]
            # Classic panels carry a per-panel time override as top-level
            # timeFrom/timeTo. The v2 model has no such top-level fields: the
            # override lives in the panel's queryOptions, and Grafana only
            # honours it when BOTH keys are present. Dropping them silently
            # reverts the panel to the dashboard time range.
            query_options = {}
            if p.get("timeFrom") and p.get("timeTo"):
                query_options["timeFrom"] = p["timeFrom"]
                query_options["timeTo"] = p["timeTo"]
            if p.get("maxDataPoints"):
                query_options["maxDataPoints"] = p["maxDataPoints"]
            if p.get("interval"):
                query_options["minInterval"] = p["interval"]
            if datasource or query_options:
                element["spec"]["data"]["spec"]["queryOptions"] = query_options
            elements[el_name] = element
            items.append({
                "kind": "GridLayoutItem",
                "spec": {
                    "x": x, "y": y, "width": w, "height": h,
                    "element": {"kind": "ElementReference", "name": el_name},
                },
            })

        rows.append({
            "title": group["title"],
            "items": items,
            "collapse": bool((group["row"] or {}).get("collapsed", False)),
        })

    # Drop trailing/leading empty rows so a flat dashboard gets a single
    # GridLayout rather than an empty RowsLayout.
    non_empty = [r for r in rows if r["items"]]
    if len(non_empty) == 1 and not non_empty[0]["title"]:
        layout = {"kind": "GridLayout", "spec": {"items": non_empty[0]["items"]}}
    else:
        layout = {
            "kind": "RowsLayout",
            "spec": {
                "rows": [
                    {
                        "kind": "RowsLayoutRow",
                        "spec": {
                            "title": r.get("title", ""),
                            "collapse": r.get("collapse", False),
                            "hideHeader": r.get("title", "") == "",
                            "layout": {"kind": "GridLayout",
                                       "spec": {"items": r["items"]}},
                        },
                    }
                    for r in non_empty
                ]
            },
        }

    annotations = []
    for a in classic.get("annotations", {}).get("list", []):
        ds = a.get("datasource") or {}
        annotations.append({
            "kind": "AnnotationQuery",
            "spec": {
                "query": {
                    "kind": "DataQuery",
                    "group": ds.get("type") or "grafana",
                    "version": "v0",
                    "datasource": {"name": ds.get("uid") or "-- Grafana --"},
                    "spec": {},
                },
                "enable": bool(a.get("enable", True)),
                "hide": bool(a.get("hide", False)),
                "iconColor": a.get("iconColor", "rgba(0, 211, 255, 1)"),
                "name": a.get("name", ""),
                "builtIn": bool(a.get("builtIn", False)),
            },
        })

    tk = classic.get("timepicker", {})
    spec = {
        "annotations": annotations,
        "cursorSync": {0: "Off", 1: "Crosshair", 2: "Tooltip"}.get(
            classic.get("graphTooltip", 0), "Off"),
        "editable": bool(classic.get("editable", True)),
        "elements": elements,
        "layout": layout,
        "links": _convert_links(classic.get("links")),
        "liveNow": bool(classic.get("liveNow", False)),
        "preload": bool(classic.get("preload", False)),
        "tags": classic.get("tags", []),
        "timeSettings": {
            "timezone": classic.get("timezone", "browser"),
            "from": classic.get("time", {}).get("from", "now-6h"),
            "to": classic.get("time", {}).get("to", "now"),
            "autoRefresh": classic.get("refresh", ""),
            "autoRefreshIntervals": tk.get("refresh_intervals",
                                            ["5s", "10s", "30s", "1m", "5m"]),
            "hideTimepicker": bool(tk.get("hidden", False)),
            "fiscalYearStartMonth": classic.get("fiscalYearStartMonth", 0),
        },
        "title": classic.get("title", ""),
        "variables": [],
        "preferences": {"layout": {"kind": "GridLayout", "spec": {"items": []}}},
    }

    return {
        "kind": "Dashboard",
        "apiVersion": "dashboard.grafana.app/v2",
        "metadata": {"name": name},
        "spec": spec,
    }


def validate(doc):
    """Structural checks that catch the common conversion mistakes."""
    errors = []
    spec = doc.get("spec") or {}
    elements = spec.get("elements") or {}
    layout = spec.get("layout")

    refs = []
    rows = []

    def walk(lay):
        if not lay:
            return
        kind = lay.get("kind")
        inner = lay.get("spec", {})
        if kind == "GridLayout":
            for it in inner.get("items", []):
                refs.append(it["spec"]["element"]["name"])
        elif kind == "RowsLayout":
            for r in inner.get("rows", []):
                items = r.get("spec", {}).get("layout", {}).get("spec", {}).get(
                    "items", [])
                ys = [it["spec"]["y"] for it in items]
                rows.append((r.get("spec", {}),
                             r.get("spec", {}).get("layout", {})))
                if ys and min(ys) != 0:
                    errors.append(
                        f"row {r.get('spec', {}).get('title', '')!r} does not "
                        f"start at y=0 (min y={min(ys)}); v2 row coordinates "
                        f"must be relative to the row")
                walk(r.get("spec", {}).get("layout"))

    walk(layout)
    if len(refs) != len(elements):
        errors.append(f"layout references {len(refs)} elements, "
                      f"but {len(elements)} are defined")
    missing = [r for r in refs if r not in elements]
    if missing:
        errors.append(f"layout references undefined elements: {missing}")
    orphan = sorted(set(elements) - set(refs))
    if orphan:
        errors.append(f"elements not placed in layout: {orphan}")
    if not elements:
        errors.append("no elements converted")

    for name, el in elements.items():
        if el.get("kind") != "Panel":
            errors.append(f"{name}: expected kind Panel, got {el.get('kind')}")
        viz = el.get("spec", {}).get("vizConfig", {})
        if not viz.get("group"):
            errors.append(f"{name}: vizConfig has no group (panel type)")
        fc = viz.get("spec", {}).get("fieldConfig", {})
        # The v2 schema requires both keys; classic dashboards often omit
        # "overrides", which is the most common paste-time validation error.
        for key, wants in (("defaults", dict), ("overrides", list)):
            if key not in fc:
                errors.append(f"{name}: fieldConfig is missing required '{key}'")
            elif not isinstance(fc[key], wants):
                errors.append(f"{name}: fieldConfig.{key} must be "
                              f"{wants.__name__}")
        # Threshold step values are typed as number in the v2 schema; the
        # classic model uses null for the -Infinity base step.
        steps = (fc.get("defaults") or {}).get("thresholds", {}).get("steps") or []
        for i, step in enumerate(steps):
            v = step.get("value") if isinstance(step, dict) else None
            if v is None:
                errors.append(f"{name}: thresholds step {i} has null value; "
                              f"v2 requires a number")
            elif not isinstance(v, (int, float)) or isinstance(v, bool):
                errors.append(f"{name}: thresholds step {i} value {v!r} is not "
                              f"a number")
        # Transformations must be typed v2 resources. Classic {id, options}
        # objects are rejected by the editor with "required fields of 'kind',
        # 'group' and 'spec' are missing from 'transformations'".
        trs = el.get("spec", {}).get("data", {}).get("spec", {}).get(
            "transformations") or []
        for i, tr in enumerate(trs):
            if not isinstance(tr, dict):
                errors.append(f"{name}: transformation {i} is not an object")
                continue
            if "id" in tr:
                errors.append(f"{name}: transformation {i} is in classic "
                              f"{{id, options}} form; v2 needs "
                              f"{{kind, group, spec}}")
            if tr.get("kind") != "Transformation":
                errors.append(f"{name}: transformation {i} kind must be "
                              f"'Transformation', got {tr.get('kind')!r}")
            if not tr.get("group"):
                errors.append(f"{name}: transformation {i} is missing 'group' "
                              f"(the transformer id)")
            if not isinstance(tr.get("spec"), dict):
                errors.append(f"{name}: transformation {i} is missing a 'spec' "
                              f"object")
        # Panel links are DataLink resources: only title/url/targetBlank are
        # defined, so anything else would be dropped rather than emitted.
        for i, link in enumerate(el.get("spec", {}).get("links") or []):
            if not isinstance(link, dict):
                errors.append(f"{name}: panel link {i} is not an object")
                continue
            for key in DATA_LINK_FIELDS:
                if key not in link:
                    errors.append(f"{name}: panel link {i} is missing "
                                  f"required '{key}'")
            extra = sorted(set(link) - set(DATA_LINK_FIELDS))
            if extra:
                errors.append(f"{name}: panel link {i} has fields not in the "
                              f"v2 DataLink schema: {extra}")

    # Dashboard links are the other common paste-time failure: the v2
    # DashboardLink schema requires icon, tooltip and asDropdown, which the
    # classic shape omits.
    for i, link in enumerate(spec.get("links") or []):
        if not isinstance(link, dict):
            errors.append(f"dashboard link {i} is not an object")
            continue
        for key in LINK_DEFAULTS:
            if key not in link:
                errors.append(f"dashboard link {i} ({link.get('title')!r}) is "
                              f"missing required '{key}'")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="classic dashboard JSON file ('-' for stdin)")
    ap.add_argument("--uid", help="override the dashboard uid/metadata.name")
    ap.add_argument("--paste", action="store_true",
                    help="copy the result to the clipboard instead of stdout")
    ap.add_argument("--out", help="write the v2 resource to this file "
                                  "(used with --paste when no clipboard tool "
                                  "is available)")
    ap.add_argument("--check", action="store_true",
                    help="validate only; print problems and exit nonzero")
    args = ap.parse_args()

    raw = sys.stdin.read() if args.path == "-" else open(args.path).read()
    classic = json.loads(raw)

    # Guard against the most common mix-up: converting a file that is already
    # a v2 resource (or pasting the classic file instead of the converted one).
    # A classic dashboard has no "metadata"/"spec"; a v2 resource has both.
    if "metadata" in classic and "spec" in classic:
        raise SystemExit(
            f"error: {args.path} is already a v2 resource, not a classic "
            f"dashboard; nothing to convert")
    if "panels" not in classic:
        raise SystemExit(
            f"error: {args.path} has no top-level 'panels' array, so it does "
            f"not look like a classic dashboard")

    doc = convert_dashboard(classic, args.uid)

    problems = validate(doc)
    if problems:
        for p in problems:
            print(f"validation: {p}", file=sys.stderr)
        if args.check:
            sys.exit(1)

    text = json.dumps(doc, indent=2)
    if args.check:
        print(f"ok: {len(doc['spec']['elements'])} elements")
        return

    # Final guard: the editor validates the pasted root against the v2 resource
    # schema, so anything missing these four keys fails with
    # "required property `metadata`/`spec` is missing at `#`".
    missing = [k for k in ("apiVersion", "kind", "metadata", "spec")
               if k not in doc]
    if missing:
        raise SystemExit(f"error: converted document is missing {missing}; "
                         f"refusing to emit something the editor will reject")

    if args.paste:
        for cmd in (["wl-copy"], ["xclip", "-selection", "clipboard"],
                    ["pbcopy"]):
            if shutil.which(cmd[0]):
                subprocess.run(cmd, input=text, text=True, check=True)
                print(f"copied {len(doc['spec']['elements'])} elements to clipboard "
                      f"({cmd[0]}); paste into the dashboard editor",
                      file=sys.stderr)
                return
        # No clipboard tool: fall back to writing a file the user can open and
        # copy from, rather than failing with nothing to paste.
        out = args.out or "/tmp/dashboard-v2.json"
        with open(out, "w") as fh:
            fh.write(text + "\n")
        print(f"warning: no clipboard tool found (wl-copy/xclip/pbcopy); "
              f"wrote the v2 resource to {out} instead. Copy its contents into "
              f"the dashboard editor.", file=sys.stderr)
        return

    print(text)


if __name__ == "__main__":
    main()
