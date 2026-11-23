#!/bin/sh
# Runs the agent against a fake TiG Notary and the keys it publishes: signing at each sync, the signatures going to the
# remote, a notary that is down, rate limiting or ahead of the device's clock, tags the agent must not store, and a
# notary key that is rotated.
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

notary=$work/notary
mkdir "$notary"
ssh-keygen -q -t ed25519 -N '' -C '' -f "$notary/key"
ssh-keygen -q -t ed25519 -N '' -C '' -f "$notary/other_key"
ssh-keygen -q -t ed25519 -N '' -C '' -f "$notary/next_key"

# the issuer publishes a key as allowed signers do, with a date it is valid from
publish() { # key...
  : >"$notary/published"
  for key in "$@"; do
    printf 'tigdb.org namespaces="git",valid-after="20200101" %s\n' "$(cut -d' ' -f1,2 "$notary/$key.pub")" >>"$notary/published"
  done
}
mode() { printf '%s\n' "$1" >"$notary/mode"; }
publish key
mode ok

python3 "$here/fake_notary.py" "$work/port" "$notary" &
server=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$work/port" ] && break; sleep 0.5; done
[ -s "$work/port" ] || fail "the fake notary did not start"
url=http://127.0.0.1:$(cat "$work/port")

git init -q --bare "$work/remote.git"
cat >"$work/agent.conf" <<CONF
data_dir = $work/data
remote = $work/remote.git
notary = $url
issuer_keys = $url/.well-known/tig-public-key
CONF
run() { "$agent" --config "$work/agent.conf" "$@"; }
repository=$work/data/repository
tags() { git -C "$1" for-each-ref --format='%(refname)' refs/tags/tig/sig | wc -l | tr -d ' '; }
signed() { git -C "$repository" for-each-ref --points-at HEAD --format='%(refname)' refs/tags/tig/sig | wc -l | tr -d ' '; }

# a new device is signed at its first sync, and the signature goes to the remote with the commits
run init >/dev/null
run collect
run push 2>"$work/push.err"
[ "$(tags "$repository")" -eq 1 ] || fail "expected one signature, found $(tags "$repository")"
[ "$(signed)" -eq 1 ] || fail "the latest commit is not signed"
[ "$(tags "$work/remote.git")" -eq 1 ] || fail "the signature was not pushed"
[ "$(git -C "$work/remote.git" rev-parse main)" = "$(git -C "$repository" rev-parse HEAD)" ] || fail "remote is not at HEAD"
run status | grep -q '^sign result *ok$' || fail "status does not show the signature"
run status | grep -q "^notary *$url (issuer tigdb.org)$" || fail "status does not show the notary"
grep -q 'tigdb.org signed' "$work/push.err" || fail "the signature was not logged"
grep -q ' tig-agent/[0-9]' "$notary/requests.log" || fail "the request did not say which agent made it"
[ "$(wc -l <"$notary/requests.log" | tr -d ' ')" -eq 1 ] || fail "expected one request"

# anyone with the repository can check it with Git, the issuer's published keys and nothing else
verify() { # tag...
  for tag in "$@"; do
    git -C "$work/remote.git" -c gpg.ssh.allowedSignersFile="$notary/published" verify-tag "$tag" 2>/dev/null ||
      fail "$tag does not verify"
  done
}
tag=$(git -C "$work/remote.git" for-each-ref --format='%(refname:short)' refs/tags/tig/sig)
verify "$tag"
[ "$(git -C "$work/remote.git" rev-parse "$tag^{commit}")" = "$(git -C "$repository" rev-parse HEAD)" ] ||
  fail "the signature is not of the latest commit"
git -C "$work/remote.git" cat-file -p "$tag" | grep -qx "Device: $(ssh-keygen -l -E sha256 -f "$work/data/keys/device_ed25519.pub" | cut -d' ' -f2)" ||
  fail "the signature is not for the device key"

# nothing new to sign: the notary isn't asked
run push 2>/dev/null
run status | grep -q '^sign result *current$' || fail "status does not show the commit was already signed"
[ "$(wc -l <"$notary/requests.log" | tr -d ' ')" -eq 1 ] || fail "an unchanged repository was signed again"

# a later commit gets its own signature, and the earlier one stays
sleep 1
run collect
run push 2>/dev/null
[ "$(tags "$repository")" -eq 2 ] || fail "expected two signatures, found $(tags "$repository")"
[ "$(tags "$work/remote.git")" -eq 2 ] || fail "the second signature was not pushed"
verify $(git -C "$work/remote.git" for-each-ref --format='%(refname:short)' refs/tags/tig/sig)

# with the notary down, committing and pushing carry on; one signature covers everything made meanwhile
mode down
sleep 1
run collect
run push 2>"$work/down.err" || fail "the push failed because the notary was down"
grep -q 'the notary returned 503' "$work/down.err" || fail "the notary being down was not reported"
[ "$(git -C "$work/remote.git" rev-parse main)" = "$(git -C "$repository" rev-parse HEAD)" ] || fail "remote is not at HEAD"
[ "$(tags "$repository")" -eq 2 ] || fail "a signature appeared without a notary"
[ "$(signed)" -eq 0 ] || fail "the latest commit is signed"
run status | grep -q '^sign result *failed$' || fail "status does not show the failed signature"
sleep 1
run collect
mode ok
run push 2>/dev/null
[ "$(tags "$repository")" -eq 3 ] || fail "the backlog was not signed once the notary was back"
[ "$(signed)" -eq 1 ] || fail "the latest commit is not signed"

# rate limiting is reported, and not retried at once
mode rate
sleep 1
run collect
run push 2>"$work/rate.err"
grep -q 'rate limiting.*retry after 60' "$work/rate.err" || fail "rate limiting was not reported"
[ "$(tags "$repository")" -eq 3 ] || fail "a signature appeared when the notary was rate limiting"

# the notary is an hour ahead of the device: it says so, and the agent asks again with the notary's time
mode ok
printf '3600\n' >"$notary/offset"
run push 2>"$work/skew.err"
grep -q 'asking again with its time' "$work/skew.err" || fail "a clock that is out was not corrected"
[ "$(tags "$repository")" -eq 4 ] || fail "a device with a clock that is out was not signed"
verify $(git -C "$work/remote.git" for-each-ref --format='%(refname:short)' refs/tags/tig/sig)
rm "$notary/offset"

# a tag the agent must not store: for another issuer, or signed by a key that isn't published
for bad in wrong-issuer unknown-key; do
  mode $bad
  sleep 1
  run collect
  run push 2>"$work/$bad.err" || fail "the push failed because of a $bad tag"
  [ "$(tags "$repository")" -eq 4 ] || fail "a $bad tag was stored"
  run status | grep -q '^sign result *failed$' || fail "status does not show the $bad tag was refused"
  [ "$(git -C "$work/remote.git" rev-parse main)" = "$(git -C "$repository" rev-parse HEAD)" ] || fail "remote is not at HEAD"
done
grep -q 'is not a signature of' "$work/wrong-issuer.err" || fail "a tag from another issuer was not refused"
grep -q 'does not verify' "$work/unknown-key.err" || fail "a tag from an unknown key was not refused"

# the notary changes key: the copy of the published keys the agent kept doesn't list it, so it fetches them again
cp "$notary/key" "$notary/old_key"
cp "$notary/key.pub" "$notary/old_key.pub"
cp "$notary/next_key" "$notary/key"
cp "$notary/next_key.pub" "$notary/key.pub"
publish old_key next_key
mode ok
grep -q "$(cut -d' ' -f2 "$notary/old_key.pub")" "$work/data/notary/tig-public-key" || fail "the first key was not kept"
grep -q "$(cut -d' ' -f2 "$notary/next_key.pub")" "$work/data/notary/tig-public-key" && fail "the new key was already known"
sleep 1
run push 2>/dev/null
[ "$(tags "$repository")" -eq 5 ] || fail "a signature by the new key was not stored"
grep -q "$(cut -d' ' -f2 "$notary/next_key.pub")" "$work/data/notary/tig-public-key" || fail "the new key was not kept"
verify $(git -C "$work/remote.git" for-each-ref --format='%(refname:short)' refs/tags/tig/sig)

# the published file only counts for its issuer: a key listed for another principal can't sign for it
printf 'attacker.example namespaces="git" %s\n' "$(cut -d' ' -f1,2 "$notary/other_key.pub")" >>"$notary/published"
mode unknown-key
sleep 1
run collect
run push 2>/dev/null
[ "$(tags "$repository")" -eq 5 ] || fail "a key published for another principal signed for the issuer"
grep -q "$(cut -d' ' -f2 "$notary/other_key.pub")" "$work/data/notary/tig-public-key" && fail "a key for another principal was kept"

# without a notary configured nothing is requested and nothing is asked of the network
printf 'data_dir = %s\nremote = %s\n' "$work/plain" "$work/remote-plain.git" >"$work/plain.conf"
git init -q --bare "$work/remote-plain.git"
before=$(wc -l <"$notary/requests.log" | tr -d ' ')
"$agent" --config "$work/plain.conf" init >/dev/null
"$agent" --config "$work/plain.conf" collect
"$agent" --config "$work/plain.conf" push
[ "$(tags "$work/plain/repository")" -eq 0 ] || fail "a repository with no notary was signed"
[ "$(wc -l <"$notary/requests.log" | tr -d ' ')" -eq "$before" ] || fail "a notary was asked without being configured"
"$agent" --config "$work/plain.conf" status | grep -q '^notary' && fail "status shows a notary that isn't configured"

echo "end to end notary: ok"
