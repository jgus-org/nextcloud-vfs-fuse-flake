let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/6510408d147d8c5e6d76043e899e9251756f6a75.tar.gz";
  }) {
    config = { };
  };
in
pkgs.runCommand "xattr-probe"
  {
    nativeBuildInputs = [ pkgs.attr ];
  }
  ''
    set +e
    echo "== kernel =="
    uname -r
    echo "== sandbox mount table =="
    cat /proc/mounts
    echo "== sandbox fstype =="
    stat -f -c '%T %n' /build /tmp "$TMPDIR" /nix/store
    echo "== sandbox xattr probe =="
    probe() {
      d="$1"
      kind="$2"
      if [ "$kind" = dir ]; then
        t="$d/xattr-probe-dir"
        mkdir -p "$t"
      else
        t="$d/xattr-probe-file"
        : > "$t"
      fi
      for a in user.nextcloud.vfs_fuse.probe user.test security.test; do
        msg=$(setfattr -n "$a" -v probe "$t" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
          echo "PROBE $d $kind $a OK"
        else
          echo "PROBE $d $kind $a rc=$rc $msg"
        fi
      done
      msg=$(setfattr -x user.nextcloud.vfs_fuse.probe "$t" 2>&1)
      echo "PROBE $d $kind removexattr rc=$? $msg"
    }
    probe /build file
    probe /build dir
    probe /tmp file
    touch "$out"
  ''
