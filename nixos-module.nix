{ self }:
{ config, lib, pkgs, utils, ... }:
let
  cfg = config.services.nextcloud-vfs-fuse;
  mountUserUid = mount: lib.attrByPath [ mount.user "uid" ] null config.users.users;
in
{
  options.services.nextcloud-vfs-fuse = with lib.types; {
    package = lib.mkOption {
      description = "Package providing nextcloud-vfs-fused.";
      type = package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.nextcloud-vfs-fused;
    };
    mounts = lib.mkOption {
      description = "Nextcloud virtual-files FUSE mounts present on this system.";
      default = { };
      type = attrsOf (submodule {
        options = {
          user = lib.mkOption {
            description = "User that owns the backing store and FUSE mount.";
            type = str;
          };
          group = lib.mkOption {
            description = "Group that owns the backing store and FUSE mount.";
            type = str;
            default = "users";
          };
          store = lib.mkOption {
            description = "Private backing store used by the Nextcloud sync client.";
            type = str;
          };
          mountPoint = lib.mkOption {
            description = "User-facing FUSE mount point.";
            type = str;
          };
        };
      });
    };
  };

  config = lib.mkIf (cfg.mounts != { }) {
    assertions = lib.concatLists (lib.mapAttrsToList
      (name: mount:
        let
          optionPath = "services.nextcloud-vfs-fuse.mounts.${name}";
        in
        [
          {
            assertion = builtins.match "[A-Za-z0-9_-]+" name != null;
            message = "${optionPath} has an invalid systemd service name suffix";
          }
          {
            assertion = lib.hasPrefix "/" mount.store;
            message = "${optionPath}.store must be an absolute path";
          }
          {
            assertion = lib.hasPrefix "/" mount.mountPoint;
            message = "${optionPath}.mountPoint must be an absolute path";
          }
          {
            assertion = mount.store != mount.mountPoint;
            message = "${optionPath}.store and mountPoint must differ";
          }
          {
            assertion = builtins.hasAttr mount.user config.users.users;
            message = "${optionPath}.user does not name a configured user";
          }
          {
            assertion = mountUserUid mount != null;
            message = "${optionPath}.user has no fixed uid";
          }
          {
            assertion = builtins.hasAttr mount.group config.users.groups;
            message = "${optionPath}.group does not name a configured group";
          }
        ])
      cfg.mounts);

    programs.fuse.enable = true;

    systemd.services = lib.mapAttrs'
      (name: mount:
        let
          uid = toString (mountUserUid mount);
          runtimeDirectoryUnit = "user-runtime-dir@${uid}.service";
        in
        lib.nameValuePair "nextcloud-vfs-fused-${name}" {
          description = "Nextcloud virtual-files FUSE view ${name}";
          wantedBy = [ "multi-user.target" ];
          after = [
            "local-fs.target"
            runtimeDirectoryUnit
          ];
          requires = [ runtimeDirectoryUnit ];
          unitConfig = {
            RequiresMountsFor = [ mount.store ];
            StartLimitBurst = 3;
            StartLimitIntervalSec = 60;
          };
          serviceConfig = {
            Type = "simple";
            User = mount.user;
            Group = mount.group;
            Environment = "XDG_RUNTIME_DIR=/run/user/${uid}";
            ExecStartPre = "+${utils.escapeSystemdExecArgs [
              (lib.getExe' pkgs.coreutils "install")
              "-d"
              "-m"
              "0700"
              "-o"
              mount.user
              "-g"
              mount.group
              mount.store
              mount.mountPoint
            ]}";
            ExecStart = utils.escapeSystemdExecArgs [
              (lib.getExe' cfg.package "nextcloud-vfs-fused")
              "--foreground"
              "--passthrough"
              "required"
              "--store"
              mount.store
              "--mount"
              mount.mountPoint
            ];
            Restart = "on-failure";
            RestartSec = 2;
            LimitNOFILE = 1048576;
            DevicePolicy = "closed";
            DeviceAllow = [ "/dev/fuse rw" ];
            LockPersonality = true;
            MemoryDenyWriteExecute = true;
            NoNewPrivileges = true;
            RestrictAddressFamilies = [ "AF_UNIX" ];
            RestrictNamespaces = true;
            RestrictRealtime = true;
            RestrictSUIDSGID = true;
            AmbientCapabilities = [ "CAP_SYS_ADMIN" ];
            CapabilityBoundingSet = [ "CAP_SYS_ADMIN" ];
          };
        })
      cfg.mounts;
  };
}
