#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Validate the Incus privileged integration-runner profile's structure.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROFILE="${REPO_ROOT}/ci/incus-profile-bdfs.yaml"

[[ -s "${PROFILE}" ]] || { echo "error: missing or empty ${PROFILE}" >&2; exit 1; }
command -v ruby >/dev/null 2>&1 || { echo "error: ruby is required for YAML profile validation" >&2; exit 127; }

ruby -ryaml -e '
  path = ARGV.fetch(0)
  profile = YAML.safe_load(File.read(path), aliases: false)
  abort "profile must be a mapping" unless profile.is_a?(Hash)
  abort "unexpected profile name" unless profile["name"] == "bdfs-privileged"
  config = profile["config"]
  devices = profile["devices"]
  abort "config must be a mapping" unless config.is_a?(Hash)
  abort "devices must be a mapping" unless devices.is_a?(Hash)
  abort "security.privileged must be true" unless config["security.privileged"] == "true"
  abort "security.nesting must be true" unless config["security.nesting"] == "true"
  %w[root eth0 dev fuse].each do |name|
    abort "missing required device: #{name}" unless devices[name].is_a?(Hash)
  end
  abort "root device must be a disk" unless devices["root"]["type"] == "disk"
  abort "FUSE device must expose /dev/fuse" unless devices["fuse"]["path"] == "/dev/fuse"
  puts "Validated #{path}"
' "${PROFILE}"
