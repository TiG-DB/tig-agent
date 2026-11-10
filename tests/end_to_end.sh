#!/bin/sh
# Runs the agent against a local bare repository: init, collect, push, then a remote that has diverged.
set -eu

agent=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

git init -q --bare "$work/remote.git"
cat >"$work/agent.conf" <<EOF
data_dir = $work/data
remote = $work/remote.git
EOF
run() {
  "$agent" --config "$work/agent.conf" "$@"
}

run init >"$work/init.out"
grep -q '^deploy key *ssh-ed25519 ' "$work/init.out" || fail "init did not show the deploy key"
[ "$(git -C "$work/data/repository" remote get-url origin)" = "$work/remote.git" ] || fail "origin not configured"

# init records the device: initial state with only the entity, a discovery of its components, and the boot in progress
repository=$work/data/repository
grep -qx 'kind: device' "$repository/state/initial.yaml" || fail "initial state is not a device"
grep -qx 'vocabulary: tig-device/1' "$repository/state/initial.yaml" || fail "initial state has no vocabulary"
grep -q '^  entity:' "$repository/state/initial.yaml" || fail "initial state has no entity"
[ "$(grep -c '^  [a-z]' "$repository/state/initial.yaml")" -eq 1 ] || fail "initial state holds more than the entity"
discoveries=$(grep -lx 'type: discovery' "$repository"/events/*/*.yaml | wc -l)
[ "$discoveries" -eq 1 ] || fail "expected one discovery event, found $discoveries"
discovery=$(grep -lx 'type: discovery' "$repository"/events/*/*.yaml)
grep -qx '  - type: discovered' "$discovery" || fail "the discovery has no discovered action"
grep -qx 'source: tig-agent [0-9].*' "$discovery" || fail "the discovery has no source"
if [ -r /etc/os-release ]; then
  grep -qx '      - add: os' "$discovery" || fail "the discovery did not find the operating system"
  grep -A1 -x '      - add: os' "$discovery" | grep -qE '^        since: ' || fail "the os was added without since"
fi
grep -q '^last compare  [0-9]' "$work/init.out" || fail "init did not compare the device with its state"
git -C "$repository" status --porcelain -- events state | grep -q . && fail "init left events uncommitted"

boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
boot_events() {
  grep -lx "    boot: $boot_id" "$repository"/events/*/*.yaml 2>/dev/null | wc -l
}
if [ -n "$boot_id" ]; then
  [ "$(boot_events)" -eq 1 ] || fail "expected one event for boot $boot_id, found $(boot_events)"
  boot=$(grep -lx "    boot: $boot_id" "$repository"/events/*/*.yaml)
  grep -qx 'type: boot' "$boot" || fail "the boot event has no type"
  # it happened before the discovery, so it changes nothing
  grep -q 'changes:' "$boot" && fail "the boot in progress at install should have no changes"
fi

run collect
run push
git -C "$work/remote.git" log --format=%s main | grep -qx 'Record telemetry' || fail "telemetry commit not pushed"
git -C "$work/remote.git" log --format=%s main | grep -qx 'Record events' || fail "events commit not pushed"
pushed=$(git -C "$work/remote.git" ls-tree -r --name-only main)
echo "$pushed" | grep -qx 'telemetry/system.load/stream.yaml' || fail "system.load stream not pushed"
echo "$pushed" | grep -qx 'telemetry/filesystem.root/stream.yaml' || fail "filesystem.root stream not pushed"
echo "$pushed" | grep -qE '^telemetry/(system\.uptime|disk\.root)/' && fail "old streams are still recorded"
echo "$pushed" | grep -qx 'state/initial.yaml' || fail "initial state not pushed"
echo "$pushed" | grep -qx "events/.*/${discovery##*/}" || fail "discovery not pushed"
if [ -r /proc/meminfo ]; then
  grep -q '^  - name: available$' "$repository/telemetry/memory/stream.yaml" || fail "memory stream has no available column"
  grep -q 'name: total' "$repository/telemetry/memory/stream.yaml" && fail "memory stream still records the total"
fi
run status | grep -q '^push result *ok$' || fail "status does not show a successful push"

# the same boot and the same device: nothing more is recorded
events_before=$(ls "$repository"/events/*/*.yaml | wc -l)
run commit
[ "$(ls "$repository"/events/*/*.yaml | wc -l)" -eq "$events_before" ] || fail "an unchanged device recorded new events"
if [ -n "$boot_id" ]; then
  [ "$(boot_events)" -eq 1 ] || fail "the boot was recorded again"
fi

# every commit is signed by the device key, which the first commit lists in .tig/allowed_signers
signers=$work/data/repository/.tig/allowed_signers
fingerprint=$(ssh-keygen -l -E sha256 -f "$work/data/keys/device_ed25519.pub" | cut -d' ' -f2)
grep -qF "$fingerprint namespaces=\"git\" ssh-ed25519 " "$signers" || fail "allowed_signers does not list the device key"
git -C "$work/remote.git" show --format= --name-only "$(git -C "$work/remote.git" rev-list --max-parents=0 main)" |
  grep -qx .tig/allowed_signers || fail "the first commit does not add .tig/allowed_signers"
unsigned=$(git -C "$work/remote.git" -c gpg.ssh.allowedSignersFile="$signers" log --format=%G? main | grep -cvx G || true)
[ "$unsigned" -eq 0 ] || fail "$unsigned commit(s) without a good device signature"
run status | grep -qF "device        $fingerprint" || fail "status does not show the device"

# a second run appends to the same chunk and fast-forwards the remote
sleep 1
run collect
run push
[ "$(git -C "$work/remote.git" rev-parse main)" = "$(git -C "$work/data/repository" rev-parse HEAD)" ] ||
  fail "remote is not at the local HEAD"

# someone else writes to the remote: the agent must refuse to overwrite it, and say so
git clone -q -b main "$work/remote.git" "$work/other"
git -C "$work/other" -c user.name=Other -c user.email=other@example.com commit -q --allow-empty -m "Diverge"
git -C "$work/other" push -q origin HEAD:main
diverged=$(git -C "$work/remote.git" rev-parse main)

sleep 1
run collect
status=0
run push 2>"$work/push.err" || status=$?
[ "$status" -eq 3 ] || fail "expected exit status 3 for a rejected push, got $status"
grep -q 'rejected' "$work/push.err" || fail "rejected push not reported"
[ "$(git -C "$work/remote.git" rev-parse main)" = "$diverged" ] || fail "the remote was overwritten"
run status | grep -q '^push result *rejected' || fail "status does not show the rejected push"

# a repository with unsigned history, as earlier agents made, gains the device key in a signed commit
legacy=$work/legacy/repository
git init -q "$legacy"
mkdir "$legacy/telemetry" && touch "$legacy/telemetry/.gitkeep"
git -C "$legacy" add -A
git -C "$legacy" -c user.name=Old -c user.email=old@example.com commit -q -m "Create TiG repository"
printf 'data_dir = %s\n' "$work/legacy" >"$work/legacy.conf"
"$agent" --config "$work/legacy.conf" init >/dev/null
"$agent" --config "$work/legacy.conf" collect
"$agent" --config "$work/legacy.conf" commit
history=$(git -C "$legacy" -c gpg.ssh.allowedSignersFile="$legacy/.tig/allowed_signers" log --format='%G? %s' | tr '\n' '|')
[ "$history" = "G Record telemetry|G Record events|G Add device key|N Create TiG repository|" ] ||
  fail "unexpected legacy history: $history"

echo "end to end: ok"
