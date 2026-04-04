# tests/test_renderdoctools/test_integration.py
"""Integration tests for renderdoctools against real RenderDoc captures.

Requires:
- RenderDoc extracted to tools/RenderDoc_1.43_64/ (or tools/renderdoc/)
- At least one .rdc capture file

Skip automatically if RenderDoc or captures are not available.
"""
from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path

import pytest

from renderdoctools import core

# ── Fixtures ──────────────────────────────────────────────────────────────

CAPTURE_DIR = Path(__file__).resolve().parent.parent / "test_rdc"


def _find_capture() -> Path | None:
    """Find an .rdc capture in tests/test_rdc/."""
    if not CAPTURE_DIR.is_dir():
        return None
    for f in CAPTURE_DIR.iterdir():
        if f.suffix == ".rdc":
            return f
    return None


def _renderdoc_available() -> bool:
    """Check if bundled RenderDoc is available."""
    try:
        core.find_renderdoc()
        return True
    except FileNotFoundError:
        return False


_capture = _find_capture()
_has_renderdoc = _renderdoc_available()

skip_no_renderdoc = pytest.mark.skipif(
    not _has_renderdoc, reason="RenderDoc not found in tools/"
)
skip_no_capture = pytest.mark.skipif(
    _capture is None, reason="No .rdc capture found in %s" % CAPTURE_DIR
)
requires_integration = pytest.mark.skipif(
    not (_has_renderdoc and _capture),
    reason="Integration test requires RenderDoc + capture file",
)


# ── Tests ─────────────────────────────────────────────────────────────────


@requires_integration
class TestEvents:
    def test_events_returns_list(self):
        result = core.run_script("events", str(_capture), {"draws_only": False, "filter": ""})
        assert "events" in result
        assert "total" in result
        assert result["total"] > 0
        assert isinstance(result["events"], list)

    def test_events_have_required_fields(self):
        result = core.run_script("events", str(_capture), {"draws_only": False, "filter": ""})
        ev = result["events"][0]
        assert "eid" in ev
        assert "name" in ev
        assert "depth" in ev
        assert "flags" in ev
        assert "draw" in ev
        assert "numIndices" in ev

    def test_draws_only_filters(self):
        all_result = core.run_script("events", str(_capture), {"draws_only": False, "filter": ""})
        draws_result = core.run_script("events", str(_capture), {"draws_only": True, "filter": ""})
        # draws-only should return fewer or equal events
        assert draws_result["total"] <= all_result["total"]
        # every event should be a draw
        for ev in draws_result["events"]:
            assert ev["draw"] is True

    def test_filter_narrows_results(self):
        all_result = core.run_script("events", str(_capture), {"draws_only": False, "filter": ""})
        filtered = core.run_script("events", str(_capture), {"draws_only": False, "filter": "DrawIndexed"})
        assert filtered["total"] <= all_result["total"]
        for ev in filtered["events"]:
            assert "DrawIndexed" in ev["name"]


@requires_integration
class TestAnalyze:
    def test_summary(self):
        result = core.run_script("analyze", str(_capture), {
            "summary": True, "biggest_draws": 0, "render_targets": False,
        })
        s = result["summary"]
        assert s["totalEvents"] > 0
        assert s["totalDraws"] > 0
        assert s["totalIndices"] > 0

    def test_biggest_draws(self):
        result = core.run_script("analyze", str(_capture), {
            "summary": False, "biggest_draws": 5, "render_targets": False,
        })
        draws = result["biggestDraws"]
        assert len(draws) == 5
        # should be sorted descending by numIndices
        for i in range(len(draws) - 1):
            assert draws[i]["numIndices"] >= draws[i + 1]["numIndices"]

    def test_render_targets(self):
        result = core.run_script("analyze", str(_capture), {
            "summary": False, "biggest_draws": 0, "render_targets": True,
        })
        rts = result["renderTargets"]
        assert len(rts) > 0
        for rt in rts:
            assert "resourceId" in rt
            assert "drawCount" in rt
            assert rt["drawCount"] > 0


@requires_integration
class TestPipeline:
    def _get_first_draw_eid(self):
        result = core.run_script("events", str(_capture), {"draws_only": True, "filter": ""})
        return result["events"][0]["eid"]

    def test_pipeline_returns_stages(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("pipeline", str(_capture), {"event_id": eid, "stage": ""})
        assert result["event_id"] == eid
        assert "stages" in result
        assert len(result["stages"]) > 0

    def test_pipeline_has_render_targets(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("pipeline", str(_capture), {"event_id": eid, "stage": ""})
        assert "renderTargets" in result

    def test_pipeline_stage_filter(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("pipeline", str(_capture), {"event_id": eid, "stage": "vertex"})
        # should only contain vertex stage (if bound)
        for stage_name in result["stages"]:
            assert stage_name == "vertex"


@requires_integration
class TestTextures:
    def _get_first_draw_eid(self):
        result = core.run_script("events", str(_capture), {"draws_only": True, "filter": ""})
        return result["events"][0]["eid"]

    def test_textures_list(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("textures", str(_capture), {
            "event_id": eid, "save_all": "", "save_rid": "",
            "format": "png", "save_output": "",
        })
        assert "textures" in result
        assert result["total"] > 0
        tex = result["textures"][0]
        assert "resourceId" in tex
        assert "width" in tex
        assert "height" in tex
        assert "format" in tex
        assert "binding" in tex

    def test_save_single_texture(self):
        eid = self._get_first_draw_eid()
        # First get the texture list to find a valid RID
        result = core.run_script("textures", str(_capture), {
            "event_id": eid, "save_all": "", "save_rid": "",
            "format": "png", "save_output": "",
        })
        rid = result["textures"][0]["resourceId"]

        with tempfile.TemporaryDirectory(prefix="rdtools_test_") as tmpdir:
            out_path = os.path.join(tmpdir, "test_texture.png")
            result = core.run_script("textures", str(_capture), {
                "event_id": eid, "save_all": "", "save_rid": rid,
                "format": "png", "save_output": out_path,
            })
            assert len(result["saved"]) == 1
            assert os.path.isfile(out_path)
            assert os.path.getsize(out_path) > 0

    def test_save_all_textures(self):
        eid = self._get_first_draw_eid()
        with tempfile.TemporaryDirectory(prefix="rdtools_test_") as tmpdir:
            out_dir = os.path.join(tmpdir, "texdump")
            result = core.run_script("textures", str(_capture), {
                "event_id": eid, "save_all": out_dir, "save_rid": "",
                "format": "png", "save_output": "",
            })
            assert len(result["saved"]) == result["total"]
            for f in result["saved"]:
                assert os.path.isfile(f)
                assert os.path.getsize(f) > 0


@requires_integration
class TestShaders:
    def _get_first_draw_eid(self):
        result = core.run_script("events", str(_capture), {"draws_only": True, "filter": ""})
        return result["events"][0]["eid"]

    def test_shaders_disassembly(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("shaders", str(_capture), {
            "event_id": eid, "stage": "", "cbuffers": False,
        })
        assert "shaders" in result
        assert len(result["shaders"]) > 0
        # At least one stage should have disassembly
        for stage_name, info in result["shaders"].items():
            assert "disassembly" in info
            assert len(info["disassembly"]) > 0
            assert "entryPoint" in info

    def test_shaders_stage_filter(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("shaders", str(_capture), {
            "event_id": eid, "stage": "vertex", "cbuffers": False,
        })
        for stage_name in result["shaders"]:
            assert stage_name == "vertex"

    def test_shaders_cbuffers(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("shaders", str(_capture), {
            "event_id": eid, "stage": "vertex", "cbuffers": True,
        })
        if "vertex" in result["shaders"]:
            info = result["shaders"]["vertex"]
            assert "constantBuffers" in info
            # FO4 vertex shaders typically have cbuffers
            if len(info["constantBuffers"]) > 0:
                cb = info["constantBuffers"][0]
                assert "name" in cb
                assert "index" in cb


@requires_integration
class TestMesh:
    def _get_first_draw_eid(self):
        result = core.run_script("events", str(_capture), {"draws_only": True, "filter": ""})
        return result["events"][0]["eid"]

    def test_mesh_input(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("mesh", str(_capture), {
            "event_id": eid, "post_vs": False, "indices": "",
        })
        assert result["event_id"] == eid
        assert result["post_vs"] is False
        assert len(result["attributes"]) > 0
        assert len(result["vertices"]) > 0
        # Each vertex should have an index
        for v in result["vertices"]:
            assert "index" in v

    def test_mesh_index_range(self):
        eid = self._get_first_draw_eid()
        result = core.run_script("mesh", str(_capture), {
            "event_id": eid, "post_vs": False, "indices": "0-3",
        })
        assert len(result["vertices"]) <= 3
