let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/6510408d147d8c5e6d76043e899e9251756f6a75.tar.gz";
  }) {
    config = { };
  };
in
pkgs.testers.nixosTest {
  name = "kvm-xattr-probe";

  nodes.machine =
    { ... }:
    {
      environment.systemPackages = [ pkgs.attr ];
    };

  testScript = ''
    machine.wait_for_unit("multi-user.target")
    print("VM KERNEL: " + machine.succeed("uname -r"))
    print("VM FSTYPE: " + machine.succeed("stat -f -c '%T %n' / /tmp"))
    print("VM XATTR: " + machine.succeed("touch /tmp/probe && setfattr -n user.nextcloud.vfs_fuse -v 4096 /tmp/probe && getfattr --absolute-names --only-values -n user.nextcloud.vfs_fuse /tmp/probe"))
    print("VM FUSE DEV: " + machine.succeed("test -c /dev/fuse && echo present || echo absent"))
  '';
}
