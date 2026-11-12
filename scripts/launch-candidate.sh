#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Start a locally installed fooyin with an isolated profile. Arguments after -- go to fooyin.

set -o errexit -o nounset -o pipefail

host_prefix=""
profile_dir=""

usage="Usage: $0 --host-prefix DIR --profile-dir DIR [-- FOOYIN_ARGUMENTS...]"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-prefix)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for $1" >&2
                echo "$usage" >&2
                exit 2
            fi
            host_prefix="$2"
            shift 2
            ;;
        --profile-dir)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for $1" >&2
                echo "$usage" >&2
                exit 2
            fi
            profile_dir="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "$usage" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$host_prefix" || -z "$profile_dir" ]]; then
    echo "$usage" >&2
    exit 2
fi

host_prefix="$(realpath "$host_prefix")"
fooyin="$host_prefix/bin/fooyin"

if [[ ! -x "$fooyin" ]]; then
    echo "No fooyin executable at $fooyin" >&2
    exit 1
fi

if [[ ! -e "$host_prefix/lib/fooyin/plugins/fyplugin_jellyfin.so" ]]; then
    echo "The Jellyfin plugin is not installed in $host_prefix/lib/fooyin/plugins" >&2
    exit 1
fi

mkdir --parents "$profile_dir"
profile_dir="$(realpath "$profile_dir")"
mkdir --parents "$profile_dir/home" "$profile_dir/config" "$profile_dir/data" "$profile_dir/cache" "$profile_dir/state"

# XDG_RUNTIME_DIR and the session bus stay unchanged, so the desktop secret service stores the session token.
export HOME="$profile_dir/home"
export XDG_CONFIG_HOME="$profile_dir/config"
export XDG_DATA_HOME="$profile_dir/data"
export XDG_CACHE_HOME="$profile_dir/cache"
export XDG_STATE_HOME="$profile_dir/state"
script_root="$(realpath "$(dirname "$0")/..")"
repository_root="$script_root"
if git_common_dir="$(git -C "$script_root" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"; then
    repository_root="$(dirname "$git_common_dir")"
fi
profile_key="$(printf '%s' "$profile_dir" | sha256sum | cut --characters=1-16)"
export TMPDIR="$repository_root/tmp/ipc-$profile_key"
if [[ ${#TMPDIR} -gt 65 ]]; then
    echo "The repository path is too long for an isolated local IPC namespace" >&2
    exit 1
fi
mkdir --parents "$TMPDIR"
chmod 700 "$TMPDIR"
unset FOOYIN_PLUGIN_PATH

exec "$fooyin" "$@"
