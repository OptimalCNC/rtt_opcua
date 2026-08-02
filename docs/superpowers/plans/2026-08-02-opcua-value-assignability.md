# OPC UA Value Assignability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve RTT data-source assignability through the OPC UA server so remote properties and attributes are writable while constants remain read-only.

**Architecture:** The server derives each published value node's write access from `DataSourceBase::isAssignable()` and uses the same flag for OPC UA metadata and the guarded backend. The existing client continues to derive its proxy data-source kind from OPC UA `UserAccessLevel`, so the server remains the single source of truth.

**Tech Stack:** C++20, Orocos RTT, open62541pp, Boost.Test, CMake, CTest, OCL TaskBrowser.

## Global Constraints

- Never install into or read runtime state from `~/.orocos`; build, install, and run from `/tmp/orocos-rock-modernization.0QPXI6`.
- Assignable properties and `addAttribute(...)` values are writable in every lifecycle state, including Running.
- `addConstant(...)` and every other non-assignable data source remain read-only.
- Apply the same behavior at the component root and in nested RTT services.
- Preserve `BadNotWritable` for read-only writes and `BadTypeMismatch` for incompatible writes.
- Do not add client-side category rules, lifecycle write restrictions, ACLs, or unrelated refactors.

---

### Task 1: Preserve RTT Assignability Across The OPC UA Model And Proxy

**Files:**
- Modify: `tests/object_model_test.cpp:150-230`
- Modify: `tests/task_context_proxy_test.cpp:91-145`
- Modify: `tests/task_context_proxy_test.cpp:225-355`
- Modify: `src/object_model.cpp:403-455`
- Modify: `src/object_model.cpp:690-735`

**Interfaces:**
- Consumes: `RTT::base::DataSourceBase::isAssignable() const -> bool`
- Produces: OPC UA `AccessLevel` and `UserAccessLevel` with `CurrentWrite` exactly when the RTT source is assignable.
- Produces: an assignable `TaskContextProxy` data source for writable nodes and a read-only alias for constants.

- [ ] **Step 1: Add the failing server-model regression coverage**

Extend the existing component fixture in `tests/object_model_test.cpp` with a
constant beside the writable attribute:

```cpp
std::int32_t gain = 7;
std::string status = "idle";
std::string model_name = "arm-v1";
component.addProperty("Gain", gain).doc("Controller gain");
component.addAttribute("Status", status);
component.addConstant("ModelName", model_name);
```

Create the constant node ID beside `status_id`:

```cpp
const auto model_name_id = modelNodeId(
    namespace_index,
    {"components", "arm/left", "attributes", "ModelName"});
```

Replace the old assertion that `Status` cannot be written with assertions that
the assignable attribute accepts a correctly typed write and rejects an
incompatible type. Then verify the constant remains read-only:

```cpp
const auto status_value = ::opcua::services::readValue(client, status_id);
BOOST_REQUIRE(status_value);
BOOST_TEST(status_value.value().to<std::string>() == "idle");
BOOST_CHECK(::opcua::services::writeValue(
                client, status_id, ::opcua::Variant(std::string("active")))
                .isGood());
BOOST_TEST(status == "active");

const auto wrong_status_write = ::opcua::services::writeValue(
    client, status_id, ::opcua::Variant(std::int32_t{42}));
BOOST_TEST(wrong_status_write.code() == UA_STATUSCODE_BADTYPEMISMATCH);
BOOST_TEST(status == "active");

const auto model_name_value =
    ::opcua::services::readValue(client, model_name_id);
BOOST_REQUIRE(model_name_value);
BOOST_TEST(model_name_value.value().to<std::string>() == "arm-v1");
const auto model_name_write = ::opcua::services::writeValue(
    client, model_name_id, ::opcua::Variant(std::string("unsafe")));
BOOST_TEST(model_name_write.code() == UA_STATUSCODE_BADNOTWRITABLE);
BOOST_TEST(model_name == "arm-v1");
```

- [ ] **Step 2: Add the failing proxy and nested-service regression coverage**

Extend `ProxyTarget` in `tests/task_context_proxy_test.cpp`:

```cpp
addProperty("Gain", gain).doc("Controller gain.");
addAttribute("Status", status);
addConstant("ModelName", model_name);

RTT::Service::shared_ptr math = RTT::Service::Create("math");
math->doc("Math utilities.");
math->addLocalPort(math_feedback).doc("Nested calculation feedback.");
math->addProperty("Offset", offset).doc("Scale offset.");
math->addAttribute("Mode", mode);
math->addConstant("Unit", unit);
```

Add storage with the other fixture members:

```cpp
std::int32_t gain{7};
std::int32_t offset{2};
std::string status{"idle"};
std::string model_name{"calculator-v1"};
std::string mode{"manual"};
std::string unit{"counts"};
```

Replace the read-only `Status` expectation with a writable proxy assertion and
verify a write while the component is Running:

```cpp
RTT::base::AttributeBase *status = proxy->provides()->getAttribute("Status");
BOOST_REQUIRE(status != nullptr);
auto *status_source = RTT::internal::AssignableDataSource<std::string>::narrow(
    status->getDataSource().get());
BOOST_REQUIRE(status_source != nullptr);
BOOST_TEST(status_source->get() == "idle");
BOOST_REQUIRE(proxy->configure());
BOOST_REQUIRE(proxy->start());
status_source->set("remote-running");
BOOST_TEST(target.status == "remote-running");
BOOST_REQUIRE(proxy->stop());
BOOST_REQUIRE(proxy->cleanup());
target.status = "controller-update";
BOOST_TEST(status_source->get() == "controller-update");

RTT::base::AttributeBase *model_name =
    proxy->provides()->getAttribute("ModelName");
BOOST_REQUIRE(model_name != nullptr);
BOOST_TEST(!model_name->getDataSource()->isAssignable());
auto *model_name_source = RTT::internal::DataSource<std::string>::narrow(
    model_name->getDataSource().get());
BOOST_REQUIRE(model_name_source != nullptr);
BOOST_TEST(model_name_source->get() == "calculator-v1");
```

After retrieving the nested `math` service, verify the same distinction:

```cpp
RTT::base::AttributeBase *mode = math->getAttribute("Mode");
BOOST_REQUIRE(mode != nullptr);
auto *mode_source = RTT::internal::AssignableDataSource<std::string>::narrow(
    mode->getDataSource().get());
BOOST_REQUIRE(mode_source != nullptr);
mode_source->set("automatic");
BOOST_TEST(target.mode == "automatic");

RTT::base::AttributeBase *unit = math->getAttribute("Unit");
BOOST_REQUIRE(unit != nullptr);
BOOST_TEST(!unit->getDataSource()->isAssignable());
auto *unit_source = RTT::internal::DataSource<std::string>::narrow(
    unit->getDataSource().get());
BOOST_REQUIRE(unit_source != nullptr);
BOOST_TEST(unit_source->get() == "counts");
```

- [ ] **Step 3: Build and run both tests to verify RED**

Run:

```bash
cmake --build /tmp/orocos-rock-modernization.0QPXI6/build-state/rtt_opcua \
  --target rtt_opcua_object_model_test rtt_opcua_task_context_proxy_test -j2
ctest --test-dir /tmp/orocos-rock-modernization.0QPXI6/build-state/rtt_opcua \
  -R 'rtt_opcua_(object_model|task_context_proxy)_test' --output-on-failure
```

Expected: both tests build; the object-model test fails because the `Status`
write returns `BadNotWritable`, and the proxy test fails because `Status` has a
non-assignable data source.

- [ ] **Step 4: Derive write access inside `dataSourceSpec`**

Remove the caller-provided `bool writable` parameter and derive it once from
the RTT source:

```cpp
NodeSpec dataSourceSpec(const std::string &parent_path, const std::string &name,
                        const std::string &description,
                        const RTT::base::DataSourceBase::shared_ptr &source,
                        const std::shared_ptr<ComponentState> &state) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.parent_path = parent_path;
  spec.path = appendNodeSegment(parent_path, name);
  spec.browse_name = name;
  const TypeProtocol *protocol = protocolForDataSource(source);
  const bool writable = source->isAssignable();
```

Keep capturing `writable` in `spec.create`, then remove the final boolean from
both call sites:

```cpp
dataSourceSpec(properties_path, name, property->getDescription(), source,
               state)
```

```cpp
dataSourceSpec(attributes_path, name, {}, source, state)
```

- [ ] **Step 5: Build and run the focused tests to verify GREEN**

Run the Step 3 commands again.

Expected: `2/2` selected tests pass with no compiler warnings under
`RTT_OPCUA_WARNINGS_AS_ERRORS=ON`.

- [ ] **Step 6: Run the complete package suite**

Run:

```bash
cmake --build /tmp/orocos-rock-modernization.0QPXI6/build-state/rtt_opcua -j2
ctest --test-dir /tmp/orocos-rock-modernization.0QPXI6/build-state/rtt_opcua \
  --output-on-failure
```

Expected: all five `rtt_opcua` tests pass.

- [ ] **Step 7: Review and commit the implementation**

Run:

```bash
git diff --check
git diff -- src/object_model.cpp tests/object_model_test.cpp \
  tests/task_context_proxy_test.cpp
git status --short
```

Expected: only the three planned files are modified and no whitespace errors
are reported.

Commit:

```bash
git add src/object_model.cpp tests/object_model_test.cpp \
  tests/task_context_proxy_test.cpp
git commit -m "fix: preserve RTT value assignability over OPC UA"
```

---

### Task 2: Verify The Installed Remote TaskBrowser Behavior

**Files:**
- No tracked files modified.
- Runtime fixture: `/tmp/orocos-rock-modernization.0QPXI6/manual-opcua-demo/sample.ops`

**Interfaces:**
- Consumes: installed `deployer-opcua`, `ctaskbrowser-opcua`, and `OCL::HelloWorld` from the temporary prefix.
- Produces: manual evidence that a remote writable attribute accepts assignment and a remote constant rejects it.

- [ ] **Step 1: Install only to the temporary prefix**

Run:

```bash
cmake --install /tmp/orocos-rock-modernization.0QPXI6/build-state/rtt_opcua
```

Expected: installation updates
`/tmp/orocos-rock-modernization.0QPXI6/prefix/toolchain`; no path under
`~/.orocos` is used.

- [ ] **Step 2: Start the disposable loopback deployment**

Run in terminal one:

```bash
OROCOS_TEST_ROOT=/tmp/orocos-rock-modernization.0QPXI6
"$OROCOS_TEST_ROOT/clean-env" env \
  TERM=xterm \
  OROCOS_TARGET=gnulinux \
  PATH="$OROCOS_TEST_ROOT/prefix/toolchain/bin:/usr/bin:/bin" \
  deployer-opcua --opcua-port 4841 \
    -s "$OROCOS_TEST_ROOT/manual-opcua-demo/sample.ops"
```

Expected: the server listens only on `127.0.0.1:4841` and publishes
`hello_world`.

- [ ] **Step 3: Exercise writable and read-only values remotely**

Run in terminal two:

```bash
OROCOS_TEST_ROOT=/tmp/orocos-rock-modernization.0QPXI6
"$OROCOS_TEST_ROOT/clean-env" env \
  OROCOS_TARGET=gnulinux \
  PATH="$OROCOS_TEST_ROOT/prefix/toolchain/bin:/usr/bin:/bin" \
  ctaskbrowser-opcua opc.tcp://127.0.0.1:4841/rtt hello_world
```

Execute:

```text
configure()
start()
the_attribute = "xxx"
the_attribute
the_constant = "unsafe"
stop()
cleanup()
quit
```

Expected:

- `the_attribute = "xxx"` succeeds while the component is Running.
- Reading `the_attribute` returns `xxx`.
- `the_constant = "unsafe"` is rejected as an assignment to a constant.
- Lifecycle operations return `true` and restore the component to
  PreOperational.

- [ ] **Step 4: Stop the disposable server and confirm cleanup**

Enter `quit` in terminal one, then run:

```bash
ss -ltnp 'sport = :4841'
```

Expected: no listener remains on port `4841`. The existing user-owned server
on port `4840` is not modified.
