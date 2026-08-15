#define BOOST_TEST_MODULE rtt_opcua_object_model
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/port_direction.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <open62541/server.h>
#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/method.hpp>
#include <open62541pp/services/nodemanagement.hpp>
#include <open62541pp/services/view.hpp>
#include <open62541pp/subscription.hpp>

#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/base/PortInterface.hpp>
#include <rtt/internal/GlobalEngine.hpp>
#include <rtt/internal/SharedConnection.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(RTT::opcua::UnsupportedResource)
BOOST_TEST_DONT_PRINT_LOG_VALUE(RTT::opcua::PublicationDiagnostic)
BOOST_TEST_DONT_PRINT_LOG_VALUE(::opcua::ValueRank)

namespace {

std::uint16_t unusedLoopbackPort() {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    throw std::runtime_error("failed to create test socket");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    ::close(socket_fd);
    throw std::runtime_error("failed to bind test socket");
  }

  socklen_t size = sizeof(address);
  if (::getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &size) !=
      0) {
    ::close(socket_fd);
    throw std::runtime_error("failed to inspect test socket");
  }
  const std::uint16_t port = ntohs(address.sin_port);
  ::close(socket_fd);
  return port;
}

bool waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

::opcua::NodeId modelNodeId(std::uint16_t namespace_index,
                            std::initializer_list<std::string_view> segments) {
  const std::vector<std::string_view> path_segments(segments);
  return ::opcua::NodeId(namespace_index,
                         RTT::opcua::makeNodePath(path_segments));
}

void requireMissingNode(::opcua::Client &client, const ::opcua::NodeId &id) {
  const auto result = ::opcua::services::readNodeClass(client, id);
  BOOST_REQUIRE(!result);
  BOOST_TEST(result.code() == UA_STATUSCODE_BADNODEIDUNKNOWN);
}

void requirePortDirection(::opcua::Client &client,
                          const ::opcua::NodeId &parent_id,
                          const ::opcua::NodeId &id,
                          RTT::opcua::PortDirection expected) {
  const auto value = ::opcua::services::readValue(client, id);
  BOOST_REQUIRE(value);
  BOOST_TEST(value.value().isScalar());
  BOOST_TEST(value.value().isType(
      ::opcua::NodeId(::opcua::DataTypeId::Int32)));
  BOOST_TEST(value.value().to<std::int32_t>() ==
             static_cast<std::int32_t>(expected));

  const auto data_type = ::opcua::services::readDataType(client, id);
  const auto node_class = ::opcua::services::readNodeClass(client, id);
  const auto value_rank = ::opcua::services::readValueRank(client, id);
  const auto access = ::opcua::services::readAccessLevel(client, id);
  const auto user_access =
      ::opcua::services::readUserAccessLevel(client, id);
  BOOST_REQUIRE(data_type);
  BOOST_REQUIRE(node_class);
  BOOST_REQUIRE(value_rank);
  BOOST_REQUIRE(access);
  BOOST_REQUIRE(user_access);
  BOOST_CHECK(data_type.value() ==
              ::opcua::NodeId(::opcua::DataTypeId::Int32));
  BOOST_CHECK(node_class.value() == ::opcua::NodeClass::Variable);
  BOOST_TEST(value_rank.value() == ::opcua::ValueRank::Scalar);
  BOOST_TEST(access.value().allOf(::opcua::AccessLevel::CurrentRead));
  BOOST_TEST(access.value().noneOf(::opcua::AccessLevel::CurrentWrite));
  BOOST_TEST(user_access.value().allOf(::opcua::AccessLevel::CurrentRead));
  BOOST_TEST(
      user_access.value().noneOf(::opcua::AccessLevel::CurrentWrite));

  BOOST_TEST(::opcua::services::writeValue(
                 client, id,
                 ::opcua::Variant(static_cast<std::int32_t>(expected))) ==
             UA_STATUSCODE_BADNOTWRITABLE);

  const ::opcua::BrowseDescription parent_browse(
      id, ::opcua::BrowseDirection::Inverse,
      ::opcua::ReferenceTypeId::HasComponent, false,
      ::opcua::NodeClass::Object, ::opcua::BrowseResultMask::All);
  const auto parents = ::opcua::services::browseAll(client, parent_browse);
  BOOST_REQUIRE(parents);
  BOOST_TEST(std::ranges::any_of(
      parents.value(), [&](const auto &reference) {
        return reference.nodeId().isLocal() &&
               reference.nodeId().nodeId() == parent_id;
      }));

  const ::opcua::BrowseDescription type_browse(
      id, ::opcua::BrowseDirection::Forward,
      ::opcua::ReferenceTypeId::HasTypeDefinition, false,
      ::opcua::NodeClass::VariableType, ::opcua::BrowseResultMask::All);
  const auto type_definitions =
      ::opcua::services::browseAll(client, type_browse);
  BOOST_REQUIRE(type_definitions);
  BOOST_TEST(std::ranges::any_of(
      type_definitions.value(), [](const auto &reference) {
        return reference.nodeId().isLocal() &&
               reference.nodeId().nodeId() ==
                   ::opcua::NodeId(
                       ::opcua::VariableTypeId::BaseDataVariableType);
      }));
}

void requirePortValueSchema(::opcua::Client &client, const ::opcua::NodeId &id,
                            ::opcua::DataTypeId expected_type,
                            ::opcua::ValueRank expected_rank, bool readable,
                            bool writable) {
  const auto data_type = ::opcua::services::readDataType(client, id);
  const auto node_class = ::opcua::services::readNodeClass(client, id);
  const auto value_rank = ::opcua::services::readValueRank(client, id);
  const auto access = ::opcua::services::readAccessLevel(client, id);
  const auto user_access = ::opcua::services::readUserAccessLevel(client, id);
  BOOST_REQUIRE(data_type);
  BOOST_REQUIRE(node_class);
  BOOST_REQUIRE(value_rank);
  BOOST_REQUIRE(access);
  BOOST_REQUIRE(user_access);
  BOOST_CHECK(data_type.value() == ::opcua::NodeId(expected_type));
  BOOST_CHECK(node_class.value() == ::opcua::NodeClass::Variable);
  BOOST_TEST(value_rank.value() == expected_rank);
  BOOST_TEST(access.value().anyOf(::opcua::AccessLevel::CurrentRead) ==
             readable);
  BOOST_TEST(access.value().anyOf(::opcua::AccessLevel::CurrentWrite) ==
             writable);
  BOOST_TEST(user_access.value().anyOf(::opcua::AccessLevel::CurrentRead) ==
             readable);
  BOOST_TEST(user_access.value().anyOf(::opcua::AccessLevel::CurrentWrite) ==
             writable);
}

bool hasHierarchicalReference(::opcua::Client &client,
                              const ::opcua::NodeId &source,
                              const ::opcua::NodeId &target,
                              ::opcua::BrowseDirection direction) {
  const ::opcua::BrowseDescription browse(
      source, direction, ::opcua::ReferenceTypeId::HierarchicalReferences, true,
      ::opcua::NodeClass::Unspecified, ::opcua::BrowseResultMask::All);
  const auto result = ::opcua::services::browseAll(client, browse);
  return result &&
         std::ranges::any_of(result.value(), [&](const auto &reference) {
           return reference.nodeId().isLocal() &&
                  reference.nodeId().nodeId() == target;
         });
}

std::size_t generatedMethodArgumentCount(::opcua::Server &server) {
  std::size_t count = 0U;
  const auto visitor = [](void *context, const UA_Node *node) {
    const std::string_view name(
        reinterpret_cast<const char *>(node->head.browseName.name.data),
        node->head.browseName.name.length);
    if (name == "InputArguments" || name == "OutputArguments") {
      ++*static_cast<std::size_t *>(context);
    }
  };
  UA_ServerConfig *config = UA_Server_getConfig(server.handle());
  config->nodestore.iterate(config->nodestore.context, visitor, &count);
  return count;
}

struct CanonicalTypesFixture {
  CanonicalTypesFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());
  }
};

class OperationComponent final : public RTT::TaskContext {
public:
  OperationComponent() : RTT::TaskContext("calculator") {
    addOperation("add", &OperationComponent::add, this, RTT::OwnThread)
        .doc("Add two signed values.")
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");
    addOperation("increment", &OperationComponent::increment, this,
                 RTT::OwnThread)
        .doc("Increment a value in place.")
        .arg("value", "Value to increment.");
    addOperation("onOwnerThread", &OperationComponent::onOwnerThread, this,
                 RTT::OwnThread)
        .doc("Report whether RTT dispatched to the component engine.");
    addOperation("onGlobalEngine", &OperationComponent::onGlobalEngine, this,
                 RTT::ClientThread)
        .doc("Report whether RTT dispatched to the global engine.");
  }

  std::int32_t add(std::int32_t left, std::int32_t right) {
    return left + right;
  }

  void increment(std::int32_t &value) { ++value; }

  bool onOwnerThread() const { return engine()->isSelf(); }

  bool onGlobalEngine() const {
    return RTT::internal::GlobalEngine::Instance()->isSelf();
  }
};

class GatedOperationComponent final : public RTT::TaskContext {
public:
  GatedOperationComponent() : RTT::TaskContext("gated-operation") {
    addOperation("waitForRelease", &GatedOperationComponent::waitForRelease,
                 this, RTT::OwnThread)
        .doc("Wait until the test releases the operation gate.");
    provides()
        ->addSynchronousOperation("waitSynchronouslyForRelease",
                                  &GatedOperationComponent::waitForRelease,
                                  this)
        .doc("Wait synchronously until the test releases the operation gate.");
  }

  ~GatedOperationComponent() override { release(); }

  bool waitForRelease() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++invocation_count_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    ++completion_count_;
    condition_.notify_all();
    return true;
  }

  bool waitUntilEntered(
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return invocation_count_ > 0U; });
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

  std::size_t invocationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return invocation_count_;
  }

  std::size_t completionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completion_count_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool released_{false};
  std::size_t invocation_count_{0U};
  std::size_t completion_count_{0U};
};

class GatedOperationRelease final {
public:
  explicit GatedOperationRelease(GatedOperationComponent &component)
      : component_(component) {}

  ~GatedOperationRelease() { component_.release(); }

  GatedOperationRelease(const GatedOperationRelease &) = delete;
  GatedOperationRelease &operator=(const GatedOperationRelease &) = delete;

private:
  GatedOperationComponent &component_;
};

struct UnsupportedValue {
  std::int32_t value{0};
};

constexpr std::string_view kUnsupportedTypeName = "/test/UnsupportedValue";
constexpr std::string_view kMissingProtocolReason =
    "has no registered OPC UA protocol";

void registerUnsupportedValueType() {
  if (RTT::types::Types()->type(std::string(kUnsupportedTypeName)) != nullptr) {
    return;
  }
  BOOST_REQUIRE(RTT::types::Types()->addType(
      new RTT::types::TemplateTypeInfo<UnsupportedValue, false>(
          std::string(kUnsupportedTypeName))));
}

class UnsupportedResourceComponent final : public RTT::TaskContext {
public:
  UnsupportedResourceComponent()
      : RTT::TaskContext("unsupported-component"),
        unsupported_service(RTT::Service::Create("unsupported")) {
    unsupported_service
        ->addOperation("consume", &UnsupportedResourceComponent::consume, this,
                       RTT::ClientThread)
        .arg("value", "Unsupported input value.");
    unsupported_service->addOperation("produce",
                                      &UnsupportedResourceComponent::produce,
                                      this, RTT::ClientThread);
    unsupported_service->addProperty("UnsupportedProperty", property);
    unsupported_service->addAttribute("UnsupportedAttribute", attribute);
    unsupported_service->addPort(input);
    unsupported_service->addPort(output);
    provides()->addProperty("SupportedProperty", supported_property);
    BOOST_REQUIRE(provides()->addService(unsupported_service));
  }

  void removeUnsupportedService() { provides()->removeService("unsupported"); }

  bool consume(UnsupportedValue) const { return true; }
  UnsupportedValue produce() const { return UnsupportedValue{42}; }

  RTT::Service::shared_ptr unsupported_service;
  UnsupportedValue property{1};
  UnsupportedValue attribute{2};
  std::int32_t supported_property{3};
  RTT::InputPort<UnsupportedValue> input{"UnsupportedInput"};
  RTT::OutputPort<UnsupportedValue> output{"UnsupportedOutput"};
};

class CanonicalArrayComponent final : public RTT::TaskContext {
public:
  CanonicalArrayComponent() : RTT::TaskContext("canonical-arrays") {
    addProperty("Float64ArrayProperty", float64_property);
    addProperty("Int32ArrayProperty", int32_property);
    addProperty("StringArrayProperty", string_property);
    addAttribute("Float64ArrayAttribute", float64_attribute);
    addAttribute("Int32ArrayAttribute", int32_attribute);
    addAttribute("StringArrayAttribute", string_attribute);
  }

  std::vector<double> float64_property{1.0, 2.0};
  std::vector<std::int32_t> int32_property{3, 4};
  std::vector<std::string> string_property{"five", "six"};
  std::vector<double> float64_attribute{7.0, 8.0};
  std::vector<std::int32_t> int32_attribute{9, 10};
  std::vector<std::string> string_attribute{"eleven", "twelve"};
};

class StaticSnapshotComponent final : public RTT::TaskContext {
public:
  StaticSnapshotComponent()
      : RTT::TaskContext("arm/left"),
        motion(RTT::Service::Create("motion/raw", this)),
        limits(RTT::Service::Create("limits")),
        empty(RTT::Service::Create("empty")) {
    provides()->doc("Arm controller");
    addProperty("Gain", gain).doc("Controller gain");
    addAttribute("Status", status);
    addConstant("ModelName", model_name);
    addPort(feedback).doc("Measured feedback");
    addPort(command).doc("Requested command");
    addEventPort(trigger).doc("External trigger");
    motion->addProperty("Scale", scale).doc("Motion scale");
    motion->addAttribute("Mode", motion_mode);
    motion->addConstant("Units", motion_units);
    motion->addOperation("offset", &StaticSnapshotComponent::offset, this,
                         RTT::ClientThread)
        .arg("value", "Value to offset.");
    motion->addPort(motion_feedback).doc("Nested feedback");
    motion->addPort(motion_command).doc("Nested command");
    limits->addProperty("Maximum", maximum).doc("Maximum command");
    BOOST_REQUIRE(motion->addService(limits));
    empty->doc("Intentionally empty service");
    BOOST_REQUIRE(provides()->addService(empty));
  }

  std::int32_t offset(std::int32_t value) const { return value + scale; }

  std::int32_t gain{7};
  std::string status{"idle"};
  std::string model_name{"arm-v1"};
  RTT::OutputPort<double> feedback{"Feedback"};
  RTT::InputPort<std::uint16_t> command{
      "Command", RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)};
  RTT::InputPort<std::int32_t> trigger{"Trigger"};
  RTT::Service::shared_ptr motion;
  RTT::Service::shared_ptr limits;
  RTT::Service::shared_ptr empty;
  std::uint16_t scale{2U};
  std::string motion_mode{"automatic"};
  std::string motion_units{"counts"};
  RTT::OutputPort<std::int32_t> motion_feedback{"MotionFeedback"};
  RTT::InputPort<std::int32_t> motion_command{"MotionCommand"};
  std::int32_t maximum{100};
};

class DirectionlessPort final : public RTT::base::PortInterface {
public:
  explicit DirectionlessPort(const std::string &name)
      : RTT::base::PortInterface(name) {}

  bool connected() const override { return false; }
  const RTT::types::TypeInfo *getTypeInfo() const override {
    return RTT::types::Types()->type("Int32");
  }
  void disconnect() override {}
  bool disconnect(RTT::base::PortInterface *) override { return false; }
  RTT::base::PortInterface *clone() const override {
    return new DirectionlessPort(getName());
  }
  RTT::base::PortInterface *antiClone() const override {
    return new DirectionlessPort(getName());
  }
  bool connectTo(RTT::base::PortInterface *,
                 const RTT::ConnPolicy &) override {
    return false;
  }
  bool connectTo(RTT::base::PortInterface *) override { return false; }
  bool createStream(const RTT::ConnPolicy &) override { return false; }
  bool createConnection(
      RTT::internal::SharedConnectionBase::shared_ptr,
      const RTT::ConnPolicy &) override {
    return false;
  }
  bool addConnection(RTT::internal::ConnID *,
                     RTT::base::ChannelElementBase::shared_ptr,
                     const RTT::ConnPolicy &) override {
    return false;
  }
  RTT::base::ChannelElementBase *getEndpoint() const override {
    return nullptr;
  }
};

class DirectionlessPortComponent final : public RTT::TaskContext {
public:
  DirectionlessPortComponent()
      : RTT::TaskContext("directionless-port-component"),
        directionless("Directionless") {
    ports()->addLocalPort(directionless);
  }

  ~DirectionlessPortComponent() override {
    ports()->removeLocalPort(directionless.getName());
  }

  DirectionlessPort directionless;
};

class CyclicServiceComponent final : public RTT::TaskContext {
public:
  CyclicServiceComponent()
      : RTT::TaskContext("cyclic-services"),
        first(RTT::Service::Create("first")),
        second(RTT::Service::Create("second")) {
    BOOST_REQUIRE(provides()->addService(first));
    BOOST_REQUIRE(first->addService(second));
    second->setOwner(nullptr);
    BOOST_REQUIRE(second->addService(first));
  }

  ~CyclicServiceComponent() override { second->removeService("first"); }

  RTT::Service::shared_ptr first;
  RTT::Service::shared_ptr second;
};

class DeepServiceComponent final : public RTT::TaskContext {
public:
  DeepServiceComponent() : RTT::TaskContext("deep-services") {
    RTT::Service::shared_ptr parent = provides();
    for (std::size_t index = 0U; index < 33U; ++index) {
      auto child = RTT::Service::Create("level" + std::to_string(index));
      BOOST_REQUIRE(parent->addService(child));
      services.push_back(child);
      parent = std::move(child);
    }
  }

  std::vector<RTT::Service::shared_ptr> services;
};

class ReportedProviderService final : public RTT::Service {
public:
  ReportedProviderService(std::string name, ProviderNames reported_names)
      : RTT::Service(std::move(name)),
        reported_names_(std::move(reported_names)) {}

  ProviderNames getProviderNames() const override { return reported_names_; }

private:
  ProviderNames reported_names_;
};

class NullServiceMemberComponent final : public RTT::TaskContext {
public:
  NullServiceMemberComponent()
      : RTT::TaskContext("null-service-member"),
        malformed(new ReportedProviderService("malformed", {"missing"})) {
    BOOST_REQUIRE(provides()->addService(malformed));
  }

  RTT::Service::shared_ptr malformed;
};

class DuplicateInventoryPathComponent final : public RTT::TaskContext {
public:
  DuplicateInventoryPathComponent()
      : RTT::TaskContext("duplicate-inventory-path"),
        repeating(new ReportedProviderService("repeating",
                                              {"repeated", "repeated"})),
        repeated(RTT::Service::Create("repeated")) {
    BOOST_REQUIRE(repeating->addService(repeated));
    BOOST_REQUIRE(provides()->addService(repeating));
  }

  RTT::Service::shared_ptr repeating;
  RTT::Service::shared_ptr repeated;
};

class NonRetainingOutputComponent final : public RTT::TaskContext {
public:
  NonRetainingOutputComponent()
      : RTT::TaskContext("non-retaining-output"), output("Ephemeral", false) {
    addPort(output);
  }

  RTT::OutputPort<std::int32_t> output;
};

class CollisionComponent final : public RTT::TaskContext {
public:
  CollisionComponent() : RTT::TaskContext("collision-component") {
    addOperation("echo", &CollisionComponent::echo, this, RTT::ClientThread)
        .arg("value", "Value to echo.");
    addProperty("Earlier", earlier);
    addProperty("ZCollision", collision);
  }

  std::int32_t echo(std::int32_t value) const { return value; }

  std::int32_t earlier{17};
  std::int32_t collision{42};
};

class ThrowingInputPort final : public RTT::InputPort<std::int32_t> {
public:
  using RTT::InputPort<std::int32_t>::InputPort;

  RTT::base::PortInterface *antiClone() const override {
    throw std::runtime_error("intentional antiClone failure");
  }
};

class CallbackFailureComponent final : public RTT::TaskContext {
public:
  CallbackFailureComponent() : RTT::TaskContext("callback-failure-component") {
    addProperty("Earlier", earlier);
    addPort(throwing_input);
  }

  std::int32_t earlier{17};
  ThrowingInputPort throwing_input{"ThrowingInput"};
};

class ReentrantForeignReferencePort final
    : public RTT::InputPort<std::int32_t> {
public:
  ReentrantForeignReferencePort(RTT::opcua::Server &server,
                                ::opcua::NodeId candidate_parent,
                                ::opcua::NodeId foreign_id)
      : RTT::InputPort<std::int32_t>("ReentrantInput"), server_(server),
        candidate_parent_(std::move(candidate_parent)),
        foreign_id_(std::move(foreign_id)) {}

  RTT::base::PortInterface *antiClone() const override {
    std::string error;
    bool linked = false;
    const bool invoked = server_.invoke(
        [&](::opcua::Server &native) {
          linked = ::opcua::services::addReference(
                       native, candidate_parent_, foreign_id_,
                       ::opcua::ReferenceTypeId::HasComponent, true)
                       .isGood();
        },
        std::chrono::seconds(5), &error);
    if (!invoked || !linked) {
      throw std::runtime_error("failed to add reentrant foreign reference: " +
                               error);
    }
    throw std::runtime_error("intentional reentrant antiClone failure");
  }

private:
  RTT::opcua::Server &server_;
  ::opcua::NodeId candidate_parent_;
  ::opcua::NodeId foreign_id_;
};

class ReentrantRollbackComponent final : public RTT::TaskContext {
public:
  ReentrantRollbackComponent(RTT::opcua::Server &server,
                             std::uint16_t namespace_index,
                             ::opcua::NodeId foreign_id)
      : RTT::TaskContext("reentrant-rollback-component"),
        input(server,
              modelNodeId(
                  namespace_index,
                  {"components", "reentrant-rollback-component", "ports"}),
              std::move(foreign_id)) {
    addProperty("Earlier", earlier);
    addPort(input);
  }

  std::int32_t earlier{17};
  ReentrantForeignReferencePort input;
};

class ReentrantRollbackInspectionPort final
    : public RTT::InputPort<std::int32_t> {
public:
  ReentrantRollbackInspectionPort(RTT::opcua::Server &server,
                                  ::opcua::NodeId properties_id)
      : RTT::InputPort<std::int32_t>("ReentrantInspectionInput"),
        server_(server), properties_id_(std::move(properties_id)) {}

  RTT::base::PortInterface *antiClone() const override {
    std::string error;
    bool removed = false;
    const bool invoked = server_.invoke(
        [&](::opcua::Server &native) {
          removed = ::opcua::services::deleteNode(native, properties_id_, true)
                        .isGood();
        },
        std::chrono::seconds(5), &error);
    if (!invoked || !removed) {
      throw std::runtime_error(
          "failed to remove the reentrant properties folder: " + error);
    }
    throw std::runtime_error("intentional rollback inspection failure");
  }

private:
  RTT::opcua::Server &server_;
  ::opcua::NodeId properties_id_;
};

class RollbackInspectionFailureComponent final : public RTT::TaskContext {
public:
  RollbackInspectionFailureComponent(RTT::opcua::Server &server,
                                     std::uint16_t namespace_index)
      : RTT::TaskContext("rollback-inspection-failure-component"),
        input(server, modelNodeId(namespace_index,
                                  {"components", getName(), "properties"})) {
    addProperty("Earlier", earlier);
    addPort(input);
  }

  std::int32_t earlier{17};
  ReentrantRollbackInspectionPort input;
};

} // namespace

BOOST_AUTO_TEST_CASE(publication_diagnostic_messages_are_stable) {
  using RTT::opcua::PublicationDiagnostic;
  using RTT::opcua::PublicationDiagnosticKind;

  const std::array<std::pair<PublicationDiagnostic, std::string_view>, 6U>
      cases{{
          {{PublicationDiagnosticKind::malformed_selector, "C", "S", {},
            "malformed reason."},
           "OPC UA publication: component 'C' rejected selector 'S': "
           "malformed reason."},
          {{PublicationDiagnosticKind::unmatched_selector, "C", "S", {},
            "ignored reason."},
           "OPC UA publication: component 'C' selector 'S' matched no RTT "
           "resource."},
          {{PublicationDiagnosticKind::inventory_failure, "C", {}, {},
            "inventory reason."},
           "OPC UA publication: component 'C' inventory failed: inventory "
           "reason."},
          {{PublicationDiagnosticKind::unsupported_resource, "C", {}, "P",
            "unsupported reason."},
           "OPC UA publication: component 'C' rejected resource 'P': "
           "unsupported reason."},
          {{PublicationDiagnosticKind::mandatory_resource, "C", {}, "P",
            "mandatory reason."},
           "OPC UA publication: component 'C' requires resource 'P': "
           "mandatory reason."},
          {{PublicationDiagnosticKind::publication_conflict, "C", {}, {},
            "conflict reason."},
           "OPC UA publication: component 'C' conflicts with its existing "
           "publication: conflict reason."},
      }};

  for (const auto &[diagnostic, expected] : cases) {
    BOOST_TEST(diagnostic.message() == expected);
  }
}

BOOST_FIXTURE_TEST_CASE(canonical_array_value_nodes_publish_and_remain_writable,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  CanonicalArrayComponent component;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto float64_property_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "Float64ArrayProperty"});
  const auto string_attribute_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "attributes", "StringArrayAttribute"});
  BOOST_TEST(::opcua::services::readValue(client, float64_property_id)
                     .value()
                     .to<std::vector<double>>() == component.float64_property,
             boost::test_tools::per_element());
  BOOST_TEST(
      ::opcua::services::writeValue(
          client, string_attribute_id,
          ::opcua::Variant(std::vector<std::string>{"updated", "attribute"}))
          .isGood());
  BOOST_REQUIRE(waitUntil([&] {
    return component.string_attribute ==
           std::vector<std::string>{"updated", "attribute"};
  }));

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(publish_component_rejects_invalid_options,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  RTT::TaskContext component("invalid-options-component");
  {
    RTT::opcua::ObjectModelOptions options;
    options.operation_timeout = std::chrono::milliseconds::zero();
    RTT::opcua::ObjectModel model(server, options);
    BOOST_TEST(!model.publishComponent(component, &error));
    BOOST_TEST(error == "object model operation timeout must be positive");
    BOOST_TEST(model.lastError() == error);
    BOOST_TEST(model.componentCount() == 0U);
    BOOST_TEST(model.revision() == 0U);
  }
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(non_retaining_output_omits_current_value_node,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  NonRetainingOutputComponent component;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto read_id =
      modelNodeId(namespace_index, {"components", component.getName(), "ports",
                                    "Ephemeral", "read"});
  const auto value_id =
      modelNodeId(namespace_index, {"components", component.getName(), "ports",
                                    "Ephemeral", "value"});
  requireMissingNode(client, read_id);
  requireMissingNode(client, value_id);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(publish_component_omits_empty_root_categories,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  OperationComponent component;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  BOOST_TEST(static_cast<bool>(::opcua::services::readNodeClass(
      client, modelNodeId(namespace_index, {"components", "calculator"}))));
  BOOST_TEST(static_cast<bool>(::opcua::services::readNodeClass(
      client, modelNodeId(namespace_index,
                          {"components", "calculator", "operations"}))));
  BOOST_TEST(static_cast<bool>(::opcua::services::readNodeClass(
      client, modelNodeId(namespace_index,
                          {"components", "calculator", "attributes"}))));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "calculator", "properties"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "calculator", "ports"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "calculator", "services"}));

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(publish_component_creates_one_complete_static_snapshot,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  StaticSnapshotComponent component;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  BOOST_TEST(model.componentCount() == 1U);
  BOOST_TEST(model.revision() == 1U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_id =
      modelNodeId(namespace_index, {"components", "arm/left"});
  const auto lifecycle_id = modelNodeId(
      namespace_index, {"components", "arm/left", "lifecycleState"});
  const auto operations_id = modelNodeId(
      namespace_index, {"components", "arm/left", "operations"});
  const auto get_task_state_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "operations", "getTaskState"});
  const auto get_target_state_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "operations", "getTargetState"});
  const auto get_task_state_types_id = modelNodeId(
      namespace_index, {"components", "arm/left", "operations",
                        "getTaskState", "rttOutputTypes"});
  const auto get_task_state_sources_id = modelNodeId(
      namespace_index, {"components", "arm/left", "operations",
                        "getTaskState", "rttOutputSources"});
  const auto get_target_state_types_id = modelNodeId(
      namespace_index, {"components", "arm/left", "operations",
                        "getTargetState", "rttOutputTypes"});
  const auto get_target_state_sources_id = modelNodeId(
      namespace_index, {"components", "arm/left", "operations",
                        "getTargetState", "rttOutputSources"});
  const auto gain_id = modelNodeId(
      namespace_index, {"components", "arm/left", "properties", "Gain"});
  const auto gain_type_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "properties", "Gain", "rttType"});
  const auto status_id = modelNodeId(
      namespace_index, {"components", "arm/left", "attributes", "Status"});
  const auto model_name_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "attributes", "ModelName"});
  const auto feedback_type_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Feedback", "type"});
  const auto feedback_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Feedback"});
  const auto feedback_read_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "ports", "Feedback", "read"});
  const auto feedback_value_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Feedback", "value"});
  const auto feedback_direction_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Feedback", "direction"});
  const auto trigger_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Trigger"});
  const auto trigger_direction_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Trigger", "direction"});
  const auto command_direction_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Command", "direction"});
  const auto command_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Command"});
  const auto command_value_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Command", "value"});
  const auto command_write_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "ports", "Command", "write"});
  const auto trigger_value_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Trigger", "value"});
  const auto trigger_write_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "ports", "Trigger", "write"});
  const auto command_service_operations_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "Command", "operations"});
  const auto command_service_read_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "Command",
                        "operations", "read"});
  const auto command_service_clear_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "Command",
                        "operations", "clear"});
  const auto feedback_service_operations_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "Feedback", "operations"});
  const auto feedback_service_write_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "Feedback",
                        "operations", "write"});
  const auto feedback_service_last_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "Feedback",
                        "operations", "last"});
  const auto trigger_service_read_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "Trigger",
                        "operations", "read"});
  const auto motion_operations_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "motion/raw",
                        "operations"});
  const auto motion_offset_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "motion/raw",
                        "operations", "offset"});
  const auto service_property_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "properties",
       "Scale"});
  const auto service_attribute_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "attributes",
       "Mode"});
  const auto service_constant_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "attributes",
       "Units"});
  const auto nested_output_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "ports",
       "MotionFeedback"});
  const auto nested_input_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "ports",
       "MotionCommand"});
  const auto motion_feedback_direction_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "ports",
       "MotionFeedback", "direction"});
  const auto motion_command_direction_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "services", "motion/raw", "ports",
       "MotionCommand", "direction"});
  const auto nested_port_service_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "motion/raw",
                        "services", "MotionFeedback", "operations", "last"});
  const auto deep_service_property_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "motion/raw",
                        "services", "limits", "properties", "Maximum"});
  const auto empty_service_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "empty"});
  const auto revision_id = modelNodeId(namespace_index, {"model", "revision"});

  BOOST_TEST(::opcua::services::readBrowseName(client, component_id)
                 .value()
                 .name() == "arm/left");
  requireMissingNode(client, lifecycle_id);
  BOOST_TEST(::opcua::services::readValue(client, get_task_state_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"TaskState"}));
  BOOST_TEST(::opcua::services::readValue(client, get_task_state_sources_id)
                 .value()
                 .to<std::vector<std::int32_t>>() ==
             std::vector<std::int32_t>({-1}));
  BOOST_TEST(::opcua::services::readValue(client, get_target_state_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"TaskState"}));
  BOOST_TEST(::opcua::services::readValue(client, get_target_state_sources_id)
                 .value()
                 .to<std::vector<std::int32_t>>() ==
             std::vector<std::int32_t>({-1}));
  for (const auto &[method_name, method_id] :
       std::array<std::pair<std::string_view, ::opcua::NodeId>, 2>{
           {{"getTaskState", get_task_state_id},
            {"getTargetState", get_target_state_id}}}) {
    requireMissingNode(
        client,
        modelNodeId(namespace_index,
                    {"components", "arm/left", "operations", method_name,
                     "rttInputTypes"}));
    const auto result =
        ::opcua::services::call(client, operations_id, method_id, {});
    BOOST_REQUIRE(result.statusCode().isGood());
    BOOST_REQUIRE_EQUAL(result.outputArguments().size(), 1U);
    BOOST_TEST(result.outputArguments()[0].isScalar());
    BOOST_TEST(result.outputArguments()[0].isType(
        ::opcua::NodeId(::opcua::DataTypeId::Int32)));
    BOOST_TEST(result.outputArguments()[0].to<std::int32_t>() == 4);
  }
  BOOST_TEST(::opcua::services::readValue(client, gain_id)
                 .value()
                 .to<std::int32_t>() == 7);
  BOOST_TEST(::opcua::services::readValue(client, gain_type_id)
                 .value()
                 .to<std::string>() == "Int32");
  BOOST_REQUIRE(::opcua::services::writeValue(
                    client, gain_id, ::opcua::Variant(std::int32_t{11}))
                    .isGood());
  BOOST_TEST(component.gain == 11);

  BOOST_REQUIRE(::opcua::services::writeValue(
                    client, status_id,
                    ::opcua::Variant(std::string("active")))
                    .isGood());
  BOOST_TEST(component.status == "active");
  BOOST_TEST(::opcua::services::writeValue(
                 client, status_id, ::opcua::Variant(std::int32_t{42})) ==
             UA_STATUSCODE_BADTYPEMISMATCH);
  BOOST_TEST(::opcua::services::readValue(client, model_name_id)
                 .value()
                 .to<std::string>() == "arm-v1");
  BOOST_TEST(::opcua::services::writeValue(
                 client, model_name_id,
                 ::opcua::Variant(std::string("unsafe"))) ==
             UA_STATUSCODE_BADNOTWRITABLE);
  BOOST_TEST(::opcua::services::readValue(client, feedback_type_id)
                 .value()
                 .to<std::string>() == "Float64");
  requirePortDirection(client, feedback_id, feedback_direction_id,
                       RTT::opcua::PortDirection::output);
  requirePortDirection(client, command_id, command_direction_id,
                       RTT::opcua::PortDirection::input);
  requirePortDirection(client, trigger_id, trigger_direction_id,
                       RTT::opcua::PortDirection::input);
  requirePortDirection(client, nested_output_id,
                       motion_feedback_direction_id,
                       RTT::opcua::PortDirection::output);
  requirePortDirection(client, nested_input_id, motion_command_direction_id,
                       RTT::opcua::PortDirection::input);
  requirePortValueSchema(client, command_value_id, ::opcua::DataTypeId::UInt16,
                         ::opcua::ValueRank::Scalar, true, true);
  requirePortValueSchema(client, trigger_value_id, ::opcua::DataTypeId::Int32,
                         ::opcua::ValueRank::Scalar, true, true);
  requirePortValueSchema(client, feedback_value_id, ::opcua::DataTypeId::Double,
                         ::opcua::ValueRank::Scalar, true, false);
  requireMissingNode(client, feedback_read_id);
  requireMissingNode(client, command_write_id);
  requireMissingNode(client, trigger_write_id);
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, command_service_read_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, command_service_clear_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, feedback_service_write_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, feedback_service_last_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, trigger_service_read_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, nested_output_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, nested_input_id)));
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readNodeClass(client, nested_port_service_id)));
  BOOST_TEST(::opcua::services::readValue(client, service_property_id)
                 .value()
                 .to<std::uint16_t>() == 2U);
  BOOST_TEST(::opcua::services::readValue(client, service_attribute_id)
                 .value()
                 .to<std::string>() == "automatic");
  BOOST_TEST(::opcua::services::readValue(client, service_constant_id)
                 .value()
                 .to<std::string>() == "counts");
  BOOST_TEST(::opcua::services::readValue(client, deep_service_property_id)
                 .value()
                 .to<std::int32_t>() == 100);
  BOOST_REQUIRE(::opcua::services::readDescription(client, empty_service_id));
  BOOST_TEST(::opcua::services::readDescription(client, empty_service_id)
                 ->text() == "Intentionally empty service");

  requireMissingNode(client, modelNodeId(
                                 namespace_index,
                                 {"components", "arm/left", "services",
                                  "Command", "properties"}));
  requireMissingNode(client, modelNodeId(
                                 namespace_index,
                                 {"components", "arm/left", "services",
                                  "Command", "attributes"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "Command", "ports"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "Command", "services"}));
  requireMissingNode(client, modelNodeId(
                                 namespace_index,
                                 {"components", "arm/left", "services",
                                  "Feedback", "properties"}));
  requireMissingNode(client, modelNodeId(
                                 namespace_index,
                                 {"components", "arm/left", "services",
                                  "Feedback", "attributes"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "Feedback", "ports"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "Feedback", "services"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "MotionFeedback",
                                  "properties"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "MotionFeedback",
                                  "attributes"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "MotionFeedback",
                                  "ports"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "MotionFeedback",
                                  "services"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "limits",
                                  "operations"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "limits",
                                  "attributes"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "limits",
                                  "ports"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "motion/raw", "services", "limits",
                                  "services"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "empty", "operations"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "empty", "properties"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "empty", "attributes"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "empty", "ports"}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", "arm/left", "services",
                                  "empty", "services"}));

  const auto offset_result = ::opcua::services::call(
      client, motion_operations_id, motion_offset_id,
      {::opcua::Variant(std::int32_t{40})});
  BOOST_REQUIRE(offset_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(offset_result.outputArguments().size(), 1U);
  BOOST_TEST(offset_result.outputArguments()[0].to<std::int32_t>() == 42);

  const auto unwritten_feedback = ::opcua::services::readAttribute(
      client, feedback_value_id, ::opcua::AttributeId::Value,
      ::opcua::TimestampsToReturn::Neither);
  BOOST_REQUIRE(unwritten_feedback);
  BOOST_TEST(unwritten_feedback->status() ==
             UA_STATUSCODE_BADWAITINGFORINITIALDATA);

  const auto command_read = ::opcua::services::readAttribute(
      client, command_value_id, ::opcua::AttributeId::Value,
      ::opcua::TimestampsToReturn::Neither);
  BOOST_REQUIRE(command_read);
  BOOST_TEST(command_read->status() == UA_STATUSCODE_BADWAITINGFORINITIALDATA);

  ::opcua::SubscriptionParameters subscription_parameters;
  subscription_parameters.publishingInterval = 10.0;
  ::opcua::Subscription<::opcua::Client> subscription(client,
                                                      subscription_parameters);
  std::optional<::opcua::StatusCode> input_monitor_status;
  std::optional<std::uint16_t> input_monitor_value;
  auto input_monitor = subscription.subscribeDataChange(
      command_value_id, ::opcua::AttributeId::Value,
      [&](::opcua::IntegerId, ::opcua::IntegerId,
          const ::opcua::DataValue &data) {
        input_monitor_status = data.status();
        if (data.status().isGood() && data.hasValue()) {
          input_monitor_value = data.value().to<std::uint16_t>();
        }
      });
  BOOST_REQUIRE(waitUntil([&] {
    client.runIterate(10U);
    return input_monitor_status.has_value();
  }));
  BOOST_TEST(*input_monitor_status == UA_STATUSCODE_BADWAITINGFORINITIALDATA);

  std::mutex notification_mutex;
  std::vector<double> notifications;
  ::opcua::MonitoringParametersEx monitoring_parameters;
  monitoring_parameters.samplingInterval = 10.0;
  monitoring_parameters.queueSize = 1U;
  monitoring_parameters.discardOldest = true;
  auto feedback_monitor = subscription.subscribeDataChange(
      feedback_value_id, ::opcua::AttributeId::Value,
      ::opcua::MonitoringMode::Reporting, monitoring_parameters,
      [&](::opcua::IntegerId, ::opcua::IntegerId,
          const ::opcua::DataValue &data) {
        if (!data.hasValue() || data.status().isBad()) {
          return;
        }
        const std::lock_guard<std::mutex> lock(notification_mutex);
        notifications.push_back(data.value().to<double>());
      });

  BOOST_TEST(component.feedback.write(4.25) == RTT::WriteSuccess);
  BOOST_TEST(component.feedback.write(5.25) == RTT::WriteSuccess);
  BOOST_TEST(component.feedback.write(6.25) == RTT::WriteSuccess);
  BOOST_TEST(::opcua::services::readValue(client, feedback_value_id)
                 .value()
                 .to<double>() == 6.25);
  BOOST_TEST(::opcua::services::readValue(client, feedback_value_id)
                 .value()
                 .to<double>() == 6.25);
  BOOST_TEST(::opcua::services::writeValue(client, feedback_value_id,
                                           ::opcua::Variant(7.25)) ==
             UA_STATUSCODE_BADNOTWRITABLE);
  BOOST_REQUIRE(waitUntil([&] {
    client.runIterate(10U);
    const std::lock_guard<std::mutex> lock(notification_mutex);
    return !notifications.empty() && notifications.back() == 6.25;
  }));

  BOOST_TEST(::opcua::services::writeValue(
                 client, command_value_id,
                 ::opcua::Variant(std::string("not-an-integer"))) ==
             UA_STATUSCODE_BADTYPEMISMATCH);
  BOOST_TEST(::opcua::services::writeValue(
                 client, command_value_id,
                 ::opcua::Variant(std::vector<std::uint16_t>{73U})) ==
             UA_STATUSCODE_BADTYPEMISMATCH);
  const auto unwritten_command = ::opcua::services::readAttribute(
      client, command_value_id, ::opcua::AttributeId::Value,
      ::opcua::TimestampsToReturn::Neither);
  BOOST_REQUIRE(unwritten_command);
  BOOST_TEST(unwritten_command->status() ==
             UA_STATUSCODE_BADWAITINGFORINITIALDATA);
  std::uint16_t commanded_value = 0U;
  BOOST_TEST(component.command.read(commanded_value) == RTT::NoData);

  BOOST_REQUIRE(
      ::opcua::services::writeValue(client, command_value_id,
                                    ::opcua::Variant(std::uint16_t{73U}))
          .isGood());
  BOOST_REQUIRE(component.command.read(commanded_value) == RTT::NewData);
  BOOST_TEST(commanded_value == 73U);
  BOOST_TEST(::opcua::services::readValue(client, command_value_id)
                 .value()
                 .to<std::uint16_t>() == 73U);
  BOOST_REQUIRE(waitUntil([&] {
    client.runIterate(10U);
    return input_monitor_value == 73U;
  }));
  BOOST_REQUIRE(
      ::opcua::services::writeValue(client, command_value_id,
                                    ::opcua::Variant(std::uint16_t{73U}))
          .isGood());
  BOOST_REQUIRE(component.command.read(commanded_value) == RTT::NewData);
  BOOST_TEST(commanded_value == 73U);
  BOOST_TEST(::opcua::services::readValue(client, command_value_id)
                 .value()
                 .to<std::uint16_t>() == 73U);
  BOOST_REQUIRE(
      ::opcua::services::writeValue(client, command_value_id,
                                    ::opcua::Variant(std::uint16_t{74U}))
          .isGood());
  BOOST_REQUIRE(component.command.read(commanded_value) == RTT::NewData);
  BOOST_TEST(commanded_value == 74U);
  BOOST_TEST(::opcua::services::readValue(client, command_value_id)
                 .value()
                 .to<std::uint16_t>() == 74U);

  const auto adapter_read_result = ::opcua::services::call(
      client, command_service_operations_id, command_service_read_id,
      {::opcua::Variant(std::uint16_t{0})});
  BOOST_REQUIRE(adapter_read_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(adapter_read_result.outputArguments().size(), 2U);
  BOOST_TEST(adapter_read_result.outputArguments()[0].to<std::int32_t>() ==
             static_cast<std::int32_t>(RTT::OldData));
  BOOST_TEST(adapter_read_result.outputArguments()[1].to<std::uint16_t>() ==
             74U);
  const auto adapter_clear_result = ::opcua::services::call(
      client, command_service_operations_id, command_service_clear_id, {});
  BOOST_REQUIRE(adapter_clear_result.statusCode().isGood());
  BOOST_TEST(component.command.read(commanded_value) == RTT::NoData);

  component.command.disconnect();
  BOOST_TEST(
      ::opcua::services::writeValue(client, command_value_id,
                                    ::opcua::Variant(std::uint16_t{75U})) ==
      UA_STATUSCODE_BADNOTCONNECTED);
  BOOST_TEST(::opcua::services::readValue(client, command_value_id)
                 .value()
                 .to<std::uint16_t>() == 74U);

  const auto adapter_write_result = ::opcua::services::call(
      client, feedback_service_operations_id, feedback_service_write_id,
      {::opcua::Variant(8.25)});
  BOOST_REQUIRE(adapter_write_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(adapter_write_result.outputArguments().size(), 1U);
  BOOST_TEST(adapter_write_result.outputArguments()[0].to<std::int32_t>() ==
             static_cast<std::int32_t>(RTT::WriteSuccess));
  const auto adapter_last_result = ::opcua::services::call(
      client, feedback_service_operations_id, feedback_service_last_id, {});
  BOOST_REQUIRE(adapter_last_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(adapter_last_result.outputArguments().size(), 1U);
  BOOST_TEST(adapter_last_result.outputArguments()[0].to<double>() == 8.25);
  BOOST_TEST(::opcua::services::readValue(client, feedback_value_id)
                 .value()
                 .to<double>() == 8.25);
  BOOST_TEST(::opcua::services::readValue(client, revision_id)
                 .value()
                 .to<std::uint64_t>() == 1U);

  input_monitor.deleteMonitoredItem();
  feedback_monitor.deleteMonitoredItem();
  subscription.deleteSubscription();
  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(port_values_reject_closed_component_state,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  StaticSnapshotComponent component;
  {
    RTT::opcua::ObjectModel model(server);
    BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  }

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto command_value_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Command", "value"});
  const auto feedback_value_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Feedback", "value"});
  BOOST_TEST(
      ::opcua::services::writeValue(client, command_value_id,
                                    ::opcua::Variant(std::uint16_t{73U})) ==
      UA_STATUSCODE_BADNOTCONNECTED);
  const auto feedback = ::opcua::services::readAttribute(
      client, feedback_value_id, ::opcua::AttributeId::Value,
      ::opcua::TimestampsToReturn::Neither);
  BOOST_REQUIRE(feedback);
  BOOST_TEST(feedback->status() == UA_STATUSCODE_BADNOTCONNECTED);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(directionless_port_rejects_the_whole_component,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  DirectionlessPortComponent component;
  RTT::opcua::ObjectModel model(server);
  std::vector<RTT::opcua::UnsupportedResource> diagnostics;
  BOOST_TEST(!model.publishComponent(component, &error, &diagnostics));
  BOOST_REQUIRE_EQUAL(diagnostics.size(), 1U);
  BOOST_TEST(diagnostics.front().path == "Directionless");
  BOOST_TEST(diagnostics.front().kind == "port");
  BOOST_TEST(diagnostics.front().type_name == "Int32");
  BOOST_TEST(diagnostics.front().reason ==
             "matches neither RTT input nor output interface");
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  requireMissingNode(
      client, modelNodeId(*server.namespaceIndex(),
                          {"components", component.getName()}));
  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(service_cycles_reject_the_whole_component,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  CyclicServiceComponent component;
  RTT::opcua::ObjectModel model(server);
  std::vector<RTT::opcua::UnsupportedResource> diagnostics;
  BOOST_TEST(!model.publishComponent(component, &error, &diagnostics));
  BOOST_REQUIRE_EQUAL(diagnostics.size(), 1U);
  BOOST_TEST(diagnostics[0].path == "first.second.first");
  BOOST_TEST(diagnostics[0].kind == "service");
  BOOST_TEST(diagnostics[0].reason.find("cycle") != std::string::npos);
  const std::vector<RTT::opcua::PublicationDiagnostic> expected{
      {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
       component.getName(), {},
       "services/first/services/second/services/first",
       "service uses RTT type 'RTT::Service' which forms a cycle in the RTT "
       "service graph"}};
  BOOST_TEST(model.publicationDiagnostics(component.getName()) == expected,
             boost::test_tools::per_element());
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(service_depth_overflow_rejects_the_whole_component,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  DeepServiceComponent component;
  RTT::opcua::ObjectModel model(server);
  std::vector<RTT::opcua::UnsupportedResource> diagnostics;
  BOOST_TEST(!model.publishComponent(component, &error, &diagnostics));
  BOOST_REQUIRE_EQUAL(diagnostics.size(), 1U);
  BOOST_TEST(diagnostics[0].path.starts_with("level0.level1"));
  BOOST_TEST(diagnostics[0].path.ends_with("level32"));
  BOOST_TEST(diagnostics[0].kind == "service");
  BOOST_TEST(diagnostics[0].reason.find("depth") != std::string::npos);
  std::string canonical_path;
  for (std::size_t index = 0U; index < 33U; ++index) {
    if (!canonical_path.empty()) {
      canonical_path += '/';
    }
    canonical_path += "services/level" + std::to_string(index);
  }
  const std::vector<RTT::opcua::PublicationDiagnostic> expected{
      {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
       component.getName(), {}, canonical_path,
       "service uses RTT type 'RTT::Service' which exceeds the maximum "
       "supported service depth of 32"}};
  BOOST_TEST(model.publicationDiagnostics(component.getName()) == expected,
             boost::test_tools::per_element());
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(null_service_member_has_a_canonical_diagnostic,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  NullServiceMemberComponent component;
  RTT::opcua::ObjectModel model(server);
  std::vector<RTT::opcua::UnsupportedResource> unsupported;
  BOOST_TEST(!model.publishComponent(component, &error, &unsupported));
  const std::vector<RTT::opcua::UnsupportedResource> expected_unsupported{
      {component.getName(), "malformed.missing", "service", "RTT::Service",
       "RTT service is unavailable during inventory discovery"}};
  BOOST_TEST(unsupported == expected_unsupported,
             boost::test_tools::per_element());
  const std::vector<RTT::opcua::PublicationDiagnostic> expected_diagnostics{
      {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
       component.getName(), {}, "services/malformed/services/missing",
       "service uses RTT type 'RTT::Service' which RTT service is unavailable "
       "during inventory discovery"}};
  BOOST_TEST(model.publicationDiagnostics(component.getName()) ==
                 expected_diagnostics,
             boost::test_tools::per_element());
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(duplicate_inventory_path_is_an_inventory_failure,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  DuplicateInventoryPathComponent component;
  RTT::opcua::ObjectModel model(server);
  std::vector<RTT::opcua::UnsupportedResource> unsupported;
  BOOST_TEST(!model.publishComponent(component, &error, &unsupported));
  BOOST_TEST(unsupported.empty());
  BOOST_TEST(model.unsupportedResources(component.getName()).empty());
  const std::vector<RTT::opcua::PublicationDiagnostic> expected{
      {RTT::opcua::PublicationDiagnosticKind::inventory_failure,
       component.getName(), {}, {},
       "duplicate canonical resource path "
       "'services/repeating/services/repeated'"}};
  BOOST_TEST(model.publicationDiagnostics(component.getName()) == expected,
             boost::test_tools::per_element());
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(missing_mandatory_operation_rejects_the_whole_component,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  RTT::TaskContext component("missing-mandatory-operation");
  component.provides()->removeOperation("getTaskState");
  RTT::opcua::ObjectModel model(server);
  std::vector<RTT::opcua::UnsupportedResource> unsupported;
  BOOST_TEST(!model.publishComponent(component, &error, &unsupported));
  BOOST_TEST(error.starts_with("strict OPC UA publication rejected component"));
  BOOST_TEST(model.lastError() == error);
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);
  BOOST_TEST(unsupported.empty());
  BOOST_TEST(model.unsupportedResources(component.getName()).empty());

  const std::vector<RTT::opcua::PublicationDiagnostic> expected{
      {RTT::opcua::PublicationDiagnosticKind::mandatory_resource,
       component.getName(), {}, "operations/getTaskState",
       "mandatory RTT proxy operation is unavailable"}};
  BOOST_TEST(model.publicationDiagnostics(component.getName()) == expected,
             boost::test_tools::per_element());

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(publish_component_is_idempotent_for_the_same_instance,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  RTT::TaskContext component("idempotent-component");
  std::int32_t value{7};
  component.addProperty("Value", value);
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  const std::uint64_t first_revision = model.revision();
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);

  BOOST_TEST(first_revision == 1U);
  BOOST_TEST(model.revision() == first_revision);
  BOOST_TEST(model.componentCount() == 1U);
  BOOST_TEST(error.empty());
  BOOST_TEST(model.lastError().empty());

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    publish_component_rejects_a_different_instance_with_the_same_name,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::TaskContext first("duplicate-component");
  RTT::TaskContext second("duplicate-component");
  std::int32_t first_value{7};
  std::int32_t second_value{99};
  first.addProperty("Value", first_value);
  second.addProperty("Value", second_value);
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(first, &error), error);
  const std::uint64_t first_revision = model.revision();

  BOOST_TEST(!model.publishComponent(second, &error));
  BOOST_TEST(error.find("different RTT component instance") !=
             std::string::npos);
  BOOST_TEST(model.lastError() == error);
  BOOST_TEST(model.revision() == first_revision);
  BOOST_TEST(model.componentCount() == 1U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto value_id = modelNodeId(
      namespace_index,
      {"components", first.getName(), "properties", "Value"});
  BOOST_TEST(::opcua::services::readValue(client, value_id)
                 .value()
                 .to<std::int32_t>() == first_value);

  BOOST_REQUIRE_MESSAGE(model.publishComponent(first, &error), error);
  BOOST_TEST(model.revision() == first_revision);
  BOOST_TEST(model.lastError().empty());

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(unsupported_resource_rejects_the_whole_component,
                        CanonicalTypesFixture) {
  registerUnsupportedValueType();

  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  std::vector<std::string> messages;
  RTT::opcua::ObjectModelOptions options;
  options.warning_sink = [&messages](const std::string &message) {
    messages.push_back(message);
  };
  UnsupportedResourceComponent component;
  RTT::opcua::ObjectModel model(server, options);
  std::vector<RTT::opcua::UnsupportedResource> diagnostics;
  BOOST_TEST(!model.publishComponent(component, &error, &diagnostics));
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);
  BOOST_TEST(diagnostics.size() == 9U);
  BOOST_REQUIRE_EQUAL(diagnostics.size(), 9U);
  BOOST_TEST(model.unsupportedResources(component.getName()) == diagnostics,
             boost::test_tools::per_element());
  BOOST_TEST(std::ranges::is_sorted(diagnostics));
  BOOST_TEST(error.starts_with("strict OPC UA publication rejected component"));
  BOOST_TEST(model.lastError() == error);

  const std::vector<std::pair<std::string, std::string>> expected_resources{
      {"unsupported.UnsupportedAttribute", "attribute"},
      {"unsupported.UnsupportedInput", "input port"},
      {"unsupported.UnsupportedInput.read", "operation"},
      {"unsupported.UnsupportedOutput", "output port"},
      {"unsupported.UnsupportedOutput.last", "operation"},
      {"unsupported.UnsupportedOutput.write", "operation"},
      {"unsupported.UnsupportedProperty", "property"},
      {"unsupported.consume", "operation"},
      {"unsupported.produce", "operation"},
  };
  const std::vector<RTT::opcua::PublicationDiagnostic>
      expected_publication_diagnostics{
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/attributes/UnsupportedAttribute",
           "attribute uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/operations/consume",
           "operation uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/operations/produce",
           "operation uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/ports/UnsupportedInput",
           "input port uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/ports/UnsupportedOutput",
           "output port uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/properties/UnsupportedProperty",
           "property uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/services/UnsupportedInput/operations/read",
           "operation uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/services/UnsupportedOutput/operations/last",
           "operation uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
          {RTT::opcua::PublicationDiagnosticKind::unsupported_resource,
           component.getName(), {},
           "services/unsupported/services/UnsupportedOutput/operations/write",
           "operation uses RTT type '/test/UnsupportedValue' which has no "
           "registered OPC UA protocol"},
      };
  const auto publication_diagnostics =
      model.publicationDiagnostics(component.getName());
  BOOST_TEST(publication_diagnostics == expected_publication_diagnostics,
             boost::test_tools::per_element());
  BOOST_TEST(std::ranges::is_sorted(publication_diagnostics));
  BOOST_TEST(messages.size() == publication_diagnostics.size());
  for (std::size_t index = 0U; index < diagnostics.size(); ++index) {
    const auto &diagnostic = diagnostics[index];
    BOOST_TEST(diagnostic.component == component.getName());
    BOOST_TEST(diagnostic.path == expected_resources[index].first);
    BOOST_TEST(diagnostic.kind == expected_resources[index].second);
    BOOST_TEST(diagnostic.type_name == kUnsupportedTypeName);
    BOOST_TEST(diagnostic.reason == kMissingProtocolReason);
    BOOST_TEST(diagnostic.message().find(" rejected ") != std::string::npos);
    BOOST_TEST(diagnostic.message().find(" skipped ") == std::string::npos);
    BOOST_TEST(messages[index] == publication_diagnostics[index].message());
  }

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_root_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto supported_property_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "SupportedProperty"});
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_root_id));
  BOOST_TEST(
      !::opcua::services::readBrowseName(client, supported_property_id));

  component.removeUnsupportedService();
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  BOOST_TEST(model.componentCount() == 1U);
  BOOST_TEST(model.revision() == 1U);
  BOOST_TEST(model.unsupportedResources(component.getName()).empty());
  BOOST_TEST(model.publicationDiagnostics(component.getName()).empty());
  BOOST_TEST(model.lastError().empty());

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(creation_failure_rolls_back_only_candidate_nodes,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  CollisionComponent component;
  const auto foreign_id = modelNodeId(
      namespace_index,
      {"components", component.getName(), "properties", "ZCollision"});
  bool seeded = false;
  BOOST_REQUIRE(server.invoke([&](::opcua::Server &native) {
    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(
        ::opcua::LocalizedText("en-US", "ForeignCollision"));
    attributes.setValue(::opcua::Variant(std::int32_t{99}));
    attributes.setDataType(::opcua::DataTypeId::Int32);
    attributes.setValueRank(::opcua::ValueRank::Scalar);
    seeded = static_cast<bool>(::opcua::services::addVariable(
        native, ::opcua::ObjectId::ObjectsFolder, foreign_id,
        "ForeignCollision", attributes,
        ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::Organizes));
  }));
  BOOST_REQUIRE(seeded);

  std::size_t argument_nodes_before = 0U;
  BOOST_REQUIRE(server.invoke([&](::opcua::Server &native) {
    argument_nodes_before = generatedMethodArgumentCount(native);
  }));

  RTT::opcua::ObjectModel model(server);
  BOOST_TEST(!model.publishComponent(component, &error));
  BOOST_TEST(error.find("BadNodeIdExists") != std::string::npos);
  BOOST_TEST(model.lastError() == error);
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_root_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto earlier_property_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "Earlier"});
  const auto echo_id =
      modelNodeId(namespace_index,
                  {"components", component.getName(), "operations", "echo"});
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_root_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, earlier_property_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, echo_id));
  BOOST_TEST(::opcua::services::readValue(client, foreign_id)
                 .value()
                 .to<std::int32_t>() == 99);
  BOOST_TEST(::opcua::services::readBrowseName(client, foreign_id)
                 .value()
                 .name() == "ForeignCollision");
  BOOST_TEST(hasHierarchicalReference(
      client, ::opcua::NodeId(::opcua::ObjectId::ObjectsFolder), foreign_id,
      ::opcua::BrowseDirection::Forward));
  std::size_t argument_nodes_after = 0U;
  BOOST_REQUIRE(server.invoke([&](::opcua::Server &native) {
    argument_nodes_after = generatedMethodArgumentCount(native);
  }));
  BOOST_TEST(argument_nodes_after == argument_nodes_before);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(rollback_never_deletes_a_reentrant_foreign_descendant,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();
  const ::opcua::NodeId foreign_id(namespace_index, "foreign-reentrant-node");
  bool seeded = false;
  BOOST_REQUIRE(server.invoke([&](::opcua::Server &native) {
    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", "Foreign"));
    attributes.setValue(::opcua::Variant(std::int32_t{99}));
    attributes.setDataType(::opcua::DataTypeId::Int32);
    attributes.setValueRank(::opcua::ValueRank::Scalar);
    seeded = static_cast<bool>(::opcua::services::addVariable(
        native, ::opcua::ObjectId::ObjectsFolder, foreign_id, "Foreign",
        attributes, ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::Organizes));
  }));
  BOOST_REQUIRE(seeded);

  ReentrantRollbackComponent component(server, namespace_index, foreign_id);
  RTT::opcua::ObjectModel model(server);
  BOOST_TEST(!model.publishComponent(component, &error));
  BOOST_TEST(error.find("intentional reentrant antiClone failure") !=
             std::string::npos);
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_root_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto earlier_property_id =
      modelNodeId(namespace_index,
                  {"components", component.getName(), "properties", "Earlier"});
  const auto ports_id = modelNodeId(
      namespace_index, {"components", component.getName(), "ports"});
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_root_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, earlier_property_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, ports_id));
  BOOST_TEST(::opcua::services::readValue(client, foreign_id)
                 .value()
                 .to<std::int32_t>() == 99);
  BOOST_TEST(hasHierarchicalReference(
      client, ::opcua::NodeId(::opcua::ObjectId::ObjectsFolder), foreign_id,
      ::opcua::BrowseDirection::Forward));
  BOOST_TEST(!hasHierarchicalReference(client, foreign_id, ports_id,
                                       ::opcua::BrowseDirection::Inverse));

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(rollback_never_deletes_a_reentrant_foreign_sole_child,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();
  const ::opcua::NodeId foreign_id(namespace_index,
                                   "foreign-reentrant-sole-child");
  bool seeded_and_orphaned = false;
  BOOST_REQUIRE(server.invoke([&](::opcua::Server &native) {
    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", "Foreign"));
    attributes.setValue(::opcua::Variant(std::int32_t{99}));
    attributes.setDataType(::opcua::DataTypeId::Int32);
    attributes.setValueRank(::opcua::ValueRank::Scalar);
    const auto added = ::opcua::services::addVariable(
        native, ::opcua::ObjectId::ObjectsFolder, foreign_id, "Foreign",
        attributes, ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::Organizes);
    if (!added) {
      return;
    }
    seeded_and_orphaned =
        ::opcua::services::deleteReference(
            native, ::opcua::NodeId(::opcua::ObjectId::ObjectsFolder),
            foreign_id, ::opcua::NodeId(::opcua::ReferenceTypeId::Organizes),
            true, true)
            .isGood();
  }));
  BOOST_REQUIRE(seeded_and_orphaned);

  ReentrantRollbackComponent component(server, namespace_index, foreign_id);
  RTT::opcua::ObjectModel model(server);
  BOOST_TEST(!model.publishComponent(component, &error));
  BOOST_TEST(error.find("intentional reentrant antiClone failure") !=
             std::string::npos);
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_root_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto earlier_property_id =
      modelNodeId(namespace_index,
                  {"components", component.getName(), "properties", "Earlier"});
  const auto ports_id = modelNodeId(
      namespace_index, {"components", component.getName(), "ports"});
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_root_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, earlier_property_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, ports_id));
  const auto foreign_value = ::opcua::services::readValue(client, foreign_id);
  BOOST_REQUIRE(static_cast<bool>(foreign_value));
  BOOST_TEST(foreign_value.value().to<std::int32_t>() == 99);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(rollback_inspection_failure_is_fail_closed,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RollbackInspectionFailureComponent component(server, namespace_index);
  RTT::opcua::ObjectModel model(server);
  BOOST_TEST(!model.publishComponent(component, &error));
  BOOST_TEST(error.find("intentional rollback inspection failure") !=
             std::string::npos);
  BOOST_TEST_CONTEXT("publication error: " << error) {
    BOOST_TEST(error.find("rollback threw: BadNodeIdUnknown") !=
               std::string::npos);
  }
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto properties_id = modelNodeId(
      namespace_index, {"components", component.getName(), "properties"});
  const auto earlier_id =
      modelNodeId(namespace_index,
                  {"components", component.getName(), "properties", "Earlier"});
  const auto ports_id = modelNodeId(
      namespace_index, {"components", component.getName(), "ports"});
  const auto input_id =
      modelNodeId(namespace_index, {"components", component.getName(), "ports",
                                    "ReentrantInspectionInput"});
  BOOST_TEST(static_cast<bool>(
      ::opcua::services::readBrowseName(client, component_id)));
  BOOST_TEST(!::opcua::services::readBrowseName(client, properties_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, earlier_id));
  BOOST_TEST(
      static_cast<bool>(::opcua::services::readBrowseName(client, ports_id)));
  BOOST_TEST(
      static_cast<bool>(::opcua::services::readBrowseName(client, input_id)));

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(callback_failure_rolls_back_every_candidate_node,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  CallbackFailureComponent component;
  RTT::opcua::ObjectModel model(server);
  BOOST_TEST(!model.publishComponent(component, &error));
  BOOST_TEST(error.find("intentional antiClone failure") != std::string::npos);
  BOOST_TEST(model.lastError() == error);
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == 0U);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_root_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto earlier_property_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "Earlier"});
  const auto throwing_port_id =
      modelNodeId(namespace_index, {"components", component.getName(), "ports",
                                    "ThrowingInput"});
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_root_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, earlier_property_id));
  BOOST_TEST(!::opcua::services::readBrowseName(client, throwing_port_id));

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    resource_changes_after_publication_do_not_change_topology,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::TaskContext component("static-topology-component");
  component.provides()->doc("Original documentation");
  std::int32_t original_value{7};
  component.addProperty("Original", original_value);
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  const std::uint64_t published_revision = model.revision();

  std::int32_t late_value{23};
  component.addProperty("Late", late_value);
  component.provides()->doc("Changed documentation");
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto original_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "Original"});
  const auto late_id = modelNodeId(namespace_index,
                                   {"components", component.getName(),
                                    "properties", "Late"});
  BOOST_TEST(::opcua::services::readValue(client, original_id)
                 .value()
                 .to<std::int32_t>() == original_value);
  BOOST_TEST(!::opcua::services::readNodeClass(client, late_id));
  BOOST_TEST(::opcua::services::readDescription(client, component_id)
                 .value()
                 .text() == "Original documentation");
  BOOST_TEST(model.revision() == published_revision);
  BOOST_TEST(model.componentCount() == 1U);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(operations_dispatch_on_rtt_engines,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions options;
  options.operation_timeout = std::chrono::milliseconds(30);
  OperationComponent component;
  RTT::opcua::ObjectModel model(server, options);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());

  const auto operations_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations"});
  const auto add_id = modelNodeId(
      namespace_index, {"components", "calculator", "operations", "add"});
  const auto increment_id =
      modelNodeId(namespace_index,
                  {"components", "calculator", "operations", "increment"});
  const auto add_input_types_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "add", "rttInputTypes"});
  const auto add_output_types_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "add", "rttOutputTypes"});
  const auto add_output_sources_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "add", "rttOutputSources"});
  const auto increment_output_sources_id = modelNodeId(
      namespace_index, {"components", "calculator", "operations", "increment",
                        "rttOutputSources"});
  const auto owner_thread_id = modelNodeId(
      namespace_index,
      {"components", "calculator", "operations", "onOwnerThread"});
  const auto global_engine_id =
      modelNodeId(namespace_index,
                  {"components", "calculator", "operations", "onGlobalEngine"});

  BOOST_TEST(::opcua::services::readValue(client, add_input_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"Int32", "Int32"}));
  BOOST_TEST(::opcua::services::readValue(client, add_output_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"Int32"}));
  BOOST_TEST(::opcua::services::readValue(client, add_output_sources_id)
                 .value()
                 .to<std::vector<std::int32_t>>() ==
             std::vector<std::int32_t>({-1}));
  BOOST_TEST(::opcua::services::readValue(client, increment_output_sources_id)
                 .value()
                 .to<std::vector<std::int32_t>>() ==
             std::vector<std::int32_t>({0}));

  const std::vector<::opcua::Variant> add_inputs{
      ::opcua::Variant(std::int32_t{20}), ::opcua::Variant(std::int32_t{22})};
  const auto add_result =
      ::opcua::services::call(client, operations_id, add_id, add_inputs);
  BOOST_REQUIRE(add_result.statusCode().isGood());
  BOOST_TEST(add_result.outputArguments()[0].to<std::int32_t>() == 42);

  const std::vector<::opcua::Variant> increment_inputs{
      ::opcua::Variant(std::int32_t{4})};
  const auto increment_result = ::opcua::services::call(
      client, operations_id, increment_id, increment_inputs);
  BOOST_REQUIRE(increment_result.statusCode().isGood());
  BOOST_TEST(increment_result.outputArguments()[0].to<std::int32_t>() == 5);

  const auto owner_thread_result =
      ::opcua::services::call(client, operations_id, owner_thread_id, {});
  BOOST_REQUIRE(owner_thread_result.statusCode().isGood());
  BOOST_TEST(owner_thread_result.outputArguments()[0].to<bool>());
  const auto global_engine_result =
      ::opcua::services::call(client, operations_id, global_engine_id, {});
  BOOST_REQUIRE(global_engine_result.statusCode().isGood());
  BOOST_TEST(global_engine_result.outputArguments()[0].to<bool>());

  const std::vector<::opcua::Variant> wrong_inputs{
      ::opcua::Variant(std::string("not-an-integer")),
      ::opcua::Variant(std::int32_t{2})};
  BOOST_TEST(::opcua::services::call(client, operations_id, add_id, wrong_inputs)
                 .statusCode() == UA_STATUSCODE_BADINVALIDARGUMENT);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(timed_out_operation_is_reaped_without_graph_activity,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions options;
  options.operation_timeout = std::chrono::milliseconds(30);
  GatedOperationComponent component;
  RTT::opcua::ObjectModel model(server, options);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  GatedOperationRelease release_on_exit(component);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());
  const auto operations_id = modelNodeId(
      namespace_index, {"components", "gated-operation", "operations"});
  const auto operation_id =
      modelNodeId(namespace_index, {"components", "gated-operation",
                                    "operations", "waitForRelease"});

  const auto call_started = std::chrono::steady_clock::now();
  const auto result =
      ::opcua::services::call(client, operations_id, operation_id, {});
  const auto call_elapsed = std::chrono::steady_clock::now() - call_started;
  BOOST_TEST(result.statusCode() == UA_STATUSCODE_BADTIMEOUT);
  BOOST_TEST(call_elapsed < std::chrono::seconds(1));
  BOOST_REQUIRE(component.waitUntilEntered());
  BOOST_TEST(component.invocationCount() == 1U);
  BOOST_TEST(model.pendingOperationCount() == 1U);

  component.release();
  BOOST_REQUIRE(waitUntil([&] { return model.pendingOperationCount() == 0U; }));
  BOOST_TEST(component.completionCount() == 1U);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    timed_out_synchronous_operation_is_reaped_without_graph_activity,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions options;
  options.operation_timeout = std::chrono::milliseconds(30);
  GatedOperationComponent component;
  RTT::opcua::ObjectModel model(server, options);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(component, &error), error);
  GatedOperationRelease release_on_exit(component);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());
  const auto operations_id = modelNodeId(
      namespace_index, {"components", "gated-operation", "operations"});
  const auto operation_id = modelNodeId(
      namespace_index,
      {"components", "gated-operation", "operations",
       "waitSynchronouslyForRelease"});

  const auto call_started = std::chrono::steady_clock::now();
  const auto result =
      ::opcua::services::call(client, operations_id, operation_id, {});
  const auto call_elapsed = std::chrono::steady_clock::now() - call_started;
  BOOST_TEST(result.statusCode() == UA_STATUSCODE_BADTIMEOUT);
  BOOST_TEST(call_elapsed < std::chrono::seconds(1));
  BOOST_REQUIRE(component.waitUntilEntered());
  BOOST_TEST(component.invocationCount() == 1U);
  BOOST_TEST(model.pendingOperationCount() == 1U);

  component.release();
  BOOST_REQUIRE(waitUntil([&] { return model.pendingOperationCount() == 0U; }));
  BOOST_TEST(component.completionCount() == 1U);

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(shutdown_after_timeout_waits_for_operation_completion,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions options;
  options.operation_timeout = std::chrono::milliseconds(30);
  GatedOperationComponent component;
  auto model = std::make_unique<RTT::opcua::ObjectModel>(server, options);
  BOOST_REQUIRE_MESSAGE(model->publishComponent(component, &error), error);
  GatedOperationRelease release_on_exit(component);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());
  const auto operations_id = modelNodeId(
      namespace_index, {"components", "gated-operation", "operations"});
  const auto operation_id =
      modelNodeId(namespace_index, {"components", "gated-operation",
                                    "operations", "waitForRelease"});

  const auto call_started = std::chrono::steady_clock::now();
  const auto result =
      ::opcua::services::call(client, operations_id, operation_id, {});
  const auto call_elapsed = std::chrono::steady_clock::now() - call_started;
  BOOST_TEST(result.statusCode() == UA_STATUSCODE_BADTIMEOUT);
  BOOST_TEST(call_elapsed < std::chrono::seconds(1));
  BOOST_REQUIRE(component.waitUntilEntered());
  BOOST_TEST(component.invocationCount() == 1U);
  BOOST_TEST(model->pendingOperationCount() == 1U);

  client.disconnect();
  server.stop();

  std::jthread release_thread([&component] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    component.release();
  });
  const auto destruction_started = std::chrono::steady_clock::now();
  model.reset();
  const auto destruction_elapsed =
      std::chrono::steady_clock::now() - destruction_started;
  release_thread.join();

  BOOST_TEST(destruction_elapsed >= std::chrono::milliseconds(75));
  BOOST_TEST(destruction_elapsed < std::chrono::seconds(2));
  BOOST_TEST(component.invocationCount() == 1U);
  BOOST_TEST(component.completionCount() == 1U);
}
