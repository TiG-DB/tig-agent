#!/bin/sh
# Runs the agent against a fake Docker Engine API: Compose stacks found at install, containers upgraded and removed,
# Docker going away, and Compose stacks turned on for a repository that already has a discovery.
set -eu

agent=$1
here=$(cd "$(dirname "$0")" && pwd)
command -v python3 >/dev/null 2>&1 || { echo "skipped: no python3"; exit 77; }

work=$(mktemp -d)
server=
trap '[ -z "$server" ] || kill "$server" 2>/dev/null; rm -rf "$work"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

web=3f9a0c1d2e4b
db=7b2c5e8f9a01
scenario=$work/scenario
mkdir "$scenario"

compose() { # id service image-id created
  printf '{"Id":"%s","Names":["/x"],"Image":"%s","ImageID":"%s","Created":%s,"Labels":{"com.docker.compose.project":"Shop","com.docker.compose.service":"%s","com.docker.compose.container-number":"1","com.docker.compose.config-hash":"secret"},"State":"%s","Ports":[{"PublicPort":8080}]}' \
    "$1" "$2" "$3" "$4" "$5" "$6"
}

scenario_one() {
  rm -f "$scenario"/*
  printf '[%s,%s]\n' "$(compose $web nginx:1.27 sha256:aa11 1794336291 web running)" \
    "$(compose $db postgres:16 sha256:bb22 1794300000 db exited)" >"$scenario/list.json"
  printf '{"Id":"%s","RestartCount":3,"State":{"Status":"running","Health":{"Status":"healthy"}},"Config":{"Env":["PASSWORD=hunter2"]}}\n' $web >"$scenario/$web.inspect.json"
  printf '{"Id":"%s","RestartCount":0,"State":{"Status":"exited"}}\n' $db >"$scenario/$db.inspect.json"
  printf '{"cpu_stats":{"cpu_usage":{"total_usage":5000000000}},"memory_stats":{"usage":104857600,"stats":{"inactive_file":4194304}}}\n' >"$scenario/$web.stats.json"
}

# web pulled a new image and was recreated; the database is gone
scenario_two() {
  scenario_one
  rm "$scenario/$db.inspect.json"
  printf '[%s]\n' "$(compose $web nginx:1.27 sha256:ee55 1794475802 web running)" >"$scenario/list.json"
}

scenario_one
python3 "$here/fake_docker.py" unix "$work/docker.sock" "$scenario" &
server=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$work/docker.sock" ] && break; sleep 0.5; done
[ -S "$work/docker.sock" ] || fail "the fake Docker API did not start"

cat >"$work/agent.conf" <<CONF
data_dir = $work/data
docker_host = unix://$work/docker.sock
CONF
run() { "$agent" --config "$work/agent.conf" "$@"; }
repository=$work/data/repository
events() { cat "$repository"/events/*/*.yaml; }

# a new repository finds the stacks in its one discovery
run init >/dev/null
discovery=$(grep -lx 'type: discovery' "$repository"/events/*/*.yaml)
[ "$(echo "$discovery" | wc -l)" -eq 1 ] || fail "expected one discovery"
grep -qx '      - add: stack/shop' "$discovery" || fail "the discovery did not find the stack"
grep -qx '      - add: container/shop/web/1' "$discovery" || fail "the discovery did not find the web container"
grep -qx '      - add: container/shop/db/1' "$discovery" || fail "the discovery did not find the db container"
grep -qx '        since: "2026-11-10T08:40:00Z"' "$discovery" || fail "the stack is not there since its oldest container"
grep -q 'image: ' "$discovery" || fail "no image recorded"
events | grep -qE 'secret|hunter2|8080|PASSWORD' && fail "configuration leaked into events"

# telemetry: a stream per container, with Docker's own figures; CPU needs two readings from one process
run collect
stream=$repository/telemetry/container.shop.web.1
[ -f "$stream/stream.yaml" ] || fail "no stream for the web container"
grep -q '^  - name: restarts$' "$stream/stream.yaml" || fail "the stream has no restarts column"
row=$(tail -n 1 "$stream"/*/*.csv)
case $row in
  *,running,healthy,3,,100663296) ;;
  *) fail "unexpected reading: $row" ;;
esac
tail -n 1 "$repository"/telemetry/container.shop.db.1/*/*.csv | grep -q ',exited,,0,,$' || fail "unexpected reading for the stopped container"

# nothing has changed
before=$(ls "$repository"/events/*/*.yaml | wc -l)
run commit
[ "$(ls "$repository"/events/*/*.yaml | wc -l)" -eq "$before" ] || fail "an unchanged stack recorded new events"

# web is recreated from a new image and the database is removed
scenario_two
run commit
events | grep -qx '  - type: upgraded' || fail "no upgraded action"
events | grep -qx '      - update: container/shop/web/1' || fail "web was not updated"
events | grep -qx '  - type: removed' || fail "no removed action"
events | grep -qx '      - remove: container/shop/db/1' || fail "db was not removed"
events | grep -q '  - type: replaced' && fail "a service was taken for a replacement"
after=$(ls "$repository"/events/*/*.yaml | wc -l)
# state followed the upgrade, `created` included, so the next comparison finds nothing to do
run commit
[ "$(ls "$repository"/events/*/*.yaml | wc -l)" -eq "$after" ] || fail "an upgrade was followed by a spurious change"

# Docker goes away: the stacks stay as recorded, not removed
kill "$server"; wait "$server" 2>/dev/null || true; server=
rm -f "$work/docker.sock"
run collect 2>"$work/down.err" || fail "collecting failed while Docker was down"
run commit 2>>"$work/down.err"
[ "$(ls "$repository"/events/*/*.yaml | wc -l)" -eq "$after" ] || fail "Docker being down recorded events"
grep -q 'cannot read containers' "$work/down.err" || fail "Docker being down was not reported"

# turning the stacks on later, in a repository that has its discovery already, discovers just them
cat >"$work/other.conf" <<CONF
data_dir = $work/other
CONF
"$agent" --config "$work/other.conf" init >/dev/null
other=$work/other/repository
grep -rq 'stack/' "$other/events" && fail "stacks recorded with Docker not configured"
python3 "$here/fake_docker.py" tcp "$work/port" "$scenario" &
server=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$work/port" ] && break; sleep 0.5; done
[ -s "$work/port" ] || fail "the fake Docker API did not start on TCP"
printf 'docker_host = http://127.0.0.1:%s\n' "$(cat "$work/port")" >>"$work/other.conf"
"$agent" --config "$work/other.conf" commit
[ "$(grep -lx 'type: discovery' "$other"/events/*/*.yaml | wc -l)" -eq 2 ] || fail "expected a second discovery"
second=$(grep -lx '      - add: stack/shop' "$other"/events/*/*.yaml)
grep -qx 'type: discovery' "$second" || fail "the stack was not found by a discovery"
grep -q 'add: firmware\|add: os\|add: memory' "$second" && fail "the second discovery repeated the device's components"
grep -q 'type: installed' "$second" && fail "stacks present at the start were installed"

# Docker read with no stacks is a baseline: a stack that appears later is installed, not discovered
printf '[]\n' >"$scenario/list.json"
printf 'data_dir = %s\ndocker_host = http://127.0.0.1:%s\n' "$work/empty" "$(cat "$work/port")" >"$work/empty.conf"
"$agent" --config "$work/empty.conf" init >/dev/null
scenario_one
"$agent" --config "$work/empty.conf" commit
empty=$work/empty/repository
[ "$(grep -lx 'type: discovery' "$empty"/events/*/*.yaml | wc -l)" -eq 1 ] || fail "stacks after an empty start were discovered"
grep -qx '  - type: installed' "$empty"/events/*/*.yaml || fail "stacks after an empty start were not installed"

echo "end to end docker: ok"
