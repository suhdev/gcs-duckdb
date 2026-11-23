#!/usr/bin/env python3
"""A fake of the Google endpoints gcs_adc talks to, for tests. Python standard library only.

It serves, on one port:
  POST /token                                   OAuth2 token endpoint (refresh_token and jwt-bearer grants)
  POST /v1/token                                STS token exchange (external_account)
  POST /v1/projects/-/serviceAccounts/X:generateAccessToken   IAM Credentials impersonation
  GET  /computeMetadata/v1/instance/service-accounts/default/{token,email}   GCE metadata server
  GET  /subject_token                           a URL-sourced external_account subject token
  GET  /__control?...                           test controls (token_lifetime, revoke_all, metadata=off|on)
  GET  /__stats                                 token request counters, as JSON
  everything else                               a minimal GCS XML API (path-style): GET (+Range), HEAD, PUT,
                                                DELETE, ListObjectsV2, multipart upload

Every GCS request must carry a bearer token this server issued and that has not expired or been revoked;
otherwise it answers 401, which exercises httpfs's secret refresh.

On start it writes credential fixtures (pointing at its own port) into --dir:
  authorized_user.json, service_account.json, impersonated.json, external_file.json, external_url.json,
  external_impersonated.json, subject_token.txt, gcloud/application_default_credentials.json,
  bad_refresh_token.json
"""

import argparse
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_DIR = os.path.join(os.path.dirname(HERE), "keys")
PRIVATE_KEY = os.path.join(KEY_DIR, "test_service_account.pem")
PUBLIC_KEY = os.path.join(KEY_DIR, "test_service_account.pub.pem")

CLIENT_ID = "test-client-id.apps.googleusercontent.com"
CLIENT_SECRET = "test-client-secret"
REFRESH_TOKEN = "test-refresh-token"
SA_EMAIL = "reader@test-project.iam.gserviceaccount.com"
TARGET_SA = "target@test-project.iam.gserviceaccount.com"
SUBJECT_TOKEN = "test-subject-token"
METADATA_SA = "default@test-project.iam.gserviceaccount.com"

SHA256_DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")


def b64url_decode(data):
    return base64.urlsafe_b64decode(data + "=" * (-len(data) % 4))


def load_public_key():
    out = subprocess.check_output(["openssl", "rsa", "-pubin", "-in", PUBLIC_KEY, "-noout", "-modulus"], text=True)
    modulus = int(out.strip().split("=", 1)[1], 16)
    return modulus, 65537


def verify_rs256(signing_input, signature, modulus, exponent):
    """RSASSA-PKCS1-v1_5 verification with SHA-256"""
    k = (modulus.bit_length() + 7) // 8
    if len(signature) != k:
        return False
    em = pow(int.from_bytes(signature, "big"), exponent, modulus).to_bytes(k, "big")
    digest = hashlib.sha256(signing_input).digest()
    t = SHA256_DIGEST_INFO + digest
    expected = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return em == expected


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.tokens = {}  # token -> {"expires": float, "principal": str, "scopes": [...]}
        self.token_lifetime = 3600
        self.metadata_enabled = True
        self.stats = {}
        self.objects = {}  # (bucket, key) -> bytes
        self.uploads = {}  # upload_id -> {"bucket", "key", "parts": {n: bytes}}
        self.modulus, self.exponent = load_public_key()

    def count(self, name):
        with self.lock:
            self.stats[name] = self.stats.get(name, 0) + 1

    def issue(self, principal, scopes):
        token = "ya29.mock-" + uuid.uuid4().hex
        with self.lock:
            self.tokens[token] = {
                "expires": time.time() + self.token_lifetime,
                "principal": principal,
                "scopes": scopes,
            }
            lifetime = self.token_lifetime
        return token, lifetime

    def check(self, token):
        with self.lock:
            entry = self.tokens.get(token)
            return entry is not None and entry["expires"] > time.time()


STATE = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if os.environ.get("MOCK_GOOGLE_VERBOSE"):
            sys.stderr.write("mock_google: " + (fmt % args) + "\n")

    # ---- helpers -------------------------------------------------------------------------------------------------
    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def send(self, status, body=b"", content_type="application/json", headers=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        if content_type:
            self.send_header("Content-Type", content_type)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, status, obj, headers=None):
        self.send(status, json.dumps(obj), headers=headers)

    def oauth_error(self, status, error, description):
        self.send_json(status, {"error": error, "error_description": description})

    def bearer(self):
        auth = self.headers.get("Authorization", "")
        return auth[len("Bearer ") :] if auth.startswith("Bearer ") else None

    def token_response(self, principal, scopes):
        token, lifetime = STATE.issue(principal, scopes)
        self.send_json(200, {"access_token": token, "expires_in": lifetime, "token_type": "Bearer"})

    # ---- routing -------------------------------------------------------------------------------------------------
    def route(self):
        parsed = urllib.parse.urlsplit(self.path)
        path, query = parsed.path, urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        if path == "/__control":
            return self.control(query)
        if path == "/__stats":
            with STATE.lock:
                return self.send_json(200, dict(STATE.stats))
        if path == "/token" and self.command == "POST":
            return self.oauth_token()
        if path == "/v1/token" and self.command == "POST":
            return self.sts_token()
        match = re.match(r"^/v1/projects/-/serviceAccounts/([^/:]+):generateAccessToken$", path)
        if match and self.command == "POST":
            return self.generate_access_token(urllib.parse.unquote(match.group(1)))
        if path.startswith("/computeMetadata/"):
            return self.metadata(path, query)
        if path == "/subject_token":
            if self.headers.get("X-Test-Header") != "present":
                return self.send(403, "missing header", "text/plain")
            return self.send_json(200, {"id_token": SUBJECT_TOKEN})
        return self.gcs(path, query)

    def do_GET(self):
        self.route()

    def do_POST(self):
        self.route()

    def do_PUT(self):
        self.route()

    def do_HEAD(self):
        self.route()

    def do_DELETE(self):
        self.route()

    # ---- control -------------------------------------------------------------------------------------------------
    def control(self, query):
        with STATE.lock:
            if "token_lifetime" in query:
                STATE.token_lifetime = int(query["token_lifetime"][0])
            if "revoke_all" in query:
                STATE.tokens.clear()
            if "metadata" in query:
                STATE.metadata_enabled = query["metadata"][0] != "off"
            if "reset_stats" in query:
                STATE.stats.clear()
        self.send(200, "ok\n", "text/plain")

    # ---- OAuth2 --------------------------------------------------------------------------------------------------
    def oauth_token(self):
        form = urllib.parse.parse_qs(self.body().decode())
        grant = form.get("grant_type", [""])[0]
        if grant == "refresh_token":
            STATE.count("refresh_token")
            if form.get("client_id", [""])[0] != CLIENT_ID or form.get("client_secret", [""])[0] != CLIENT_SECRET:
                return self.oauth_error(401, "invalid_client", "The OAuth client was not found.")
            if form.get("refresh_token", [""])[0] != REFRESH_TOKEN:
                return self.oauth_error(400, "invalid_grant", "Token has been expired or revoked.")
            return self.token_response("", [])
        if grant == "urn:ietf:params:oauth:grant-type:jwt-bearer":
            STATE.count("jwt_bearer")
            return self.jwt_bearer(form.get("assertion", [""])[0])
        return self.oauth_error(400, "unsupported_grant_type", "Invalid grant_type: " + grant)

    def jwt_bearer(self, assertion):
        parts = assertion.split(".")
        if len(parts) != 3:
            return self.oauth_error(400, "invalid_grant", "Invalid JWT.")
        header = json.loads(b64url_decode(parts[0]))
        claims = json.loads(b64url_decode(parts[1]))
        if header.get("alg") != "RS256" or header.get("kid") != "test-key-id":
            return self.oauth_error(400, "invalid_grant", "Invalid JWT header.")
        if not verify_rs256(
            (parts[0] + "." + parts[1]).encode(), b64url_decode(parts[2]), STATE.modulus, STATE.exponent
        ):
            return self.oauth_error(400, "invalid_grant", "Invalid JWT Signature.")
        now = time.time()
        expected_aud = "http://%s/token" % self.headers.get("Host")
        if claims.get("iss") != SA_EMAIL or claims.get("sub") != SA_EMAIL or claims.get("aud") != expected_aud:
            return self.oauth_error(400, "invalid_grant", "Invalid JWT claims.")
        if not (claims.get("iat", 0) - 60 <= now <= claims.get("exp", 0)) or claims["exp"] - claims["iat"] > 3600:
            return self.oauth_error(400, "invalid_grant", "Invalid JWT: token must be a short-lived token.")
        if not claims.get("scope"):
            return self.oauth_error(400, "invalid_scope", "Empty or missing scope not allowed.")
        return self.token_response(SA_EMAIL, claims["scope"].split(" "))

    # ---- STS -----------------------------------------------------------------------------------------------------
    def sts_token(self):
        STATE.count("sts")
        form = urllib.parse.parse_qs(self.body().decode())
        if form.get("grant_type", [""])[0] != "urn:ietf:params:oauth:grant-type:token-exchange":
            return self.oauth_error(400, "invalid_request", "bad grant_type")
        if form.get("subject_token", [""])[0] != SUBJECT_TOKEN:
            return self.oauth_error(400, "invalid_grant", "The subject token is invalid.")
        if not form.get("audience", [""])[0].startswith("//iam.googleapis.com/"):
            return self.oauth_error(400, "invalid_target", "bad audience")
        if form.get("requested_token_type", [""])[0] != "urn:ietf:params:oauth:token-type:access_token":
            return self.oauth_error(400, "invalid_request", "bad requested_token_type")
        token, lifetime = STATE.issue("federated", form.get("scope", [""])[0].split(" "))
        self.send_json(
            200,
            {
                "access_token": token,
                "expires_in": lifetime,
                "issued_token_type": "urn:ietf:params:oauth:token-type:access_token",
                "token_type": "Bearer",
            },
        )

    # ---- IAM Credentials ------------------------------------------------------------------------------------------
    def generate_access_token(self, email):
        STATE.count("generate_access_token")
        body = json.loads(self.body() or b"{}")
        token = self.bearer()
        if not token or not STATE.check(token):
            return self.send_json(
                401,
                {
                    "error": {
                        "code": 401,
                        "message": "Request had invalid authentication " "credentials.",
                        "status": "UNAUTHENTICATED",
                    }
                },
            )
        if email != TARGET_SA:
            return self.send_json(
                403,
                {
                    "error": {
                        "code": 403,
                        "message": "Permission 'iam.serviceAccounts."
                        "getAccessToken' denied on resource (or it may not exist).",
                        "status": "PERMISSION_DENIED",
                    }
                },
            )
        for delegate in body.get("delegates", []):
            if not delegate.startswith("projects/-/serviceAccounts/"):
                return self.send_json(
                    400, {"error": {"code": 400, "message": "bad delegate", "status": "INVALID_ARGUMENT"}}
                )
        new_token, lifetime = STATE.issue(email, body.get("scope", []))
        expire = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + lifetime))
        self.send_json(200, {"accessToken": new_token, "expireTime": expire})

    # ---- metadata ------------------------------------------------------------------------------------------------
    def metadata(self, path, query):
        STATE.count("metadata")
        if not STATE.metadata_enabled:
            return self.send(404, "not found", "text/plain")
        if self.headers.get("Metadata-Flavor") != "Google":
            return self.send(403, "Missing Metadata-Flavor header", "text/plain")
        headers = {"Metadata-Flavor": "Google"}
        base = "/computeMetadata/v1/instance/service-accounts/default/"
        if path == base + "email":
            return self.send(200, METADATA_SA, "application/text", headers)
        if path == base + "token":
            STATE.count("metadata_token")
            scopes = query.get("scopes", [""])[0].split(",")
            token, lifetime = STATE.issue(METADATA_SA, scopes)
            return self.send_json(200, {"access_token": token, "expires_in": lifetime, "token_type": "Bearer"}, headers)
        return self.send(404, "not found", "text/plain", headers)

    # ---- GCS XML API ---------------------------------------------------------------------------------------------
    def xml_error(self, status, code, message):
        body = "<?xml version='1.0' encoding='UTF-8'?><Error><Code>%s</Code><Message>%s</Message></Error>" % (
            code,
            message,
        )
        self.send(status, body, "application/xml")

    def gcs(self, path, query):
        self.body_bytes = self.body() if self.command in ("PUT", "POST") else b""
        STATE.count("gcs")
        token = self.bearer()
        if not token or not STATE.check(token):
            STATE.count("gcs_unauthorized")
            return self.xml_error(401, "AuthenticationRequired", "Authentication required.")
        parts = path.lstrip("/").split("/", 1)
        bucket = urllib.parse.unquote(parts[0])
        key = urllib.parse.unquote(parts[1]) if len(parts) > 1 else ""
        if not bucket:
            return self.xml_error(400, "InvalidRequest", "no bucket")
        if not key:
            if self.command in ("GET", "HEAD"):
                return self.list_objects(bucket, query)
            return self.xml_error(400, "InvalidRequest", "unsupported bucket operation")
        if self.command == "POST" and "uploads" in query:
            upload_id = uuid.uuid4().hex
            with STATE.lock:
                STATE.uploads[upload_id] = {"bucket": bucket, "key": key, "parts": {}}
            return self.send(
                200,
                "<?xml version='1.0' encoding='UTF-8'?><InitiateMultipartUploadResult>"
                "<Bucket>%s</Bucket><Key>%s</Key><UploadId>%s</UploadId>"
                "</InitiateMultipartUploadResult>" % (bucket, key, upload_id),
                "application/xml",
            )
        if self.command == "PUT" and "uploadId" in query:
            upload = STATE.uploads.get(query["uploadId"][0])
            if upload is None:
                return self.xml_error(404, "NoSuchUpload", "no such upload")
            etag = '"%s"' % hashlib.md5(self.body_bytes).hexdigest()
            with STATE.lock:
                upload["parts"][int(query["partNumber"][0])] = self.body_bytes
            return self.send(200, b"", None, {"ETag": etag})
        if self.command == "POST" and "uploadId" in query:
            with STATE.lock:
                upload = STATE.uploads.pop(query["uploadId"][0], None)
                if upload is None:
                    return self.xml_error(404, "NoSuchUpload", "no such upload")
                data = b"".join(upload["parts"][n] for n in sorted(upload["parts"]))
                STATE.objects[(bucket, key)] = data
            etag = '"%s"' % hashlib.md5(data).hexdigest()
            return self.send(
                200,
                "<?xml version='1.0' encoding='UTF-8'?><CompleteMultipartUploadResult>"
                "<Bucket>%s</Bucket><Key>%s</Key><ETag>%s</ETag>"
                "</CompleteMultipartUploadResult>" % (bucket, key, etag),
                "application/xml",
            )
        if self.command == "DELETE":
            with STATE.lock:
                STATE.objects.pop((bucket, key), None)
            return self.send(204, b"", None)
        if self.command == "PUT":
            with STATE.lock:
                STATE.objects[(bucket, key)] = self.body_bytes
            return self.send(200, b"", None, {"ETag": '"%s"' % hashlib.md5(self.body_bytes).hexdigest()})
        if self.command in ("GET", "HEAD"):
            with STATE.lock:
                data = STATE.objects.get((bucket, key))
            if data is None:
                return self.xml_error(404, "NoSuchKey", "The specified key does not exist.")
            headers = {
                "ETag": '"%s"' % hashlib.md5(data).hexdigest(),
                "Last-Modified": "Mon, 23 Nov 2026 00:00:00 GMT",
                "Accept-Ranges": "bytes",
            }
            rng = self.headers.get("Range")
            if rng and self.command == "GET":
                match = re.match(r"bytes=(\d+)-(\d*)", rng)
                start = int(match.group(1))
                end = int(match.group(2)) if match.group(2) else len(data) - 1
                end = min(end, len(data) - 1)
                headers["Content-Range"] = "bytes %d-%d/%d" % (start, end, len(data))
                return self.send(206, data[start : end + 1], "application/octet-stream", headers)
            if self.command == "HEAD":
                self.send_response(200)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                return None
            return self.send(200, data, "application/octet-stream", headers)
        return self.xml_error(405, "MethodNotAllowed", "method not allowed")

    def list_objects(self, bucket, query):
        prefix = query.get("prefix", [""])[0]
        delimiter = query.get("delimiter", [""])[0]
        with STATE.lock:
            keys = sorted(k for (b, k) in STATE.objects if b == bucket and k.startswith(prefix))
            sizes = {k: len(STATE.objects[(bucket, k)]) for k in keys}
            etags = {k: hashlib.md5(STATE.objects[(bucket, k)]).hexdigest() for k in keys}
        contents, prefixes = [], []
        for key in keys:
            rest = key[len(prefix) :]
            if delimiter and delimiter in rest:
                common = prefix + rest.split(delimiter, 1)[0] + delimiter
                if common not in prefixes:
                    prefixes.append(common)
                continue
            contents.append(
                "<Contents><Key>%s</Key><LastModified>2026-11-23T00:00:00.000Z</LastModified>"
                "<ETag>&quot;%s&quot;</ETag><Size>%d</Size><StorageClass>STANDARD</StorageClass>"
                "</Contents>" % (key, etags[key], sizes[key])
            )
        body = (
            "<?xml version='1.0' encoding='UTF-8'?><ListBucketResult xmlns='http://doc.s3.amazonaws.com/"
            "2006-03-01'><Name>%s</Name><Prefix>%s</Prefix><KeyCount>%d</KeyCount><MaxKeys>1000</MaxKeys>"
            "<IsTruncated>false</IsTruncated>%s%s</ListBucketResult>"
            % (
                bucket,
                prefix,
                len(contents),
                "".join(contents),
                "".join("<CommonPrefixes><Prefix>%s</Prefix></CommonPrefixes>" % p for p in prefixes),
            )
        )
        self.send(200, body, "application/xml")


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Clients (httpfs) may close connections mid-response; that is not a test failure
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def write_fixtures(directory, port):
    base = "http://localhost:%d" % port
    with open(PRIVATE_KEY) as f:
        private_key = f.read()
    os.makedirs(os.path.join(directory, "gcloud"), exist_ok=True)
    authorized_user = {
        "type": "authorized_user",
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
        "refresh_token": REFRESH_TOKEN,
        "token_uri": base + "/token",
        "quota_project_id": "quota-project",
    }
    service_account = {
        "type": "service_account",
        "project_id": "test-project",
        "private_key_id": "test-key-id",
        "private_key": private_key,
        "client_email": SA_EMAIL,
        "client_id": "123",
        "token_uri": base + "/token",
    }
    external_common = {
        "type": "external_account",
        "audience": "//iam.googleapis.com/projects/1/locations/global/workloadIdentityPools/p/" "providers/q",
        "subject_token_type": "urn:ietf:params:oauth:token-type:jwt",
        "token_url": base + "/v1/token",
    }
    fixtures = {
        "authorized_user.json": authorized_user,
        "service_account.json": service_account,
        "gcloud/application_default_credentials.json": service_account,
        "bad_refresh_token.json": dict(authorized_user, refresh_token="revoked"),
        "impersonated.json": {
            "type": "impersonated_service_account",
            "service_account_impersonation_url": base
            + "/v1/projects/-/serviceAccounts/%s:generateAccessToken" % TARGET_SA,
            "delegates": [],
            "source_credentials": authorized_user,
        },
        "external_file.json": dict(
            external_common, credential_source={"file": os.path.join(directory, "subject_token.txt")}
        ),
        "external_url.json": dict(
            external_common,
            credential_source={
                "url": base + "/subject_token",
                "headers": {"X-Test-Header": "present"},
                "format": {"type": "json", "subject_token_field_name": "id_token"},
            },
        ),
        "external_impersonated.json": dict(
            external_common,
            credential_source={"file": os.path.join(directory, "subject_token.txt")},
            service_account_impersonation_url=base
            + "/v1/projects/-/serviceAccounts/%s:generateAccessToken" % TARGET_SA,
        ),
    }
    for name, content in fixtures.items():
        with open(os.path.join(directory, name), "w") as f:
            json.dump(content, f, indent=2)
    with open(os.path.join(directory, "subject_token.txt"), "w") as f:
        f.write(SUBJECT_TOKEN + "\n")


def main():
    global STATE
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--dir", required=True, help="directory to write credential fixtures into")
    parser.add_argument("--port-file", help="write the bound port to this file once listening")
    args = parser.parse_args()

    STATE = State()
    server = Server(("127.0.0.1", args.port), Handler)
    port = server.server_address[1]
    os.makedirs(args.dir, exist_ok=True)
    write_fixtures(args.dir, port)
    if args.port_file:
        with open(args.port_file + ".tmp", "w") as f:
            f.write(str(port))
        os.replace(args.port_file + ".tmp", args.port_file)
    sys.stderr.write("mock_google: listening on 127.0.0.1:%d, fixtures in %s\n" % (port, args.dir))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
