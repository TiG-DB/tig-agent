#!/bin/sh
# Runs the updater against a fake curl: a GITHUB_TOKEN that GitHub refuses is dropped and the check carries on, a token
# that is accepted is used, and a refusal that isn't about the token stops the run and says what GitHub answered.
set -eu

updater=$1
command -v jq >/dev/null 2>&1 || { echo "skipped: no jq"; exit 77; }
command -v bash >/dev/null 2>&1 || { echo "skipped: no bash"; exit 77; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

bin=$work/bin
mkdir "$bin"

# answers as FAKE_MODE says, and records whether each request carried a token
cat >"$bin/curl" <<'EOF'
#!/bin/sh
out=/dev/stdout
fmt=
fail=no
auth=no
url=
while [ $# -gt 0 ]; do
  case $1 in
    --output | -o) out=$2; shift ;;
    --write-out) fmt=$2; shift ;;
    --max-time) shift ;;
    --fail) fail=yes ;;
    -H) case $2 in Authorization:*) auth=yes ;; esac; shift ;;
    -*) ;;
    *) url=$1 ;;
  esac
  shift
done
echo "$auth $url" >>"$FAKE_LOG"
case $FAKE_MODE:$auth in
  refuse-token:yes) status=403; body='{"message":"Resource not accessible by personal access token"}' ;;
  rate-limited:*) status=403; body='{"message":"API rate limit exceeded"}' ;;
  *) status=200; body='{"name":"0.3.0+29.abc1234","assets":[]}' ;;
esac
if [ "$fail" = yes ] && [ "$status" -ge 400 ]; then
  echo "curl: (22) The requested URL returned error: $status" >&2
  exit 22
fi
printf '%s' "$body" >"$out"
[ -z "$fmt" ] || printf '%s' "$status"
EOF

# the installed version is the latest release, so a successful lookup ends the run
cat >"$bin/dpkg-query" <<'EOF'
#!/bin/sh
printf '%s' "0.3.0+29.abc1234"
EOF

cat >"$bin/dpkg" <<'EOF'
#!/bin/sh
case $1 in
  --print-architecture) echo amd64 ;;
  --compare-versions) [ "$2" != "$4" ] ;;
esac
EOF
chmod +x "$bin/curl" "$bin/dpkg-query" "$bin/dpkg"

token=ghp_not_a_real_token

run() { # mode [token]: sets rc, and leaves the requests in curl.log and the updater's messages in err
  : >"$work/curl.log"
  if [ -n "${2:-}" ]; then export GITHUB_TOKEN=$2; else unset GITHUB_TOKEN; fi
  rc=0
  FAKE_MODE=$1 FAKE_LOG="$work/curl.log" PATH="$bin:$PATH" bash "$updater" 2>"$work/err" || rc=$?
}

requests() {
  wc -l <"$work/curl.log" | tr -d ' '
}

# an accepted token is used, and once
run ok "$token"
[ "$rc" = 0 ] || fail "an accepted token: exit $rc"
[ "$(requests)" = 1 ] || fail "an accepted token: $(requests) requests"
grep -q '^yes ' "$work/curl.log" || fail "an accepted token wasn't sent"
[ ! -s "$work/err" ] || fail "an accepted token: unexpected output: $(cat "$work/err")"

# no token is needed for a public repository
run ok
[ "$rc" = 0 ] || fail "no token: exit $rc"
grep -q '^no ' "$work/curl.log" || fail "no token: a token was sent"

# a token GitHub refuses is dropped, and says why
run refuse-token "$token"
[ "$rc" = 0 ] || fail "a refused token: exit $rc"
[ "$(requests)" = 2 ] || fail "a refused token: $(requests) requests"
[ "$(sed -n 1p "$work/curl.log" | cut -d' ' -f1)" = yes ] || fail "a refused token: the first request had no token"
[ "$(sed -n 2p "$work/curl.log" | cut -d' ' -f1)" = no ] || fail "a refused token: the retry still had a token"
grep -q 'refused GITHUB_TOKEN (403: Resource not accessible by personal access token)' "$work/err" ||
  fail "a refused token wasn't explained: $(cat "$work/err")"
! grep -q "$token" "$work/err" || fail "the token was logged"

# a refusal with no token to blame stops the run, and says what GitHub answered
run rate-limited
[ "$rc" != 0 ] || fail "a rate limit didn't stop the run"
[ "$(requests)" = 1 ] || fail "a rate limit: $(requests) requests"
grep -q 'GitHub answered 403 .*: API rate limit exceeded' "$work/err" || fail "a rate limit wasn't explained: $(cat "$work/err")"

# with a token, it is tried once more without before giving up
run rate-limited "$token"
[ "$rc" != 0 ] || fail "a rate limit with a token didn't stop the run"
[ "$(requests)" = 2 ] || fail "a rate limit with a token: $(requests) requests"
grep -q 'refused GITHUB_TOKEN' "$work/err" || fail "a rate limit with a token: no retry message"
grep -q 'GitHub answered 403 .*: API rate limit exceeded' "$work/err" || fail "a rate limit with a token wasn't explained"

echo "ok"
