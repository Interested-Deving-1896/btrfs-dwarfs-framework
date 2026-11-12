# SPDX-License-Identifier: GPL-2.0-or-later
"""Repository-level contract tests for FSA mode and socket build wiring."""

import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FSA_MODE = ROOT / "scripts" / "includes" / "fsa-mode.sh"


def run_fsa_mode(cwd: Path, **overrides: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.update(
        {
            "FSA_MANAGED": "",
            "GH_TOKEN": "",
            "GITHUB_REPOSITORY": "Interested-Deving-1896/btrfs-dwarfs-framework",
            **overrides,
        }
    )
    return subprocess.run(
        ["bash", "-c", 'source "$1"; fsa_is_managed', "fsa-mode-test", str(FSA_MODE)],
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def test_bdfs_autonomous_marker_overrides_owner_repository_heuristic() -> None:
    result = run_fsa_mode(ROOT, GH_TOKEN="test-token")
    assert result.returncode == 1
    assert "autonomous marker present" in result.stderr


def test_explicit_false_disables_managed_mode_without_api_lookup(tmp_path: Path) -> None:
    result = run_fsa_mode(tmp_path, FSA_MANAGED="false", GH_TOKEN="test-token")
    assert result.returncode == 1
    assert "explicit autonomous mode" in result.stderr


def test_missing_marker_and_token_defaults_to_autonomous_mode(tmp_path: Path) -> None:
    result = run_fsa_mode(tmp_path)
    assert result.returncode == 1
    assert "No GH_TOKEN" in result.stderr


def test_production_socket_io_is_wired_into_daemon_and_unit_tests() -> None:
    socket_source = ROOT / "userspace" / "daemon" / "bdfs_socket_io.c"
    daemon_cmake = (ROOT / "userspace" / "CMakeLists.txt").read_text()
    unit_cmake = (ROOT / "tests" / "unit" / "CMakeLists.txt").read_text()

    assert socket_source.is_file()
    assert "daemon/bdfs_socket_io.c" in daemon_cmake
    assert "../../userspace/daemon/bdfs_socket_io.c" in unit_cmake
