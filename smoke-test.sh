#!/usr/bin/env -S nix shell nixpkgs#bash nixpkgs#coreutils nixpkgs#gnugrep nixpkgs#attr nixpkgs#util-linux nixpkgs#fuse3 nixpkgs#nix --command bash

set -euo pipefail

usage() {
  printf 'Usage: NC_USER=user NC_PASSWORD=password %s STORE SERVER_URL REMOTE_PATH RELATIVE_FILE\n' "${0##*/}" >&2
}

fail() {
  printf '%s\n' "${1}" >&2
  return 1
}

if [[ ${#} -ne 4 ]]; then
  usage
  exit 2
fi

if [[ -z ${NC_USER:-} || -z ${NC_PASSWORD:-} ]]; then
  fail 'NC_USER and NC_PASSWORD must be set'
fi

STORE_INPUT=${1}
SERVER_URL=${2}
REMOTE_PATH=${3}
RELATIVE_FILE=${4}

if [[ ${RELATIVE_FILE} == /* || ${RELATIVE_FILE} == .. || ${RELATIVE_FILE} == ../* || ${RELATIVE_FILE} == */../* || ${RELATIVE_FILE} == */.. ]]; then
  fail 'RELATIVE_FILE must stay within STORE'
fi

STORE=$(realpath -e -- "${STORE_INPUT}")
if [[ ! -d ${STORE} ]]; then
  fail "STORE is not a directory: ${STORE}"
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
FLAKE="path:${SCRIPT_DIR}"
TEMP_DIR=$(mktemp -d -t nextcloud-vfs-fuse-smoke.XXXXXXXX)
MOUNT_DIR=${TEMP_DIR}/mount
CLIENT_LOG=${TEMP_DIR}/client.log
DAEMON_LOG=${TEMP_DIR}/daemon.log
CLIENT_PID=
DAEMON_PID=
mkdir "${MOUNT_DIR}"

cleanup() {
  CLEANUP_STATUS=${?}
  trap - EXIT INT TERM
  set +e
  if [[ -n ${DAEMON_PID} ]] && kill -0 "${DAEMON_PID}" 2>/dev/null; then
    kill "${DAEMON_PID}"
    wait "${DAEMON_PID}"
  fi
  if [[ $(findmnt -rn -T "${MOUNT_DIR}" -o FSTYPE) == fuse ]] && ! fusermount3 -u "${MOUNT_DIR}"; then
    CLEANUP_STATUS=1
  fi
  if [[ -n ${CLIENT_PID} ]] && kill -0 "${CLIENT_PID}" 2>/dev/null; then
    kill "${CLIENT_PID}"
    wait "${CLIENT_PID}"
  fi
  if [[ ${CLEANUP_STATUS} -ne 0 ]]; then
    if [[ -s ${CLIENT_LOG} ]]; then
      printf '%s\n' 'nextcloudcmd log:' >&2
      tail -80 "${CLIENT_LOG}" >&2
    fi
    if [[ -s ${DAEMON_LOG} ]]; then
      printf '%s\n' 'nextcloud-vfs-fused log:' >&2
      tail -80 "${DAEMON_LOG}" >&2
    fi
  fi
  rm -f -- "${CLIENT_LOG}" "${DAEMON_LOG}"
  rmdir -- "${MOUNT_DIR}" "${TEMP_DIR}" 2>/dev/null
  exit "${CLEANUP_STATUS}"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

CLIENT_OUT=$(nix --extra-experimental-features 'nix-command flakes' build "${FLAKE}#nextcloud-client-vfs-fuse" --print-out-paths)
DAEMON_OUT=$(nix --extra-experimental-features 'nix-command flakes' build "${FLAKE}#nextcloud-vfs-fused" --print-out-paths)

"${CLIENT_OUT}/bin/nextcloudcmd" \
  --non-interactive \
  --vfs fuse \
  --root-pin onlineonly \
  --download-only \
  --vfs-daemon \
  --path "${REMOTE_PATH}" \
  "${STORE}" \
  "${SERVER_URL}" >"${CLIENT_LOG}" 2>&1 &
CLIENT_PID=${!}

CLIENT_DEADLINE=$((SECONDS + 60))
until grep -qF 'Sync finished; serving virtual files' "${CLIENT_LOG}"; do
  if ! kill -0 "${CLIENT_PID}" 2>/dev/null; then
    wait "${CLIENT_PID}"
    fail 'nextcloudcmd exited before its initial sync completed'
  fi
  if ((SECONDS >= CLIENT_DEADLINE)); then
    fail 'nextcloudcmd did not finish its initial sync within 60 seconds'
  fi
  sleep 0.1
done

BACKING_FILE=$(realpath -e -- "${STORE}/${RELATIVE_FILE}")
case ${BACKING_FILE} in
  "${STORE}"/*) ;;
  *) fail 'RELATIVE_FILE resolves outside STORE' ;;
esac

REMOTE_SIZE=$(getfattr --absolute-names --only-values -n user.nextcloud.vfs_fuse "${BACKING_FILE}" 2>/dev/null) || fail 'the selected backing file is not a placeholder'
if [[ ! ${REMOTE_SIZE} =~ ^[0-9]+$ ]]; then
  fail 'the placeholder marker is not a non-negative decimal size'
fi
if [[ $(stat -c %s -- "${BACKING_FILE}") -ne 0 ]]; then
  fail 'the placeholder backing file is not empty'
fi
ORIGINAL_MTIME=$(stat -c %y -- "${BACKING_FILE}")

"${DAEMON_OUT}/bin/nextcloud-vfs-fused" \
  --foreground \
  --store "${STORE}" \
  --mount "${MOUNT_DIR}" >"${DAEMON_LOG}" 2>&1 &
DAEMON_PID=${!}

MOUNT_DEADLINE=$((SECONDS + 10))
until [[ $(findmnt -rn -T "${MOUNT_DIR}" -o FSTYPE) == fuse ]]; do
  if ! kill -0 "${DAEMON_PID}" 2>/dev/null; then
    wait "${DAEMON_PID}"
    fail 'nextcloud-vfs-fused exited before mounting'
  fi
  if ((SECONDS >= MOUNT_DEADLINE)); then
    fail 'nextcloud-vfs-fused did not mount within 10 seconds'
  fi
  sleep 0.1
done

MOUNTED_FILE=${MOUNT_DIR}/${RELATIVE_FILE}
if [[ $(stat -c %s -- "${MOUNTED_FILE}") -ne ${REMOTE_SIZE} ]]; then
  fail 'the mounted placeholder does not report its remote size'
fi

MOUNTED_HASH=$(sha256sum -- "${MOUNTED_FILE}")
MOUNTED_HASH=${MOUNTED_HASH%% *}

if getfattr --absolute-names --only-values -n user.nextcloud.vfs_fuse "${BACKING_FILE}" >/dev/null 2>&1; then
  fail 'the placeholder marker remains after hydration'
fi
if [[ $(stat -c %s -- "${BACKING_FILE}") -ne ${REMOTE_SIZE} ]]; then
  fail 'the hydrated backing file has the wrong size'
fi
if [[ $(stat -c %y -- "${BACKING_FILE}") != "${ORIGINAL_MTIME}" ]]; then
  fail 'hydration changed the backing file mtime'
fi

BACKING_HASH=$(sha256sum -- "${BACKING_FILE}")
BACKING_HASH=${BACKING_HASH%% *}
if [[ ${BACKING_HASH} != "${MOUNTED_HASH}" ]]; then
  fail 'the mounted and backing file hashes differ after hydration'
fi

printf 'PASS %s: %s bytes, sha256 %s\n' "${RELATIVE_FILE}" "${REMOTE_SIZE}" "${BACKING_HASH}"
