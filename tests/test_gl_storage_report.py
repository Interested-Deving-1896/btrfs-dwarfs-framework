"""Focused tests for the GitLab storage report helper introduced by PR #33."""
import importlib.util
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "gl-storage-report.py"
SPEC = importlib.util.spec_from_file_location("gl_storage_report", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


def test_merge_pages_combines_concatenated_json_arrays():
    assert REPORT.merge_pages('[{"id": 1}][{"id": 2}]') == [{"id": 1}, {"id": 2}]


def test_merge_pages_accepts_whitespace_between_pages():
    assert REPORT.merge_pages('  [{"id": 1}]\n [{"id": 2}]  ') == [
        {"id": 1}, {"id": 2}
    ]


def test_merge_pages_rejects_non_array_page():
    with pytest.raises(ValueError, match="JSON array"):
        REPORT.merge_pages('{"id": 1}')


def test_merge_pages_rejects_malformed_json():
    with pytest.raises(ValueError):
        REPORT.merge_pages('[{"id": 1')


@pytest.mark.parametrize(
    ("value", "expected"),
    [(0, "0B"), (1023, "1023B"), (1024, "1.0K"),
     (1024**2, "1.0M"), (1024**3, "1.0G")],
)
def test_fmt_uses_binary_units(value, expected):
    assert REPORT.fmt(value) == expected


def test_report_sorts_projects_and_prints_totals(capsys):
    projects = [
        {"name": "small", "statistics": {"repository_size": 1024, "storage_size": 2048}},
        {"name": "large", "statistics": {"repository_size": 4096, "storage_size": 8192}},
    ]
    REPORT.report(projects)
    output = capsys.readouterr().out
    assert output.index("large") < output.index("small")
    assert "Total projects found: 2" in output
    assert "TOTAL" in output
