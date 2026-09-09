# Local SSH connection regression fixture

This optional fixture exercises the real `Libssh2Worker` against a local SSH
server. It uses `127.0.0.1`, a random port, an ephemeral host key, and a fixed
synthetic password. It does not read saved hosts, Keychain, private keys, or
real server credentials. It is never included in the application bundle.

Build the fixture with Go 1.25 or later from this directory:

```sh
go build -o ../../build/noxshell-auth-fixture .
```

From the repository root, enable and build the optional integration target,
then run it with the fixture path in `NOXSHELL_AUTH_TEST_SERVER`:

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DNOXSHELL_BUILD_AUTH_INTEGRATION_TESTS=ON
cmake --build build --target NoxShellAuthIntegrationTest
NOXSHELL_AUTH_TEST_SERVER="$PWD/build/noxshell-auth-fixture" ./build/NoxShellAuthIntegrationTest -v1
```

The integration executable uses Qt Core/Test without opening application UI.
It skips when the fixture path is unset. Each case starts and cleans up its
own process. The tests cover ordinary password authentication, interactive
password authentication, fallback after an explicit rejection, a first
authentication response slower than the former 8-second deadline, discovery
timeout, password-response timeout, an incorrect password, prompt cancellation,
and ignoring obsolete connection requests. It additionally covers:

- Ed25519/curve25519/AES-128-GCM, ECDSA/P-256/AES-256-CTR,
  RSA-SHA256/group14-SHA256/AES-128-CTR and RSA-SHA512/curve25519/AES-256-GCM.
- Rejecting a changed host fingerprint before sending credentials.
- Reusing one worker for five reconnects, with byte-exact input/output across rekeying.
- A server that stops consuming input: bounded stall timeout and cancellation,
  disconnecting without replaying partially delivered input.
- Continuous stdout without starving stderr or queued resize/disconnect operations.
- Complete buffered output at EOF and recovery after an abrupt TCP reset.

Known limitation: when the server closes the TCP connection immediately after
sending a large final burst, libssh2 1.11.1 can report a transport error before
returning already buffered packets. The `channel-and-tcp-close` row retains
this reproducer as **XFAIL** for libssh2 <= 1.11.1, not as a fixed issue.
Ordinary channel EOF (with the transport still open) is checked without an
exception. The source of the library limitation is the transport-error return
before the packet-queue loop in
[libssh2 channel.c](https://github.com/libssh2/libssh2/blob/libssh2-1.11.1/src/channel.c#L1941).
This suite must not be described as passing every output-loss edge case.

Connection and first-output timings are printed for the algorithm matrix.
These are loopback measurements, not predictions of real network latency.
This fixture emulates SSH configurations; it does **not** certify testing on
Ubuntu, Debian, CentOS, Rocky, or any other Linux distribution. Real-server
validation must separately record the OS, SSH daemon/configuration, port,
authentication method and network conditions. Do not put real credentials in
this fixture or its logs.

Expect about 15–25 seconds to run the suite. CTest applies a 120-second outer
timeout so a regression cannot hang CI indefinitely. A sandbox must allow local
loopback sockets. The macOS release workflow already runs this optional suite.
