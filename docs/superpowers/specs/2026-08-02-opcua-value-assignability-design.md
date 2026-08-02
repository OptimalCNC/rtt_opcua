# OPC UA Value Assignability Design

## Context

The native OPC UA object model currently publishes RTT properties as writable
and every RTT attribute as read-only. This loses the distinction between an
assignable value registered with `addAttribute(...)` and a constant registered
with `addConstant(...)`.

As a result, a local TaskBrowser can assign an RTT attribute while
`ctaskbrowser-opcua` rejects the same expression as an assignment to a
constant. The established RTT CORBA proxy preserves the underlying data
source's assignability, so the OPC UA behavior is not transport-compatible.

## Decision

OPC UA writability follows `RTT::base::DataSourceBase::isAssignable()`.

| RTT value | OPC UA access | Remote TaskBrowser behavior |
|---|---|---|
| Property with an assignable data source | Read/write | Assignment succeeds |
| Attribute added with `addAttribute(...)` | Read/write | Assignment succeeds |
| Constant added with `addConstant(...)` | Read-only | Assignment is rejected |
| Any non-assignable custom data source | Read-only | Assignment is rejected |

The rule applies to the component root and every nested RTT service. An
assignable value remains writable in every component lifecycle state,
including Running, matching local TaskBrowser and CORBA behavior.

## Server Model

When constructing property and attribute nodes, the object model determines
the write flag from the value's RTT data source. The flag controls both:

- OPC UA `AccessLevel` and `UserAccessLevel`
- the guarded value backend's acceptance of write requests

This keeps advertised access and actual backend behavior consistent. A write
to a read-only value returns `BadNotWritable`. A write with an incompatible
wire type continues to return `BadTypeMismatch`.

## Client Proxy

No new client-side policy is introduced. The client already reads
`UserAccessLevel` during discovery and supplies a writer only for nodes that
advertise `CurrentWrite`. The type protocol then creates either an assignable
or read-only proxy data source.

This keeps the OPC UA server as the source of truth and prevents the client
from guessing writability based on whether a value appears in the Properties
or Attributes folder.

## Tests

The object-model test will publish one value of each relevant kind and verify:

- property writes succeed and update the RTT value
- assignable attribute writes succeed and update the RTT value
- constant writes fail and leave the RTT value unchanged
- incompatible writes to writable values still fail

The task-context proxy test will verify:

- remote properties remain assignable
- remote assignable attributes expose an assignable data source
- assigning through the proxy updates the target component
- remote constants expose a non-assignable data source and remain readable
- nested service values follow the same rule

The manual smoke test will publish `OCL::HelloWorld` and execute
`the_attribute = "xxx"` through `ctaskbrowser-opcua`, followed by a readback.

## Security And Scope

This change does not broaden component publication, endpoint binding, user
authentication, or authorization. Only values on an explicitly published RTT
component are affected. Deployment-level access control remains responsible
for deciding who may connect; RTT assignability determines what a connected
client may write.

No lifecycle write restrictions, per-value ACLs, new metadata nodes, or changes
to local RTT behavior are included.
