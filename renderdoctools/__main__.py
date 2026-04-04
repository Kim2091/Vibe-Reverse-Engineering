"""CLI entry point for renderdoctools -- RenderDoc capture analysis toolkit.

Usage:  python -m renderdoctools <command> <capture.rdc> [args]

Event browser:
    python -m renderdoctools events <capture.rdc> [--draws-only] [--filter TEXT]

Pipeline state:
    python -m renderdoctools pipeline <capture.rdc> --event <EID> [--stage STAGE]

Textures:
    python -m renderdoctools textures <capture.rdc> --event <EID> [--save-all DIR]

Shaders:
    python -m renderdoctools shaders <capture.rdc> --event <EID> [--stage STAGE]

Mesh data:
    python -m renderdoctools mesh <capture.rdc> --event <EID> [--post-vs]

GPU counters:
    python -m renderdoctools counters <capture.rdc> [--fetch NAME] [--zero-samples]

Analysis:
    python -m renderdoctools analyze <capture.rdc> [--summary] [--biggest-draws N]

Utilities:
    python -m renderdoctools open <capture.rdc>
    python -m renderdoctools capture <exe> [--output FILE]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from . import core


def cmd_events(args: argparse.Namespace) -> None:
    config = {
        "draws_only": args.draws_only,
        "filter": args.filter or "",
    }
    result = core.run_script("events", args.capture, config)

    if args.json:
        print(json.dumps(result, indent=2))
        return

    if "error" in result:
        print("[error] %s" % result["error"], file=sys.stderr)
        sys.exit(1)

    events = result["events"]
    print("=== %d events ===" % result["total"])

    for ev in events:
        indent = "  " * ev["depth"]
        tag = ""
        if ev["draw"]:
            tag = " [DRAW idx=%d inst=%d]" % (ev["numIndices"], ev["numInstances"])
        elif ev["clear"]:
            tag = " [CLEAR]"
        print("%s%d: %s%s" % (indent, ev["eid"], ev["name"], tag))


def cmd_open(args: argparse.Namespace) -> None:
    qrd = core.find_renderdoc()
    capture = str(Path(args.capture).resolve())
    subprocess.Popen([str(qrd), capture])
    print("Opened %s in RenderDoc." % capture)


def cmd_pipeline(args: argparse.Namespace) -> None:
    config = {
        "event_id": args.event,
        "stage": args.stage or "",
    }
    result = core.run_script("pipeline", args.capture, config)

    if args.json:
        print(json.dumps(result, indent=2))
        return

    if "error" in result:
        print("[error] %s" % result["error"], file=sys.stderr)
        sys.exit(1)

    print("=== Pipeline State @ EID %d ===" % result["event_id"])
    for stage_name, info in result["stages"].items():
        print("\n[%s]" % stage_name.upper())
        print("  entry: %s" % info["entryPoint"])
        if info["constantBuffers"]:
            print("  cbuffers: %d" % len(info["constantBuffers"]))
            for cb in info["constantBuffers"]:
                print("    %d: %s (%d bytes)" % (cb["index"], cb["name"], cb["byteSize"]))
        if info["readOnlyResources"]:
            print("  SRVs: %d" % len(info["readOnlyResources"]))
            for r in info["readOnlyResources"]:
                print("    %d: %s (%s)" % (r["index"], r["name"], r["type"]))
        if info["readWriteResources"]:
            print("  UAVs: %d" % len(info["readWriteResources"]))
            for r in info["readWriteResources"]:
                print("    %d: %s (%s)" % (r["index"], r["name"], r["type"]))

    if result.get("renderTargets"):
        print("\nRender Targets: %s" % ", ".join(result["renderTargets"]))
    if result.get("depthTarget"):
        print("Depth Target: %s" % result["depthTarget"])


def cmd_textures(args: argparse.Namespace) -> None:
    config = {
        "event_id": args.event,
        "save_all": args.save_all or "",
        "save_rid": args.save or "",
        "format": args.format or "png",
        "save_output": args.save_output or "",
    }
    result = core.run_script("textures", args.capture, config)

    if args.json:
        print(json.dumps(result, indent=2))
        return

    if "error" in result:
        print("[error] %s" % result["error"], file=sys.stderr)
        sys.exit(1)

    print("=== %d textures @ EID %d ===" % (result["total"], args.event))
    for tex in result["textures"]:
        dim = "%dx%d" % (tex["width"], tex["height"])
        if tex["depth"] > 1:
            dim += "x%d" % tex["depth"]
        print("  %s  %s  %s  [%s]  %s" % (
            tex["resourceId"].rjust(10),
            dim.ljust(12),
            tex["format"][:24].ljust(24),
            tex["binding"],
            tex.get("name", ""),
        ))

    if result.get("saved"):
        print("\nSaved %d textures:" % len(result["saved"]))
        for f in result["saved"]:
            print("  %s" % f)


def cmd_shaders(args: argparse.Namespace) -> None:
    config = {
        "event_id": args.event,
        "stage": args.stage or "",
        "cbuffers": args.cbuffers,
    }
    result = core.run_script("shaders", args.capture, config)

    if args.json:
        print(json.dumps(result, indent=2))
        return

    if "error" in result:
        print("[error] %s" % result["error"], file=sys.stderr)
        sys.exit(1)

    print("=== Shaders @ EID %d [%s] ===" % (result["event_id"], result["disasmTarget"]))
    for stage_name, info in result["shaders"].items():
        print("\n-- %s -- (entry: %s)" % (stage_name.upper(), info["entryPoint"]))
        print(info["disassembly"][:2000])
        if len(info["disassembly"]) > 2000:
            print("... (truncated, use --json for full output)")

        if "constantBuffers" in info:
            for cb in info["constantBuffers"]:
                print("\n  cbuffer %s [%d]:" % (cb["name"], cb["index"]))
                if "error" in cb:
                    print("    (error: %s)" % cb["error"])
                    continue
                for v in cb.get("variables", []):
                    if "values" in v:
                        vals = ", ".join("%.4f" % x for x in v["values"])
                        print("    %s: [%s]" % (v["name"], vals))


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="renderdoctools",
        description="RenderDoc capture analysis toolkit",
    )
    sub = parser.add_subparsers(dest="command")
    sub.required = True

    # events
    p_events = sub.add_parser("events", help="List events and draw calls")
    p_events.add_argument("capture", help="Path to .rdc capture file")
    p_events.add_argument("--draws-only", action="store_true", help="Only show draw calls")
    p_events.add_argument("--filter", type=str, default="", help="Filter events by name")
    p_events.add_argument("--json", action="store_true", help="Output raw JSON")
    p_events.add_argument("--output", type=str, help="Write output to file")
    p_events.set_defaults(func=cmd_events)

    # open
    p_open = sub.add_parser("open", help="Launch RenderDoc GUI with capture")
    p_open.add_argument("capture", help="Path to .rdc capture file")
    p_open.set_defaults(func=cmd_open)

    # pipeline
    p_pipe = sub.add_parser("pipeline", help="Inspect pipeline state at an event")
    p_pipe.add_argument("capture", help="Path to .rdc capture file")
    p_pipe.add_argument("--event", type=int, required=True, help="Event ID")
    p_pipe.add_argument("--stage", type=str, default="", help="Filter to stage: vertex, pixel, geometry, hull, domain, compute")
    p_pipe.add_argument("--json", action="store_true", help="Output raw JSON")
    p_pipe.add_argument("--output", type=str, help="Write output to file")
    p_pipe.set_defaults(func=cmd_pipeline)

    # textures
    p_tex = sub.add_parser("textures", help="List and export textures at an event")
    p_tex.add_argument("capture", help="Path to .rdc capture file")
    p_tex.add_argument("--event", type=int, required=True, help="Event ID")
    p_tex.add_argument("--save-all", type=str, metavar="DIR", help="Export all textures to directory")
    p_tex.add_argument("--save", type=str, metavar="RID", help="Export specific texture by resource ID")
    p_tex.add_argument("--save-output", type=str, metavar="FILE", help="Output path for --save")
    p_tex.add_argument("--format", type=str, default="png", choices=["png", "jpg", "dds", "hdr", "bmp", "tga"])
    p_tex.add_argument("--json", action="store_true", help="Output raw JSON")
    p_tex.add_argument("--output", type=str, help="Write output to file")
    p_tex.set_defaults(func=cmd_textures)

    # shaders
    p_shd = sub.add_parser("shaders", help="Disassemble shaders and inspect cbuffers")
    p_shd.add_argument("capture", help="Path to .rdc capture file")
    p_shd.add_argument("--event", type=int, required=True, help="Event ID")
    p_shd.add_argument("--stage", type=str, default="", help="Filter to stage")
    p_shd.add_argument("--cbuffers", action="store_true", help="Include constant buffer contents")
    p_shd.add_argument("--json", action="store_true", help="Output raw JSON")
    p_shd.add_argument("--output", type=str, help="Write output to file")
    p_shd.set_defaults(func=cmd_shaders)

    args = parser.parse_args()

    # Handle --output redirect
    if hasattr(args, "output") and args.output:
        with open(args.output, "w") as f:
            old_stdout = sys.stdout
            sys.stdout = f
            args.func(args)
            sys.stdout = old_stdout
    else:
        args.func(args)


if __name__ == "__main__":
    main()
