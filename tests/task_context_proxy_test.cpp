#define BOOST_TEST_MODULE rtt_opcua_task_context_proxy
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <rtt/FactoryExceptions.hpp>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/TaskContext.hpp>
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
  }

  std::int32_t add(std::int32_t left, std::int32_t right) {
    return left + right;
  }

  void increment(std::int32_t &value) { ++value; }

  std::int32_t delayedAdd(std::int32_t left, std::int32_t right) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return left + right;
  }
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
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::connected);

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
