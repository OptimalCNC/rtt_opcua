#define BOOST_TEST_MODULE rtt_opcua_task_context_proxy
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <rtt/FactoryExceptions.hpp>
#include <rtt/OperationInterfacePart.hpp>
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

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

struct CanonicalTypesFixture {
  CanonicalTypesFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());
  }
};

class ProxyTarget final : public RTT::TaskContext {
public:
  ProxyTarget() : RTT::TaskContext("remote/calculator") {
    provides()->doc("Remote calculator.");
    addProperty("Gain", gain).doc("Controller gain.");
    addAttribute("Status", status);
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
    math->addProperty("Offset", offset).doc("Scale offset.");
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
};

} // namespace

BOOST_FIXTURE_TEST_CASE(proxy_calls_remote_operations_synchronously_and_async,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  RTT::opcua::ObjectModelOptions model_options;
  model_options.reconcile_interval = std::chrono::milliseconds(10);
  RTT::opcua::ObjectModel model(server, model_options);
  ProxyTarget target;
  auto registration = model.registerComponent(target, &error);
  BOOST_REQUIRE_MESSAGE(registration.has_value(), error);

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
  BOOST_TEST(!status->getDataSource()->isAssignable());
  auto *status_source = RTT::internal::DataSource<std::string>::narrow(
      status->getDataSource().get());
  BOOST_REQUIRE(status_source != nullptr);
  BOOST_TEST(status_source->get() == "idle");
  target.status = "running";
  BOOST_TEST(status_source->get() == "running");

  RTT::Service::shared_ptr math = proxy->provides()->getService("math");
  BOOST_REQUIRE(math);
  BOOST_TEST(math->doc() == "Math utilities.");
  auto *offset =
      dynamic_cast<RTT::Property<std::int32_t> *>(math->getProperty("Offset"));
  BOOST_REQUIRE(offset != nullptr);
  BOOST_TEST(offset->getDescription() == "Scale offset.");
  BOOST_TEST(offset->get() == 2);
  offset->set(3);
  BOOST_TEST(target.offset == 3);

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

  proxy.reset();
  registration->reset();
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
