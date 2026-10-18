#!/usr/bin/env bash
# Validate the checked-in Incus profile using Ruby's standard YAML parser.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
profile="${repo_root}/ci/incus-profile-bdfs.yaml"

command -v ruby >/dev/null 2>&1 || {
  echo "ERROR: ruby is required to parse the Incus profile YAML" >&2
  exit 1
}

PROFILE_PATH="${profile}" ruby -ryaml -e '
  path = ENV.fetch("PROFILE_PATH")
  profile = YAML.safe_load(File.read(path), aliases: false)
  abort "ERROR: profile must be a YAML mapping" unless profile.is_a?(Hash)
  abort "ERROR: profile name must be bdfs-privileged" unless profile["name"] == "bdfs-privileged"
  abort "ERROR: profile description must be present" unless profile["description"].is_a?(String) && !profile["description"].empty?
  config = profile["config"]
  devices = profile["devices"]
  abort "ERROR: config must be a mapping" unless config.is_a?(Hash)
  abort "ERROR: privileged mode must be explicitly enabled" unless config["security.privileged"] == "true"
  abort "ERROR: devices must be a mapping" unless devices.is_a?(Hash)
  %w[root eth0 dev fuse].each do |device|
    abort "ERROR: required device #{device.inspect} is missing" unless devices.key?(device)
  end
  puts "Incus profile YAML and required privileged-runner settings are valid."
'
