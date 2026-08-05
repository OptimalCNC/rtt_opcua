#define BOOST_TEST_MODULE rtt_opcua_task_context_proxy
#include <boost/test/included/unit_test.hpp>

#include "custom_datatype_test_support.hpp"

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <rtt/FactoryExceptions.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Property.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/internal/GlobalEngine.hpp>
#include <rtt/internal/OperationCallerC.hpp>
#include <rtt/internal/SendHandleC.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/Types.hpp>

#include <open62541pp/services/nodemanagement.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using RTT::opcua::test::FixtureValue;

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

struct CanonicalTypesFixture {
  CanonicalTypesFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());
  }
};

struct CustomDatatypeFixture {
  CustomDatatypeFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    std::string error;
    if (!RTT::opcua::registerCanonicalTypeProtocols(&error) ||
        !RTT::opcua::test::registerFixtureType(&error)) {
      throw std::runtime_error(error);
    }
  }
};

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout =
                                        std::chrono::milliseconds(1000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

class ProxyTarget final : public RTT::TaskContext {
public:
  ProxyTarget()
      : RTT::TaskContext("remote/calculator",
                         RTT::TaskContext::PreOperational) {
    provides()->doc("Remote calculator.");
    addPort(feedback).doc("Calculated feedback.");
    addPort(command).doc("Requested command.");
    addProperty("Gain", gain).doc("Controller gain.");
    addAttribute("Status", status);
    addConstant("ModelName", model_name);
    addOperation("add", &ProxyTarget::add, this, RTT::OwnThread)
        .doc("Add two signed values.")
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");
    addOperation("increment", &ProxyTarget::increment, this, RTT::OwnThread)
        .doc("Increment a value in place.")
        .arg("value", "Value to increment.");
    addOperation("delayedAdd", &ProxyTarget::delayedAdd, this, RTT::OwnThread)
        .doc("Add two values after a short delay.")
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");

    RTT::Service::shared_ptr math = RTT::Service::Create("math");
    math->doc("Math utilities.");
    math->addLocalPort(math_feedback).doc("Nested calculation feedback.");
    math->addProperty("Offset", offset).doc("Scale offset.");
    math->addAttribute("Mode", mode);
    math->addConstant("Unit", unit);
    math->addOperation("scale", &ProxyTarget::scale, this, RTT::OwnThread)
        .doc("Scale a value and add the configured offset.")
        .arg("value", "Value to scale.")
        .arg("factor", "Scale factor.");
    RTT::Service::shared_ptr advanced = RTT::Service::Create("advanced");
    advanced->addOperation("negate", &ProxyTarget::negate, this, RTT::OwnThread)
        .doc("Negate a signed value.")
        .arg("value", "Value to negate.");
    BOOST_REQUIRE(math->addService(advanced));
    BOOST_REQUIRE(provides()->addService(math));
  }

  ~ProxyTarget() override {
    if (RTT::Service::shared_ptr math = provides()->getService("math")) {
      math->removeLocalPort(math_feedback.getName());
      math->clear();
    }
  }

  std::int32_t add(std::int32_t left, std::int32_t right) {
    return left + right;
  }

  void increment(std::int32_t &value) { ++value; }

  std::int32_t delayedAdd(std::int32_t left, std::int32_t right) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return left + right;
  }

  std::int32_t scale(std::int32_t value, std::int32_t factor) {
    return value * factor + offset;
  }

  std::int32_t negate(std::int32_t value) { return -value; }

  std::int32_t gain{7};
  std::int32_t offset{2};
  std::string status{"idle"};
  std::string model_name{"calculator-v1"};
  std::string mode{"manual"};
  std::string unit{"counts"};
  RTT::OutputPort<std::int32_t> feedback{"Feedback"};
  RTT::InputPort<std::int32_t> command{"Command"};
  RTT::OutputPort<std::int32_t> math_feedback{"MathFeedback"};
};

class CustomProxyTarget final : public RTT::TaskContext {
public:
  CustomProxyTarget()
      : RTT::TaskContext("remote/custom", RTT::TaskContext::PreOperational) {
    addProperty("Configured", configured);
    addAttribute("Observed", observed);
    addPort(feedback);
    addPort(command);
    addOperation("adjust", &CustomProxyTarget::adjust, this, RTT::OwnThread)
        .arg("value", "Value to adjust.");
  }

  FixtureValue adjust(FixtureValue value) {
    ++value.count;
    value.scale *= 2.0;
    return value;
  }

  FixtureValue configured{3, 1.5};
  FixtureValue observed{4, 2.5};
  RTT::OutputPort<FixtureValue> feedback{"Feedback"};
  RTT::InputPort<FixtureValue> command{"Command"};
};

} // namespace

BOOST_GLOBAL_FIXTURE(CustomDatatypeFixture);

BOOST_FIXTURE_TEST_CASE(proxy_calls_remote_operations_synchronously_and_async,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->ready());
  BOOST_TEST(proxy->getName() == target.getName());
  BOOST_TEST(proxy->provides()->doc() == "Remote calculator.");
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::connected);

  BOOST_TEST(proxy->isActive());
  BOOST_TEST(proxy->activate());
  BOOST_TEST(proxy->getPeriod() == target.getPeriod());
  BOOST_TEST(proxy->setPeriod(0.0));
  BOOST_TEST(proxy->setPeriod(0.01));
  BOOST_TEST(proxy->getPeriod() == 0.01, boost::test_tools::tolerance(0.001));
  BOOST_TEST(target.getPeriod() == 0.01, boost::test_tools::tolerance(0.001));
  BOOST_TEST(proxy->setPeriod(0.0));
  BOOST_TEST(proxy->getCpuAffinity() == target.getCpuAffinity());
  BOOST_TEST(!proxy->update());
  BOOST_TEST(proxy->trigger());

  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);
  BOOST_TEST(!proxy->isConfigured());
  BOOST_TEST(!proxy->isRunning());
  BOOST_TEST(proxy->configure());
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::Stopped);
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Stopped);
  BOOST_TEST(proxy->isConfigured());
  BOOST_TEST(proxy->start());
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->getTargetState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->isRunning());
  proxy->error();
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::RunTimeError);
  BOOST_TEST(proxy->inRunTimeError());
  BOOST_TEST(proxy->recover());
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->stop());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Stopped);
  BOOST_TEST(proxy->cleanup());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);

  auto *gain = dynamic_cast<RTT::Property<std::int32_t> *>(
      proxy->provides()->getProperty("Gain"));
  BOOST_REQUIRE(gain != nullptr);
  BOOST_TEST(gain->getDescription() == "Controller gain.");
  BOOST_TEST(gain->get() == 7);
  target.gain = 9;
  BOOST_TEST(gain->get() == 9);
  gain->set(11);
  BOOST_TEST(target.gain == 11);

  RTT::base::AttributeBase *status = proxy->provides()->getAttribute("Status");
  BOOST_REQUIRE(status != nullptr);
  auto *status_source =
      RTT::internal::AssignableDataSource<std::string>::narrow(
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

  auto *remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  BOOST_TEST(remote_feedback->getTypeInfo()->getTypeName() == "Int32");
  BOOST_TEST(remote_feedback->getDescription() == "Calculated feedback.");
  RTT::InputPort<std::int32_t> feedback_sink("FeedbackSink");
  RTT::ConnPolicy feedback_policy =
      RTT::ConnPolicy::buffer(1, RTT::ConnPolicy::LOCK_FREE, false);
  feedback_policy.mandatory = true;
  BOOST_REQUIRE(
      remote_feedback->createConnection(feedback_sink, feedback_policy));
  const RTT::base::DataSourceBase::shared_ptr filler =
      new RTT::internal::ConstantDataSource<std::int32_t>(40);
  BOOST_REQUIRE(remote_feedback->write(filler) == RTT::WriteSuccess);
  BOOST_TEST(target.feedback.write(std::int32_t{41}) == RTT::WriteSuccess);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::int32_t feedback_value = 0;
  BOOST_REQUIRE(feedback_sink.read(feedback_value) == RTT::NewData);
  BOOST_TEST(feedback_value == 40);
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 41);
  RTT::InputPort<std::int32_t> initialized_feedback_sink(
      "InitializedFeedbackSink");
  BOOST_REQUIRE(remote_feedback->createConnection(
      initialized_feedback_sink,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, true)));
  std::int32_t initialized_feedback_value = 0;
  BOOST_REQUIRE(initialized_feedback_sink.read(initialized_feedback_value) ==
                RTT::NewData);
  BOOST_TEST(initialized_feedback_value == 41);
  initialized_feedback_sink.disconnect();
  feedback_sink.disconnect();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  BOOST_REQUIRE(
      remote_feedback->createConnection(feedback_sink, feedback_policy));
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 41);

  auto *remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_TEST(remote_command->getTypeInfo()->getTypeName() == "Int32");
  BOOST_TEST(remote_command->getDescription() == "Requested command.");
  RTT::OutputPort<std::int32_t> command_source("CommandSource");
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(std::int32_t{73}) == RTT::WriteSuccess);
  std::int32_t command_value = 0;
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value == 73);

  BOOST_REQUIRE_MESSAGE(proxy->synchronize(&error), error);
  BOOST_TEST(!feedback_sink.connected());
  BOOST_TEST(!command_source.connected());

  remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  BOOST_REQUIRE(remote_feedback->createConnection(
      feedback_sink, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(target.feedback.write(std::int32_t{42}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 42);

  remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(std::int32_t{74}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value == 74);

  RTT::Service::shared_ptr math = proxy->provides()->getService("math");
  BOOST_REQUIRE(math);
  BOOST_TEST(math->doc() == "Math utilities.");
  auto *remote_math_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      math->getPort("MathFeedback"));
  BOOST_REQUIRE(remote_math_feedback != nullptr);
  BOOST_TEST(remote_math_feedback->getDescription() ==
             "Nested calculation feedback.");
  RTT::InputPort<std::int32_t> math_feedback_sink("MathFeedbackSink");
  BOOST_REQUIRE(remote_math_feedback->createConnection(
      math_feedback_sink,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(target.math_feedback.write(std::int32_t{84}) == RTT::WriteSuccess);
  std::int32_t math_feedback_value = 0;
  BOOST_REQUIRE(waitUntil([&] {
    return math_feedback_sink.read(math_feedback_value) == RTT::NewData;
  }));
  BOOST_TEST(math_feedback_value == 84);
  auto *offset =
      dynamic_cast<RTT::Property<std::int32_t> *>(math->getProperty("Offset"));
  BOOST_REQUIRE(offset != nullptr);
  RTT::base::DataSourceBase::shared_ptr cached_offset_source =
      offset->getDataSource();
  auto *cached_offset =
      RTT::internal::AssignableDataSource<std::int32_t>::narrow(
          cached_offset_source.get());
  BOOST_REQUIRE(cached_offset != nullptr);
  BOOST_TEST(offset->getDescription() == "Scale offset.");
  BOOST_TEST(offset->get() == 2);
  offset->set(3);
  BOOST_TEST(target.offset == 3);

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

  std::int32_t scaled = 0;
  RTT::OperationInterfacePart *scale = math->getOperation("scale");
  BOOST_REQUIRE(scale != nullptr);
  RTT::internal::OperationCallerC scale_caller(
      scale, "scale", RTT::internal::GlobalEngine::Instance());
  scale_caller.argC(std::int32_t{4}).argC(std::int32_t{5}).ret(scaled);
  scale_caller.check();
  BOOST_TEST(scale_caller.call());
  BOOST_TEST(scaled == 23);

  RTT::Service::shared_ptr advanced = math->getService("advanced");
  BOOST_REQUIRE(advanced);
  std::int32_t negated = 0;
  RTT::OperationInterfacePart *negate = advanced->getOperation("negate");
  BOOST_REQUIRE(negate != nullptr);
  RTT::internal::OperationCallerC negate_caller(
      negate, "negate", RTT::internal::GlobalEngine::Instance());
  negate_caller.argC(std::int32_t{9}).ret(negated);
  negate_caller.check();
  BOOST_TEST(negate_caller.call());
  BOOST_TEST(negated == -9);

  RTT::OperationInterfacePart *add = proxy->provides()->getOperation("add");
  BOOST_REQUIRE(add != nullptr);
  const std::vector<RTT::ArgumentDescription> add_arguments =
      add->getArgumentList();
  BOOST_REQUIRE_EQUAL(add_arguments.size(), 2U);
  BOOST_TEST(add_arguments[0].name == "left");
  BOOST_TEST(add_arguments[0].description == "Left operand.");
  BOOST_TEST(add_arguments[0].type == "Int32");
  BOOST_TEST(add_arguments[1].name == "right");
  BOOST_TEST(add_arguments[1].description == "Right operand.");
  BOOST_TEST(add_arguments[1].type == "Int32");
  const std::vector<RTT::base::DataSourceBase::shared_ptr> too_few_arguments{
      new RTT::internal::ConstantDataSource<std::int32_t>(1)};
  BOOST_CHECK_THROW(
      add->produce(too_few_arguments, RTT::internal::GlobalEngine::Instance()),
      RTT::wrong_number_of_args_exception);
  const std::vector<RTT::base::DataSourceBase::shared_ptr> wrong_arguments{
      new RTT::internal::ConstantDataSource<double>(1.0),
      new RTT::internal::ConstantDataSource<std::int32_t>(2)};
  BOOST_CHECK_THROW(
      add->produce(wrong_arguments, RTT::internal::GlobalEngine::Instance()),
      RTT::wrong_types_of_args_exception);

  std::int32_t sum = 0;
  RTT::internal::OperationCallerC add_caller(
      add, "add", RTT::internal::GlobalEngine::Instance());
  add_caller.argC(std::int32_t{20}).argC(std::int32_t{22}).ret(sum);
  add_caller.check();
  BOOST_REQUIRE(add_caller.ready());
  BOOST_TEST(add_caller.call());
  BOOST_TEST(sum == 42);

  const std::vector<RTT::base::DataSourceBase::shared_ptr> cached_add_arguments{
      new RTT::internal::ConstantDataSource<std::int32_t>(30),
      new RTT::internal::ConstantDataSource<std::int32_t>(12)};
  RTT::base::DataSourceBase::shared_ptr cached_add_call = add->produce(
      cached_add_arguments, RTT::internal::GlobalEngine::Instance());
  BOOST_REQUIRE(cached_add_call);

  RTT::OperationInterfacePart *increment =
      proxy->provides()->getOperation("increment");
  BOOST_REQUIRE(increment != nullptr);
  const std::vector<RTT::ArgumentDescription> increment_arguments =
      increment->getArgumentList();
  BOOST_REQUIRE_EQUAL(increment_arguments.size(), 1U);
  BOOST_TEST(increment_arguments[0].name == "value");
  BOOST_TEST(increment_arguments[0].description == "Value to increment.");
  BOOST_TEST(increment_arguments[0].type == "Int32 &");
  const std::vector<RTT::base::DataSourceBase::shared_ptr>
      constant_increment_argument{
          new RTT::internal::ConstantDataSource<std::int32_t>(4)};
  BOOST_CHECK_THROW(increment->produce(constant_increment_argument,
                                       RTT::internal::GlobalEngine::Instance()),
                    RTT::non_lvalue_args_exception);

  std::int32_t value = 4;
  RTT::internal::OperationCallerC increment_caller(
      increment, "increment", RTT::internal::GlobalEngine::Instance());
  increment_caller.arg(value);
  increment_caller.check();
  BOOST_TEST(increment_caller.call());
  BOOST_TEST(value == 5);

  std::int32_t async_sum = 0;
  RTT::internal::OperationCallerC async_caller(
      add, "add", RTT::internal::GlobalEngine::Instance());
  async_caller.argC(std::int32_t{8}).argC(std::int32_t{9});
  async_caller.check();
  RTT::internal::SendHandleC handle = async_caller.send();
  handle.arg(async_sum);
  handle.check();
  BOOST_REQUIRE(handle.ready());
  BOOST_TEST(handle.collect() == RTT::SendSuccess);
  BOOST_TEST(async_sum == 17);

  RTT::OperationInterfacePart *delayed_add =
      proxy->provides()->getOperation("delayedAdd");
  BOOST_REQUIRE(delayed_add != nullptr);
  std::int32_t delayed_sum = 0;
  RTT::internal::OperationCallerC delayed_caller(
      delayed_add, "delayedAdd", RTT::internal::GlobalEngine::Instance());
  delayed_caller.argC(std::int32_t{10}).argC(std::int32_t{11});
  delayed_caller.check();
  RTT::internal::SendHandleC delayed_handle = delayed_caller.send();
  delayed_handle.arg(delayed_sum);
  delayed_handle.check();
  BOOST_REQUIRE(delayed_handle.ready());
  BOOST_TEST(delayed_handle.collectIfDone() == RTT::SendNotReady);
  BOOST_TEST(delayed_handle.collect() == RTT::SendSuccess);
  BOOST_TEST(delayed_sum == 21);

  target.command.disconnect();
  BOOST_TEST(command_source.write(std::int32_t{75}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil([&] {
    return proxy->lastError().find("NotConnected") != std::string::npos;
  }));
  BOOST_REQUIRE_MESSAGE(proxy->synchronize(&error), error);
  BOOST_TEST(proxy->lastError().find("NotConnected") != std::string::npos);

  BOOST_TEST(cached_add_call->evaluate());
  target.offset = 3;
  cached_offset->set(99);
  BOOST_TEST(target.offset == 99);
  BOOST_TEST(!waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; },
      std::chrono::milliseconds(100)));

  constexpr std::size_t synchronize_count = 4U;
  std::barrier synchronize_start(
      static_cast<std::ptrdiff_t>(synchronize_count));
  std::array<bool, synchronize_count> synchronized{};
  std::array<std::string, synchronize_count> synchronize_errors;
  std::vector<std::jthread> synchronizers;
  synchronizers.reserve(synchronize_count);
  std::atomic<bool> control_calls_succeeded{true};
  std::jthread control_caller([&] {
    for (std::size_t call = 0U; call < 20U; ++call) {
      if (!proxy->isActive() ||
          proxy->getTaskState() != RTT::TaskContext::PreOperational) {
        control_calls_succeeded.store(false);
        return;
      }
    }
  });
  for (std::size_t index = 0; index < synchronize_count; ++index) {
    synchronizers.emplace_back([&, index] {
      synchronize_start.arrive_and_wait();
      synchronized[index] = proxy->synchronize(&synchronize_errors[index]);
    });
  }
  synchronizers.clear();
  control_caller.join();
  for (std::size_t index = 0; index < synchronize_count; ++index) {
    BOOST_TEST(synchronized[index], synchronize_errors[index]);
  }
  BOOST_TEST(control_calls_succeeded.load());
  BOOST_TEST(proxy->ready());
  BOOST_REQUIRE(proxy->ports()->getPort("Feedback") != nullptr);

  remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  target.command.disconnect();
  BOOST_TEST(command_source.write(std::int32_t{76}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil([&] {
    return proxy->lastError().find("NotConnected") != std::string::npos;
  }));

  BOOST_REQUIRE_MESSAGE(proxy->synchronize(&error), error);
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::connected);
  BOOST_TEST(proxy->ready());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);
  BOOST_TEST(!waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; },
      std::chrono::milliseconds(100)));

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_round_trips_an_endpoint_bound_custom_datatype,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  server_options.additional_namespace_uris = {"urn:test:unrelated"};
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  CustomProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);

  auto *configured = dynamic_cast<RTT::Property<FixtureValue> *>(
      proxy->provides()->getProperty("Configured"));
  BOOST_REQUIRE(configured != nullptr);
  BOOST_TEST(configured->get().count == 3);
  BOOST_TEST(configured->get().scale == 1.5);
  configured->set({8, 4.5});
  BOOST_TEST(target.configured.count == 8);
  BOOST_TEST(target.configured.scale == 4.5);

  RTT::base::AttributeBase *observed =
      proxy->provides()->getAttribute("Observed");
  BOOST_REQUIRE(observed != nullptr);
  auto *observed_source =
      RTT::internal::AssignableDataSource<FixtureValue>::narrow(
          observed->getDataSource().get());
  BOOST_REQUIRE(observed_source != nullptr);
  BOOST_TEST(observed_source->get().count == 4);
  observed_source->set({9, 5.5});
  BOOST_TEST(target.observed.count == 9);
  BOOST_TEST(target.observed.scale == 5.5);

  RTT::OperationInterfacePart *adjust =
      proxy->provides()->getOperation("adjust");
  BOOST_REQUIRE(adjust != nullptr);
  FixtureValue adjusted;
  RTT::internal::OperationCallerC adjust_caller(
      adjust, "adjust", RTT::internal::GlobalEngine::Instance());
  adjust_caller.argC(FixtureValue{10, 3.0}).ret(adjusted);
  adjust_caller.check();
  BOOST_REQUIRE(adjust_caller.call());
  BOOST_TEST(adjusted.count == 11);
  BOOST_TEST(adjusted.scale == 6.0);

  auto *remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  RTT::InputPort<FixtureValue> feedback_sink("FeedbackSink");
  BOOST_REQUIRE(remote_feedback->createConnection(
      feedback_sink, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(target.feedback.write(FixtureValue{12, 6.5}) == RTT::WriteSuccess);
  FixtureValue feedback_value;
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value.count == 12);
  BOOST_TEST(feedback_value.scale == 6.5);

  auto *remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  RTT::OutputPort<FixtureValue> command_source("CommandSource");
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(FixtureValue{13, 7.5}) == RTT::WriteSuccess);
  FixtureValue command_value;
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value.count == 13);
  BOOST_TEST(command_value.scale == 7.5);

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_creation_rejects_a_missing_component,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  RTT::opcua::ObjectModel model(server);
  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), "missing/component", proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(!error.empty());

  server.stop();
}

BOOST_AUTO_TEST_CASE(proxy_creation_rejects_an_invalid_port_poll_interval) {
  RTT::opcua::TaskContextProxyOptions options;
  options.port_poll_interval = std::chrono::milliseconds::zero();
  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      "opc.tcp://127.0.0.1:4840", "remote/component", options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error ==
             "OPC UA port poll interval is outside the supported range");
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_an_incompatible_port_method_signature,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  const std::uint16_t namespace_index = server.namespaceIndex().value();
  const std::vector<std::string_view> port_segments{
      "components", target.getName(), "ports", "Feedback"};
  const std::vector<std::string_view> method_segments{
      "components", target.getName(), "ports", "Feedback", "read"};
  const ::opcua::NodeId port_id(namespace_index,
                                RTT::opcua::makeNodePath(port_segments));
  const ::opcua::NodeId method_id(namespace_index,
                                  RTT::opcua::makeNodePath(method_segments));
  bool replaced = false;
  BOOST_REQUIRE(server.invoke(
      [&](::opcua::Server &native) {
        const ::opcua::StatusCode deleted =
            ::opcua::services::deleteNode(native, method_id, true);
        if (!deleted.isGood()) {
          return;
        }
        ::opcua::MethodAttributes attributes;
        attributes.setDisplayName(::opcua::LocalizedText("en-US", "read"));
        attributes.setExecutable(true);
        attributes.setUserExecutable(true);
        ::opcua::services::MethodCallback callback =
            std::function<::opcua::StatusCode(
                ::opcua::Session &, ::opcua::Span<const ::opcua::Variant>,
                ::opcua::Span<::opcua::Variant>, const ::opcua::NodeId &,
                const ::opcua::NodeId &)>(
                [](::opcua::Session &, ::opcua::Span<const ::opcua::Variant>,
                   ::opcua::Span<::opcua::Variant>, const ::opcua::NodeId &,
                   const ::opcua::NodeId &) {
                  return ::opcua::StatusCode(UA_STATUSCODE_GOOD);
                });
        replaced =
            ::opcua::services::addMethod(
                native, port_id, method_id, "read", std::move(callback), {}, {},
                attributes, ::opcua::ReferenceTypeId::HasComponent)
                .hasValue();
      },
      std::chrono::seconds(1), &error));
  BOOST_REQUIRE_MESSAGE(replaced, error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("incompatible method signature") != std::string::npos);

  server.stop();
}
