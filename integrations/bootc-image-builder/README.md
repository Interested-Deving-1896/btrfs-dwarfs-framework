# bootc-image-builder Integration

Bridges [bootc-image-builder](https://github.com/osbuild/bootc-image-builder)'s
disk image production with btrfs-dwarfs-framework's workspace and snapshot
capabilities.

> **Note on the tool image name:** bootc-image-builder is published at
> `quay.io/centos-bootc/bootc-image-builder` — the `centos-bootc` namespace
> reflects where osbuild chose to publish it, not what it builds. It is a
> distro-neutral tool that produces disk images from **any** bootc-compatible
> OCI image (Fedora, Debian, Ubuntu, custom, etc.).

## What this provides

| File | Purpose |
|---|---|
| `bdfs-bootc-image-builder.sh` | CLI wrapper: build, from-workspace, list-types, push-ami, status |
| `bdfs-bootc-image-builder.conf` | Site configuration (copy to `/etc/bdfs/bootc-image-builder.conf`) |

## How it fits together

bootc-image-builder takes a bootc OCI container image and produces deployable
disk images (qcow2, raw, vmdk, ami, iso, vhd, gce). This integration adds two
things:

1. **Direct builds** — `bdfs-bootc-image-builder build` wraps the
   bootc-image-builder container invocation with bdfs-aware defaults (btrfs
   rootfs, output directory, config.toml resolution).

2. **Workspace → disk image pipeline** — `bdfs-bootc-image-builder from-workspace`
   chains `bdfs-bootc commit` (workspace → OCI image) with
   `bdfs-bootc-image-builder build` (OCI image → disk image) in a single
   command, completing the full bdfs round-trip.

```
bdfs workspace  ──bdfs-bootc commit──►  OCI image  ──bdfs-bootc-image-builder build──►  disk image
      ▲                                     │                                                │
      │                                     │                                                │
bdfs-bootc workspace                  registry push                              qcow2 / raw / vmdk
(from live bootc root)                                                           ami / iso / vhd / gce
```

Combined with the [bootc integration](../bootc/README.md), the full pipeline is:

```
live bootc root
      │
      └──bdfs-bootc workspace──► bdfs workspace (writable)
                                        │
                                        └──bdfs-bootc commit──► OCI image ──► registry
                                                                     │
                                                                     └──bdfs-bootc-image-builder build──► disk image
```

## Install

```bash
install -m 755 bdfs-bootc-image-builder.sh /usr/local/bin/bdfs-bootc-image-builder
install -m 644 bdfs-bootc-image-builder.conf /etc/bdfs/bootc-image-builder.conf
# Edit /etc/bdfs/bootc-image-builder.conf — set BDFS_BOOTC_IMAGE and BDFS_BIB_OUTPUT
```

## Usage

### Build a disk image directly from a bootc OCI image

```bash
# qcow2 (default) — for KVM/QEMU
bdfs-bootc-image-builder build \
    --image quay.io/myorg/myos:stable \
    --out ./output

# Raw disk with btrfs root — recommended for btrfs-dwarfs-framework hosts
bdfs-bootc-image-builder build \
    --image quay.io/myorg/myos:stable \
    --type raw \
    --rootfs btrfs \
    --out ./output

# With user/SSH injection via config.toml
bdfs-bootc-image-builder build \
    --image quay.io/myorg/myos:stable \
    --type qcow2 \
    --config ./config.toml \
    --out ./output

# AMI for AWS EC2
AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... \
bdfs-bootc-image-builder build \
    --image quay.io/myorg/myos:stable \
    --type ami \
    --out ./output
```

### Full pipeline: bdfs workspace → disk image

```bash
# 1. Create a workspace from the live bootc root
bdfs-bootc workspace --name my-experiment

# 2. Work in the workspace
bdfs dev shell my-experiment

# 3. Build a disk image directly from the workspace (commit + build in one step)
bdfs-bootc-image-builder from-workspace my-experiment \
    --image quay.io/myorg/myos:dev \
    --type qcow2 \
    --rootfs btrfs \
    --out ./output
```

### List supported image types

```bash
bdfs-bootc-image-builder list-types
```

### Show recent builds

```bash
bdfs-bootc-image-builder status
bdfs-bootc-image-builder status --out /var/lib/bdfs/images
```

## config.toml

bootc-image-builder uses a TOML file to inject users and SSH keys into the
built image. Without it, the image has no default user.

```toml
[[customizations.user]]
name = "admin"
password = "$6$..."          # hashed — use: openssl passwd -6
key = "ssh-ed25519 AAAA..."  # SSH public key
groups = ["wheel"]
```

Place at `./config.toml` (auto-detected) or pass `--config PATH`.

## Rootless mode

Rootless builds use KVM instead of full root privileges. Experimental.

```bash
# Enable in config
BDFS_BIB_ROOTLESS=true

# Or per-invocation
BDFS_BIB_ROOTLESS=true bdfs-bootc-image-builder build \
    --image quay.io/myorg/myos:stable \
    --type qcow2
```

Requires `/dev/kvm` accessible to the current user:
```bash
sudo usermod -aG kvm "$USER"   # then log out and back in
```

## Dependencies

- `podman` — runs the bootc-image-builder container
- `bootc-image-builder` — pulled automatically via podman on first run
- `bdfs-bootc` — required for `from-workspace` only (see [bootc integration](../bootc/README.md))
- `aws` CLI — required for `push-ami` only

## Relationship to the bootc integration

The [bootc integration](../bootc/README.md) handles the *workspace/snapshot*
side: creating writable overlays on a live bootc root, committing changes back
to OCI, and exporting to DwarFS. This integration handles the *disk image
production* side: taking any bootc OCI image and producing deployable disk
images for VMs, cloud providers, or bare metal.

Use them together for the full round-trip, or independently — `build` works
with any bootc-compatible OCI image regardless of whether it was produced by
bdfs.

## Supported image types

| Type | Format | Use case |
|---|---|---|
| `qcow2` | QEMU | KVM/QEMU virtualisation (default) |
| `raw` | Raw disk | Bare-metal, custom partitioning |
| `vmdk` | VMware | vSphere, VMware Workstation/Fusion |
| `vhd` | VHD | Hyper-V, Azure, Virtual PC |
| `ami` | AWS AMI | Amazon EC2 |
| `anaconda-iso` | ISO | Unattended installer for physical machines |
| `gce` | GCE | Google Compute Engine |
