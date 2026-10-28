Nix flake providing **nextcloud-vfs-fuse**: a build of the [Nextcloud desktop client](https://github.com/nextcloud/desktop) carrying an experimental virtual-files backend implemented over FUSE (`--vfs fuse`), and **`nextcloud-vfs-fused`**, a FUSE daemon that presents that backend's private backing store as an ordinary directory in which unhydrated placeholders report their remote size. The client is built from nixpkgs' `nextcloud-client` with these additions; the FUSE backend, the `nextcloudctl` control tool, and the daemon are written in this repository.

The two halves have distinct jobs. The sync client owns the backing store — it talks to the server, writes placeholders, and hydrates them on request. `nextcloud-vfs-fused` only presents that store: it never contacts the server, so the mount point is what a user or another program reads while the client stays out of the way.

## Outputs

- `nixosModules.default` — the `services.nextcloud-vfs-fuse` module: one FUSE view per configured mount.
- `packages.<system>.nextcloud-client-vfs-fuse` — the patched client, installing `bin/nextcloud` (the GUI), `bin/nextcloudcmd` (headless sync driver), and `bin/nextcloudctl` (availability control against the running client). `packages.<system>.default` is this package, so `nix build .#default` builds the whole Qt client.
- `packages.<system>.nextcloud-vfs-fused` — the daemon, installing `bin/nextcloud-vfs-fused`.
- `apps.<system>.{default,nextcloud,nextcloudcmd,nextcloudctl,nextcloud-vfs-fused}` — `default` is `nextcloudcmd`.
- `checks.<system>.formatting`, `formatter` — `nixfmt`, `clang-format`, `shfmt` over this repository's own source.
- `checks.x86_64-linux.passthrough-service` — a NixOS test that boots the module with two mounts and drives the store, the mount, and the hydration lease.

Both packages are defined for `aarch64-linux` and `x86_64-linux`.

Building the client runs `SyncFuseTest`, which writes the placeholder marker attributes, and nix's
build sandbox refuses those syscalls with `ENOTSUP` whatever filesystem the build runs on.
`.github/nix.conf` asks for `filter-syscalls = false`, and the CI gate applies that fragment to every
nix call in the run — it has to reach the command that builds the packages, which is not a command
rooted in this flake and so cannot read the request from it. CI is all that file covers: a setting
nix calls restricted is honored only from a user nix trusts, so a local build whose nix does not
trust its user fails in this suite rather than passing.

```nix
# flake input
nextcloud-vfs-fuse = {
  url = "github:jgus-org/nextcloud-vfs-fuse-flake";
};
```

```nix
imports = [ nextcloud-vfs-fuse.nixosModules.default ];

services.nextcloud-vfs-fuse.mounts = {
  personal = {
    user = "nextcloud";
    group = "nextcloud";
    store = "/var/lib/nextcloud-vfs-fuse/personal";
    mountPoint = "/mnt/nextcloud-personal";
  };
};
```

The module is inert until at least one mount is configured. It enables `programs.fuse`, creates `nextcloud-vfs-fused-<name>.service` per mount, and pre-creates `store` and `mountPoint` mode `0700` owned by the mount's user and group. `services.nextcloud-vfs-fuse.package` selects the daemon package and defaults to this flake's.

## Requirements the module enforces

An invalid mount is a configuration-time assertion, not a runtime failure: `<name>` must match `[A-Za-z0-9_-]+` (it becomes a systemd unit suffix), `store` and `mountPoint` must be absolute and different, `user` must name a configured user **with a fixed `uid`**, and `group` must name a configured group (`group` defaults to `"users"`). The fixed `uid` is not cosmetic — the unit runs `user-runtime-dir@<uid>.service` and sets `XDG_RUNTIME_DIR=/run/user/<uid>`, which the daemon needs (see below).

Each unit runs as the mount's user with a deliberately narrow profile: `DeviceAllow=/dev/fuse rw` with a closed device policy, `NoNewPrivileges`, `MemoryDenyWriteExecute`, `RestrictAddressFamilies=AF_UNIX`, and `CAP_SYS_ADMIN` as the only capability in the bounding set. The mount is started with `--passthrough required`, so a read fails rather than falling back to daemon-mediated copies if the kernel data path is unavailable.

## Runtime assumptions

- **Linux only.** The daemon uses libfuse3, kernel FUSE passthrough, and `fcntl` leases; the packages exist for the two Linux hosts above.
- **The store filesystem must support user xattrs.** Placeholders are marked with `user.nextcloud.vfs_fuse` (the remote size), directories being filled with `user.nextcloud.vfs_fuse.incomplete`; the daemon reads the marker to report the size and to decide what needs hydration.
- **The client must be running for hydration.** The daemon asks the client to hydrate over a local socket named `nextcloud-vfs-fuse-<sha256 of the store's real path>`, which lives in the mount user's runtime directory. Both halves must therefore run as the same user with the same `XDG_RUNTIME_DIR`. With no client serving that socket, a read of an unhydrated placeholder fails with an I/O error after a five-second connection timeout.
- **The daemon must not be the first writer.** `store` is the client's database-and-files directory; the daemon refuses to start unless `mountPoint` exists and is empty, and refuses a mount inside the store or a store inside the mount.

## Behaviour worth knowing

In the mount, an unhydrated placeholder reports its **remote** size (`st_blocks` is 0, so it is not counted as occupying space), and reading it triggers hydration through the client. The backing store keeps the zero-length placeholder until that happens.

The sync client's own database files are hidden from the mount: names containing `.db` that start with `.sync_`, `._sync_`, or `.csync_journal.db` (case-insensitively) are not visible and cannot be opened through the view. Other dotfiles are passed through normally.

While a file is open through the mount, the daemon holds an exclusive lease on it, named `<runtime directory>/nextcloud-vfs-fuse-<digest>.replacement-lock`; the sync client will not atomically replace that file until the last open handle is closed.

The `fuse` virtual-files backend, its xattrs, and the `nextcloudctl` tool come from this repository's patch to the client, so a backing store written by this build is understood only by this build. The patched `nextcloudcmd` gains `--vfs fuse`, `--root-pin {unspecified,alwayslocal,onlineonly}`, and `--vfs-daemon` (keep serving virtual files and synchronize periodically); `nextcloud-vfs-fused --help` lists the daemon's own options.

`smoke-test.sh` runs the whole path against a real server: `NC_USER=… NC_PASSWORD=… ./smoke-test.sh <store> <server-url> <remote-path> <relative-file>` builds this checkout's client and daemon, syncs the prefix online-only, mounts the store, and checks that the chosen placeholder reads at its remote size and hydrates to matching content.
