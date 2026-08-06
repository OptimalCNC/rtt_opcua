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

## Static Publication API

Application typekits and their OPC UA transport plugins must register every
required datatype provider and codec before the endpoint starts. The generic
server flow is:

```cpp
std::string error;
if (!RTT::opcua::registerCanonicalTypeProtocols(&error) ||
    !RTT::opcua::freezeDataTypeRegistry(&error)) {
    throw std::runtime_error(error);
}

RTT::opcua::Server server(options);
if (!server.start(&error)) {
    throw std::runtime_error(error);
}

{
    RTT::opcua::ObjectModel model(server);
    std::vector<RTT::opcua::UnsupportedResource> unsupported;
    if (!model.publishComponent(component, &error, &unsupported)) {
        throw std::runtime_error(error); // Nothing was partially published.
    }

    server.stop();
} // Destroy the ObjectModel before releasing published components.
```

`publishComponent` validates the complete component interface before commit.
It is strict, static, and idempotent for the same component instance. An
unsupported operation, property, attribute, constant, or port rejects the
whole component and leaves diagnostics available through
`unsupportedResources`.

Resources added to an RTT component after publication are not added to the
address space. This version has no public unpublish or component-replacement
API. Destroying `ObjectModel` is endpoint teardown; it drains retained timed-out
operation calls before its published RTT components may be destroyed.

OCL owns the operator-facing lifecycle. A typical deployment script is:

```text
import("sample_typekit")
loadComponent("sample", "SampleComponent")
opcua.start()
opcua.publishComponent("sample")
```

The first `opcua.start()` freezes the registry and publishes the complete
Deployer interface. Other local components require an explicit
`publishComponent` call; `Server=true` is not an OPC UA publication rule.

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
