#!/usr/bin/env bash
# Install a prebuilt companion locally. Activation is an explicit operator step.
set -euo pipefail
umask 077

fail() { printf 'controller-audio install: %s\n' "$*" >&2; exit 1; }
usage() {
    printf '%s\n' 'Usage: install-user.sh --binary PREBUILT --prefix FRESH_ABSOLUTE_DIR [--config-dir PRIVATE_ABSOLUTE_DIR] [--unit NAME.service]'
}
binary_source= prefix= config_dir= unit_name=kyty-controller-audio-v3.service
while (($#)); do
    case "$1" in
        --help) usage; exit 0 ;;
        --binary|--prefix|--config-dir|--unit)
            (($# >= 2)) || fail "$1 requires a value"
            case "$1" in
                --binary) binary_source=$2 ;;
                --prefix) prefix=$2 ;;
                --config-dir) config_dir=$2 ;;
                --unit) unit_name=$2 ;;
            esac
            shift 2 ;;
        *) usage >&2; fail "unknown option" ;;
    esac
done
[[ -n $binary_source && -n $prefix ]] || { usage >&2; exit 1; }
[[ -f $binary_source && -x $binary_source ]] || fail 'prebuilt binary must be an executable file'
[[ $unit_name =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*\.service$ ]] || fail 'invalid user unit name'
[[ -n $config_dir ]] || config_dir=$prefix/config
config_home=${XDG_CONFIG_HOME:-${HOME:?HOME required}/.config}
for path in "$prefix" "$config_dir" "$config_home"; do
    [[ $path == /* && $path != / && $path != *$'\n'* && $path != *$'\r'* ]] || fail 'paths must be absolute and contain no line breaks'
done
owner_uid=$(id -u)
private_dir() {
    local path=$1
    [[ ! -L $path ]] || fail 'private directory must not be a symlink'
    [[ -e $path ]] || mkdir -p -m 0700 -- "$path"
    [[ -d $path && $(stat -c '%u %a' -- "$path") == "$owner_uid 700" ]] || fail 'private directory must be owned by this user with mode0700'
}
private_file() {
    [[ -f $1 && ! -L $1 && $(stat -c '%u %a' -- "$1") == "$owner_uid 600" ]] || fail 'existing install metadata must be a private regular file'
}
private_dir "$prefix"
marker=$prefix/.controller-audio-install
if [[ -e $marker || -L $marker ]]; then
    private_file "$marker"
    [[ $(<"$marker") == controller-audio-user-install-v1 ]] || fail 'unrecognized installation prefix'
else
    (shopt -s nullglob dotglob; entries=("$prefix"/*); ((${#entries[@]} == 0))) || fail 'prefix contains unrelated files; choose a fresh prefix'
fi
private_dir "$prefix/bin"
private_dir "$config_dir"
binary_target=$prefix/bin/controller-audio-receiver
if [[ -e $binary_target || -L $binary_target ]]; then
    [[ -f $binary_target && ! -L $binary_target && $(stat -c '%u' -- "$binary_target") == "$owner_uid" ]] || fail 'existing binary must be an owned regular file'
    cmp -s -- "$binary_source" "$binary_target" || fail 'different binary already installed; choose a fresh prefix and unit'
fi
service_dir=$config_home/systemd/user
[[ ! -L $service_dir ]] || fail 'user service directory must not be a symlink'
mkdir -p -m 0700 -- "$service_dir"
[[ -d $service_dir && $(stat -c '%u' -- "$service_dir") == "$owner_uid" ]] || fail 'user service directory must belong to this user'
unit_path=$service_dir/$unit_name
[[ ! -L $unit_path ]] || fail 'user service file must not be a symlink'
# systemd quoted arguments use C-style escaping and expand percent specifiers.
unit_quote() {
    local value=$1
    value=${value//\\/\\\\}; value=${value//\"/\\\"}; value=${value//%/%%}; value=${value//\$/\$\$}
    printf '"%s"' "$value"
}
temp_binary= temp_unit=
trap 'rm -f -- "${temp_binary:-}" "${temp_unit:-}"' EXIT
temp_unit=$(mktemp "$service_dir/.controller-audio-unit.XXXXXX")
{
    printf '%s\n' '[Unit]' 'Description=Kyty controller audio companion' 'After=network-online.target'
    condition_path=${config_dir//%/%%}/config.json
    printf 'ConditionPathExists=%s\n' "$condition_path"
    printf '\n%s\n' '[Service]' 'Type=simple' 'UMask=0077' 'NoNewPrivileges=true' 'LimitCORE=0'
    printf 'ExecCondition=%s --config %s --status\n' "$(unit_quote "$binary_target")" "$(unit_quote "$config_dir/config.json")"
    printf 'ExecStart=%s --config %s\n' "$(unit_quote "$binary_target")" "$(unit_quote "$config_dir/config.json")"
    printf '%s\n' 'Restart=on-failure' 'RestartSec=5' 'TimeoutStopSec=5' 'KillSignal=SIGTERM'
    printf '\n%s\n' '[Install]' 'WantedBy=default.target'
} > "$temp_unit"
chmod 0600 "$temp_unit"
if [[ -e $unit_path ]]; then
    private_file "$unit_path"
    cmp -s -- "$temp_unit" "$unit_path" || fail 'different unit already exists; choose a fresh unit name'
fi
if [[ ! -e $binary_target ]]; then
    temp_binary=$(mktemp "$prefix/bin/.controller-audio-binary.XXXXXX")
    install -m 0755 -- "$binary_source" "$temp_binary"
    mv -- "$temp_binary" "$binary_target"
fi
chmod 0755 "$binary_target"
if [[ ! -e $marker ]]; then
    printf '%s\n' controller-audio-user-install-v1 > "$marker"
    chmod 0600 "$marker"
fi
if [[ ! -e $unit_path ]]; then mv -- "$temp_unit" "$unit_path"; fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload || printf '%s\n' 'User manager unavailable; run systemctl --user daemon-reload when logged in.' >&2
fi
printf 'Installed binary: %s\nPrivate configuration: %s/config.json\nUser unit: %s\nActivation requires an explicit systemctl --user enable --now command.\n' "$binary_target" "$config_dir" "$unit_path"
