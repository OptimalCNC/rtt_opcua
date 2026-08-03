# rtt_opcua

`rtt_opcua` provides a native OPC UA transport for Orocos RTT. It publishes
the supported interface of an RTT `TaskContext` and can construct a remote RTT
proxy from that model. The implementation uses open62541 through open62541pp.

The package is generic transport infrastructure. The OPC UA deployment
component, `deployer-opcua-<target>`, and `ctaskbrowser-opcua-<target>` are
provided by OCL.

## Current Scope

- C++20
- open62541pp 0.21.2 or newer within the 0.21 API series
- server binding restricted to `127.0.0.1` or `::1`
- complete publication of each selected component's supported RTT interface
- no publication allowlist, PKI configuration, or user-level access control

Non-loopback binding and PKI are intentionally deferred until their security
contract is designed and tested.

## Information Model

The namespace URI is `urn:orocos:rtt`. Namespace indexes are resolved at
runtime. String NodeIds start at `rtt` and use paths such as
`rtt/components/<component>/operations/<operation>`. Each path segment is
percent-escaped independently, so component and resource names remain
unambiguous.

Published resources include recursively nested services, operations,
properties, attributes, ports, lifecycle state, and model revision. Assignable
properties and attributes are writable; constants and other non-assignable
data sources are read-only.

## Build And Test

Install RTT, open62541, and open62541pp into an isolated prefix first, then:

```bash
export OROCOS_TARGET=gnulinux
prefix=/tmp/rtt-opcua-prefix
build=/tmp/rtt-opcua-build

cmake -S . -B "$build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$prefix" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DBUILD_TESTING=ON \
  -DRTT_OPCUA_WARNINGS_AS_ERRORS=ON
cmake --build "$build" --parallel
ctest --test-dir "$build" --output-on-failure
cmake --install "$build"
```

The package installs the `orocos-rtt-opcua-<target>` library, the
`rtt-transport-opcua-<target>` RTT plugin, headers, and package metadata into
the selected prefix.

## License

LGPL-2.1-or-later. See [LICENSE](LICENSE).
