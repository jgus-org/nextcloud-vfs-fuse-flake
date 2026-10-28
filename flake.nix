{
  description = "nextcloud-client carrying an experimental FUSE-backed virtual files plugin (Vfs::Fuse), and the nextcloud-vfs-fused daemon presenting that plugin's backing store with the placeholders' remote sizes.";

  inputs = {
    flake-lib.url = "github:jgus-org/flake-lib/v1";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    {
      self,
      flake-lib,
      nixpkgs,
    }:
    {
      nixConfig = ''
        filter-syscalls = false
      '';
      nixosModules.default = import ./nixos-module.nix { inherit self; };
    }
    // flake-lib.lib.eachDefaultLinuxSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        inherit (pkgs) lib;

        orgFormatting = flake-lib.lib.mkFormatting {
          inherit self system;
        };

        upstream = pkgs.nextcloud-client;

        nextcloud-client-vfs-fuse = upstream.overrideAttrs (previousAttrs: {
          checkPhase = ''
            runHook preCheck

            export HOME=$(mktemp -d)
            export XDG_RUNTIME_DIR=$(mktemp -d)
            export QT_QPA_PLATFORM=offscreen
            ctest --output-on-failure -R SyncFuse

            runHook postCheck
          '';
          doCheck = true;
          pname = "nextcloud-client-vfs-fuse";
          postPatch = ''
            install -Dm644 ${./client/vfs_fuse.h} src/libsync/vfs/fuse/vfs_fuse.h
            install -Dm644 ${./client/vfs_fuse.cpp} src/libsync/vfs/fuse/vfs_fuse.cpp
            install -Dm644 ${./client/CMakeLists.txt} src/libsync/vfs/fuse/CMakeLists.txt
            install -Dm644 ${./client/mount_config.hpp} src/libsync/vfs/fuse/mount_config.hpp
            install -Dm644 ${./daemon/protocol.hpp} src/libsync/vfs/fuse/protocol.hpp
            install -Dm644 ${./daemon/replacement_lock.hpp} src/libsync/vfs/fuse/replacement_lock.hpp
            install -Dm644 ${./daemon/unique_fd.hpp} src/libsync/vfs/fuse/unique_fd.hpp
            install -Dm644 ${./client/testsyncfuse.cpp} test/testsyncfuse.cpp
            install -Dm644 ${./client/nextcloudctl.cpp} src/cmd/nextcloudctl.cpp

            patch -p1 < ${./client/nextcloud-vfs-fuse.patch}

          ''
          + previousAttrs.postPatch;
        });

        nextcloud-vfs-fused = pkgs.stdenv.mkDerivation {
          buildInputs = [
            pkgs.fuse3
            pkgs.boost
            pkgs.qt6.qtbase
          ];
          buildPhase = ''
            runHook preBuild
            ''${CXX} -O3 -Wall -Wextra -pipe -std=c++23 -o nextcloud-vfs-fused *.cpp \
              $(pkg-config --cflags --libs fuse3 Qt6Core Qt6Network) -lboost_program_options
            runHook postBuild
          '';
          checkPhase = ''
            runHook preCheck
            STORE=$(mktemp -d)
            MOUNT=$(mktemp -d)
            touch "''${MOUNT}/occupied"
            if ./nextcloud-vfs-fused --foreground --store "''${STORE}" --mount "''${MOUNT}" >check.log 2>&1; then
              exit 1
            fi
            grep -Fx "the mount point ''${MOUNT} is not empty" check.log
            runHook postCheck
          '';
          doCheck = true;
          installPhase = ''
            runHook preInstall
            install -Dm755 nextcloud-vfs-fused "$out/bin/nextcloud-vfs-fused"
            runHook postInstall
          '';
          meta = {
            description = "FUSE daemon presenting and hydrating the Vfs::Fuse backing store";
            mainProgram = "nextcloud-vfs-fused";
            platforms = flake-lib.lib.defaultLinuxSystems;
          };
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.qt6.wrapQtAppsHook
          ];
          pname = "nextcloud-vfs-fused";
          src = ./daemon;
          version = "0.1";
        };
      in
      {
        apps =
          let
            nextcloudcmd = {
              meta.description = "Headless sync driver, the harness for exercising --vfs fuse.";
              program = lib.getExe' nextcloud-client-vfs-fuse "nextcloudcmd";
              type = "app";
            };
          in
          {
            inherit nextcloudcmd;
            default = nextcloudcmd;
            nextcloud = {
              meta.description = "The desktop client GUI built against the same patch set.";
              program = lib.getExe nextcloud-client-vfs-fuse;
              type = "app";
            };
            nextcloud-vfs-fused = {
              meta.description = "Mounts a backing store so its placeholders read at their remote sizes.";
              program = lib.getExe' nextcloud-vfs-fused "nextcloud-vfs-fused";
              type = "app";
            };
            nextcloudctl = {
              meta.description = "Controls virtual-file availability through the running Nextcloud client.";
              program = lib.getExe' nextcloud-client-vfs-fuse "nextcloudctl";
              type = "app";
            };
          };
        checks = {
          formatting = orgFormatting.check;
        }
        // lib.optionalAttrs (system == "x86_64-linux") {
          passthrough-service = pkgs.testers.nixosTest {
            name = "nextcloud-vfs-fuse-passthrough-service";

            nodes.machine = { ... }: {
              imports = [ self.nixosModules.default ];
              services.nextcloud-vfs-fuse.mounts = {
                personal = {
                  group = "nextcloud";
                  mountPoint = "/mnt/nextcloud-personal";
                  store = "/var/lib/nextcloud-vfs-fuse/personal";
                  user = "nextcloud";
                };
                work = {
                  group = "nextcloud";
                  mountPoint = "/mnt/nextcloud-work";
                  store = "/var/lib/nextcloud-vfs-fuse/work";
                  user = "nextcloud";
                };
              };
              users = {
                groups.nextcloud.gid = 1000;
                users.nextcloud = {
                  group = "nextcloud";
                  isNormalUser = true;
                  uid = 1000;
                };
              };
            };

            testScript = ''
              import shlex

              replacement_lease_probe_source = """
              import errno
              import fcntl
              import hashlib
              import os
              import sys

              store = os.path.realpath(sys.argv[1])
              expected = sys.argv[2]
              attributes = os.stat(os.path.join(store, "payload"))
              mask = (1 << 64) - 1
              lock_hash = attributes.st_dev
              lock_hash ^= (
                  attributes.st_ino
                  + 0x9E3779B97F4A7C15
                  + (lock_hash << 6)
                  + (lock_hash >> 2)
              ) & mask
              offset = lock_hash & ((1 << 63) - 1)
              digest = hashlib.sha256(store.encode()).hexdigest()
              lock_path = f"/run/user/{os.getuid()}/nextcloud-vfs-fuse-{digest}.replacement-lock"

              with open(lock_path, "r+b", buffering=0) as lease:
                  try:
                      fcntl.lockf(lease, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, offset, os.SEEK_SET)
                      acquired = True
                  except OSError as error:
                      if error.errno not in {errno.EACCES, errno.EAGAIN}:
                          raise
                      acquired = False

              expected_acquired = expected == "acquired"
              if acquired != expected_acquired:
                  raise AssertionError(f"exclusive replacement lease was {'acquired' if acquired else 'blocked'}")
              """

              def replacement_lease_probe(store, expected):
                  return (
                      "${lib.getExe' pkgs.util-linux "runuser"} -u nextcloud -- ${lib.getExe pkgs.python3} -c "
                      f"{shlex.quote(replacement_lease_probe_source)} {shlex.quote(store)} {shlex.quote(expected)}"
                  )

              start_all()
              instances = [
                  ("personal", "/var/lib/nextcloud-vfs-fuse/personal", "/mnt/nextcloud-personal"),
                  ("work", "/var/lib/nextcloud-vfs-fuse/work", "/mnt/nextcloud-work"),
              ]
              for name, store, mount in instances:
                  unit = f"nextcloud-vfs-fused-{name}.service"
                  machine.wait_for_unit(unit)
                  machine.wait_until_succeeds(f'test "$(findmnt -rn -T {mount} -o FSTYPE)" = fuse')
                  pid = machine.succeed(f"systemctl show {unit} -P MainPID").strip()
                  machine.succeed(f"grep -q '^CapEff:[[:space:]]*0000000000200000$' /proc/{pid}/status")
                  machine.succeed(f"grep -zqx 'XDG_RUNTIME_DIR=/run/user/1000' /proc/{pid}/environ")
                  machine.succeed(f"printf '{name}-passthrough-data' > {store}/payload")
                  machine.succeed(f"printf private > {store}/.sync_test.db")
                  machine.succeed(f"printf private > {store}/.sync_test.db-wal")
                  machine.succeed(f"printf visible > {store}/.ordinary-dotfile")
                  machine.succeed(f"chown nextcloud:nextcloud {store}/payload")
                  machine.succeed(f"chown nextcloud:nextcloud {store}/.sync_test.db {store}/.sync_test.db-wal {store}/.ordinary-dotfile")
                  machine.succeed(f"runuser -u nextcloud -- cmp {store}/payload {mount}/payload")
                  entries = machine.succeed(f"runuser -u nextcloud -- ${lib.getExe' pkgs.coreutils "ls"} -A {mount}").splitlines()
                  assert ".sync_test.db" not in entries
                  assert ".sync_test.db-wal" not in entries
                  assert ".ordinary-dotfile" in entries
                  machine.fail(f"runuser -u nextcloud -- ${lib.getExe' pkgs.coreutils "test"} -e {mount}/.sync_test.db")
                  machine.fail(f"runuser -u nextcloud -- ${lib.getExe' pkgs.coreutils "touch"} {mount}/.sync_created.db")
                  holder = f"nextcloud-vfs-open-{name}.service"
                  machine.succeed(
                      f"systemd-run --collect --unit={holder} --service-type=exec --uid=nextcloud "
                      f"${lib.getExe' pkgs.coreutils "tail"} -f {mount}/payload"
                  )
                  machine.wait_until_succeeds(replacement_lease_probe(store, "blocked"), timeout=10)
                  machine.succeed(f"systemctl stop {holder}")
                  machine.succeed(replacement_lease_probe(store, "acquired"))
                  machine.require_unit_state(unit, "active")
            '';
          };
        };
        formatter = orgFormatting.formatter;
        packages = {
          inherit nextcloud-client-vfs-fuse nextcloud-vfs-fused;
          default = nextcloud-client-vfs-fuse;
        };
      }
    );
}
