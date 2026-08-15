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
- server binding restricted to `127.0.0.1`, `::1`, or the explicit IPv4
  wildcard `0.0.0.0`
- static full or selected publication of RTT component interfaces
- no PKI configuration or user-level access control

The generic server default remains `127.0.0.1`. Setting `bind_address` to
`0.0.0.0` exposes the endpoint on every IPv4 interface with SecurityPolicy
None, anonymous access, and no authentication or authorization. Use wildcard
binding only on a trusted, isolated network. Concrete non-loopback addresses,
the IPv6 wildcard, and PKI configuration remain unsupported.

## Static Publication APIs

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
} // Destroy the ObjectModel before releasing published components.

server.stop();
```

`ObjectModel` has two publication APIs:

```cpp
bool publishComponent(
    RTT::TaskContext& component, std::string* error = nullptr,
    std::vector<RTT::opcua::UnsupportedResource>* unsupported = nullptr);

bool publishComponentSelected(
    RTT::TaskContext& component, const std::vector<std::string>& selectors,
    std::string* error = nullptr,
    std::vector<RTT::opcua::PublicationDiagnostic>* diagnostics = nullptr);
```

`publishComponent` validates the complete component interface before commit.
It is strict and static. An unsupported operation, property, attribute,
constant, or port rejects the whole component and leaves compatibility
diagnostics in `UnsupportedResource` through both the output argument and
`unsupportedResources(component_name)`.

`publishComponentSelected` requires at least one selector. Every individual
selector must match at least one logical RTT resource; a malformed or unmatched
selector rejects the complete publication even when another selector matches.
Publication is atomic: each selected logical resource is mapped as its complete
OPC UA node bundle, including required metadata and values, and no component is
committed when validation fails. Validation covers only those selected resource
bundles, their required service ancestors, and the mandatory proxy baseline
below. Unsupported resources outside that effective set do not reject selected
publication. Selector, inventory, selected-resource, mandatory baseline, and
replay conflicts are reported as structured
`PublicationDiagnostic` values through the output argument and
`publicationDiagnostics(component_name)`. Existing callers that inspect
`unsupportedResources` retain the underlying unsupported-resource records;
new selective callers should use the structured diagnostics to distinguish
selector and topology errors.

### Selector syntax

Selectors are glob paths over logical RTT resources, not regular expressions.
The root resource forms are:

```text
operations/<operation>
properties/<property>
attributes/<attribute>
ports/<port>
services/<service>
services/<service>/<root resource form>
services/<service>/services/<nested service>/...
```

For example, these selectors publish one root operation, one root property,
one root port, and two resources in the `math` service:

```cpp
const std::vector<std::string> selectors{
    "operations/add",
    "properties/Gain",
    "ports/Command",
    "services/math/operations/scale",
    "services/math/properties/Offset",
};
model.publishComponentSelected(component, selectors, &error);
```

`*` is a whole-segment wildcard matching exactly one segment. `**` is a
whole-segment recursive wildcard and must be the final segment; for example,
`services/math/**` selects resources below `math`. Partial wildcards such as
`motor*`, regex syntax, empty segments, and a non-terminal `**` are invalid.
An exact service selector such as `services/math` selects that service object
only; use `services/math/**` to select its descendants. The equivalent nested
form is `services/<service>/services/<nested service>`.

Ports and services with the same name are independent logical resources. For
example, `ports/Command` selects the `Command` port and its port bundle, while
`services/Command/**` selects a same-named `Command` service and its contents;
neither selector implies the other.

Literal selector segments use the same canonical percent escaping as OPC UA
NodeId path segments. Unreserved ASCII characters (`A-Z`, `a-z`, `0-9`, `-`,
`.`, `_`, `~`) are literal; every other byte is `%HH` with uppercase hex. Thus
a service named `motion/raw*` is selected as
`services/motion%2Fraw%2A/**`, not with a glob. Use
`RTT::opcua::escapeNodeIdSegment` to construct literal name segments rather
than hand-escaping them.

### Mandatory proxy baseline

Selected publication always includes these eight root lifecycle-query
operations, even when no selector matches them:

```text
getTaskState
getTargetState
isConfigured
isActive
isRunning
inFatalError
inException
inRunTimeError
```

They must exist with their proxy-compatible schemas or selected publication is
rejected. Mutating lifecycle operations such as `configure`, `start`, `stop`,
and `cleanup` are not part of this baseline. A `TaskContextProxy` can therefore
reconstruct and report lifecycle state from a deliberately sparse publication
without remote lifecycle control being exposed.

### Static identity and topology

The first successful publication fixes both the publication mode and effective
resource set for that component instance. Repeating full publication is
idempotent. Repeating selected publication may use different selector text,
but it is accepted only when it resolves to the same effective resource set;
switching between full and selected modes, changing the effective set, or
publishing a different component instance with the same name is rejected.

Resources added after publication are not added to the address space. There is
no public unpublish or component-replacement API. Changing publication
topology therefore requires endpoint teardown and restart with a new
`ObjectModel`; clients must reconstruct or synchronize their proxies against
the restarted endpoint.

Destroying `ObjectModel` drains retained timed-out operation calls before its
published RTT components may be destroyed.

OCL owns the operator-facing lifecycle. A typical deployment script is:

```text
import("sample_typekit")
loadComponent("sample", "SampleComponent")
opcua.start()
var StringArray deployer_selectors = StringArray("services/opcua/**")
opcua.publishComponentSelected("Deployer", deployer_selectors)
```

`opcua.start()` starts the endpoint and freezes the registry; it does not
publish the Deployer or any local component. Publication is always explicit.
`Server=true` is not an OPC UA publication rule.

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
