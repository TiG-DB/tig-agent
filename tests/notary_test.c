/* The parts of the notary client that need no notary: what makes a returned tag acceptable, and the checks on what is
   configured. end_to_end_notary.sh runs the whole exchange. */
#include "agent.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

static int failures;

#define COMMIT "9fceb02d0ae598e95dc970b74767f19372d61af8"
#define DEVICE "SHA256:uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s"
#define SIGNATURE "-----BEGIN SSH SIGNATURE-----\nAAAA\n-----END SSH SIGNATURE-----\n"

/* 1792504800 is 2026-10-20T14:00:00Z */
#define HEADERS "object " COMMIT "\ntype commit\ntag tig/sig/20261020T140000Z\ntagger TiG Notary <notary@example.com> 1792504800 +0000\n\n"
#define MESSAGE "TiG-Signature: 1\nIssuer: example.com\nDevice: " DEVICE "\n"

static int check(const char *tag) {
  char name[64];

  return agent_notary_check_tag(tag, COMMIT, "example.com", DEVICE, name, sizeof(name));
}

static void test_tag(void) {
  char name[64];

  CHECK(agent_notary_check_tag(HEADERS MESSAGE SIGNATURE, COMMIT, "example.com", DEVICE, name, sizeof(name)) == 0);
  CHECK(strcmp(name, "tig/sig/20261020T140000Z") == 0);

  /* lines the client doesn't know are signed and ignored, before the signature */
  CHECK(check(HEADERS "Note: hello\n" MESSAGE "Extra: 1\n" SIGNATURE) == 0);

  /* a tag of another commit, another issuer or another device */
  CHECK(agent_notary_check_tag(HEADERS MESSAGE SIGNATURE, "0000000000000000000000000000000000000000", "example.com", DEVICE,
                               name, sizeof(name)) != 0);
  CHECK(agent_notary_check_tag(HEADERS MESSAGE SIGNATURE, COMMIT, "example.org", DEVICE, name, sizeof(name)) != 0);
  CHECK(agent_notary_check_tag(HEADERS MESSAGE SIGNATURE, COMMIT, "example.com", "SHA256:other", name, sizeof(name)) != 0);
  CHECK(agent_notary_check_tag(HEADERS MESSAGE SIGNATURE, COMMIT, "example.com", DEVICE, name, 10) != 0);

  /* the name must be the tagger's time: the notary's clock, not the device's */
  CHECK(check("object " COMMIT "\ntype commit\ntag tig/sig/20261020T140001Z\n"
              "tagger TiG Notary <notary@example.com> 1792504800 +0000\n\n" MESSAGE SIGNATURE) != 0);
  CHECK(check("object " COMMIT "\ntype commit\ntag tig/sig/20261020T140000Z\n"
              "tagger TiG Notary <notary@example.com> 1792504800 +0100\n\n" MESSAGE SIGNATURE) != 0);
  CHECK(check("object " COMMIT "\ntype commit\ntag tig/sig/20261020T140000Z\n"
              "tagger TiG Notary <notary@example.com> soon +0000\n\n" MESSAGE SIGNATURE) != 0);
  CHECK(check("object " COMMIT "\ntype commit\ntag other/20261020T140000Z\n"
              "tagger TiG Notary <notary@example.com> 1792504800 +0000\n\n" MESSAGE SIGNATURE) != 0);
  CHECK(check("object " COMMIT "\ntype commit\ntag tig/sig/../../heads/main\n"
              "tagger TiG Notary <notary@example.com> 1792504800 +0000\n\n" MESSAGE SIGNATURE) != 0);

  /* the tag must point at a commit, and have the version, issuer and device in its signed message */
  CHECK(check("object " COMMIT "\ntype tree\ntag tig/sig/20261020T140000Z\n"
              "tagger TiG Notary <notary@example.com> 1792504800 +0000\n\n" MESSAGE SIGNATURE) != 0);
  CHECK(check(HEADERS "Issuer: example.com\nDevice: " DEVICE "\n" SIGNATURE) != 0);
  CHECK(check(HEADERS "TiG-Signature: 2\nIssuer: example.com\nDevice: " DEVICE "\n" SIGNATURE) != 0);
  CHECK(check(HEADERS "TiG-Signature: 1\nDevice: " DEVICE "\n" SIGNATURE) != 0);
  CHECK(check(HEADERS "TiG-Signature: 1\nIssuer: example.com\n" SIGNATURE) != 0);

  /* an issuer or device only counts if it is the first, as a verifier reads it */
  CHECK(check(HEADERS "TiG-Signature: 1\nIssuer: example.org\nIssuer: example.com\nDevice: " DEVICE "\n" SIGNATURE) != 0);
  /* an issuer that only starts like the right one is another issuer */
  CHECK(check(HEADERS "TiG-Signature: 1\nIssuer: example.com.evil.org\nDevice: " DEVICE "\n" SIGNATURE) != 0);

  /* no signature, a cut-off one, or text after it */
  CHECK(check(HEADERS MESSAGE) != 0);
  CHECK(check(HEADERS MESSAGE "-----BEGIN SSH SIGNATURE-----\nAAAA\n") != 0);
  CHECK(check(HEADERS MESSAGE SIGNATURE "more\n") != 0);
  CHECK(check(HEADERS MESSAGE "-----BEGIN SSH SIGNATURE-----\nAAAA\n-----END SSH SIGNATURE-----") != 0);
  CHECK(check("") != 0);
  CHECK(check("object " COMMIT) != 0);
  CHECK(check(HEADERS "TiG-Signature: 1\r\nIssuer: example.com\r\nDevice: " DEVICE "\r\n" SIGNATURE) != 0);
}

static void test_http_date(void) {
  CHECK(agent_http_date("Tue, 20 Oct 2026 14:00:00 GMT") == 1792504800);
  CHECK(agent_http_date("  Thu, 01 Jan 1970 00:00:00 GMT") == 0);
  CHECK(agent_http_date("Sat, 29 Feb 2028 23:59:59 GMT") == 1835481599);

  CHECK(agent_http_date("") == -1);
  CHECK(agent_http_date("2026-10-20T14:00:00Z") == -1);
  CHECK(agent_http_date("Tue, 20 Oct 2026 14:00:00 PST") == -1);
  CHECK(agent_http_date("Tue, 20 Foo 2026 14:00:00 GMT") == -1);
  CHECK(agent_http_date("Tue, 20 Oct 2026 24:00:00 GMT") == -1);
  CHECK(agent_http_date("Tue, 20 Oct 1969 14:00:00 GMT") == -1);
}

static void test_urls(void) {
  CHECK(agent_notary_url_ok("https://notary.tigdb.org"));
  CHECK(agent_notary_url_ok("https://notary.example.com:8443/tig/"));
  CHECK(agent_notary_url_ok("http://127.0.0.1:3030"));
  CHECK(agent_notary_url_ok("http://localhost/"));
  CHECK(agent_notary_url_ok("http://[::1]:3030"));

  /* plain HTTP is for this machine only */
  CHECK(!agent_notary_url_ok("http://notary.tigdb.org"));
  CHECK(!agent_notary_url_ok("http://localhost.evil.org"));
  CHECK(!agent_notary_url_ok("http://127.0.0.1.evil.org"));
  CHECK(!agent_notary_url_ok("http://127.0.0.10"));
  CHECK(!agent_notary_url_ok("ftp://notary.tigdb.org"));
  CHECK(!agent_notary_url_ok("notary.tigdb.org"));
  CHECK(!agent_notary_url_ok("https://"));
  CHECK(!agent_notary_url_ok("https:///v1"));
  CHECK(!agent_notary_url_ok("https://user:secret@notary.tigdb.org"));
  CHECK(!agent_notary_url_ok("https://notary.tigdb.org/a b"));
  CHECK(!agent_notary_url_ok("https://notary.tigdb.org/'"));
  CHECK(!agent_notary_url_ok(""));
}

static void test_hostnames(void) {
  CHECK(agent_hostname_ok("tigdb.org"));
  CHECK(agent_hostname_ok("notary-1.example.com"));
  CHECK(agent_hostname_ok("localhost"));

  CHECK(!agent_hostname_ok(""));
  CHECK(!agent_hostname_ok(".example.com"));
  CHECK(!agent_hostname_ok("example.com."));
  CHECK(!agent_hostname_ok("example..com"));
  CHECK(!agent_hostname_ok("-example.com"));
  CHECK(!agent_hostname_ok("example.com/keys"));
  CHECK(!agent_hostname_ok("example.com:8080"));
  CHECK(!agent_hostname_ok("exa mple.com"));
  CHECK(!agent_hostname_ok("user@example.com"));
}

int main(void) {
  test_tag();
  test_http_date();
  test_urls();
  test_hostnames();
  return failures == 0 ? 0 : 1;
}
