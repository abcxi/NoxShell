# Local SSH authentication regression fixture

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
cmake -S . -B build -DNOXSHELL_BUILD_AUTH_INTEGRATION_TESTS=ON
cmake --build build --target NoxShellAuthIntegrationTest
NOXSHELL_AUTH_TEST_SERVER="$PWD/build/noxshell-auth-fixture" ./build/NoxShellAuthIntegrationTest -v1
```

The integration executable uses Qt Core/Test without opening application UI.
It skips when the fixture path is unset. Each case starts and cleans up its
own process. The tests cover ordinary password authentication, interactive
password authentication, fallback after an explicit rejection, a first
authentication response slower than the former 8-second deadline, discovery
timeout, password-response timeout, an incorrect password, prompt cancellation,
and ignoring obsolete connection requests. Expect about 12 seconds to run the
suite. A sandbox must allow local loopback sockets.
