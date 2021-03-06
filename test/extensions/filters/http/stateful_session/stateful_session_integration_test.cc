#include <chrono>
#include <cstdint>

#include "envoy/config/endpoint/v3/endpoint_components.pb.h"

#include "source/common/common/base64.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/extensions/filters/http/stateful_session/stateful_session.h"

#include "test/integration/http_integration.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace StatefulSession {
namespace {

class StatefulSessionIntegrationTest : public Envoy::HttpIntegrationTest, public testing::Test {
public:
  StatefulSessionIntegrationTest()
      : HttpIntegrationTest(
            Http::CodecType::HTTP1,
            [](int i) { return Network::Utility::parseInternetAddress("127.0.0.1", 50000 + i); },
            Network::Address::IpVersion::v4) {
    // Create 4 different upstream server for stateful session test.
    setUpstreamCount(4);

    skipPortUsageValidation();

    // Update endpoints of default cluster `cluster_0` to 4 different fake upstreams.
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* cluster_0 = bootstrap.mutable_static_resources()->mutable_clusters()->Mutable(0);
      ASSERT(cluster_0->name() == "cluster_0");
      auto* endpoint = cluster_0->mutable_load_assignment()->mutable_endpoints()->Mutable(0);

      const std::string EndpointsYaml = R"EOF(
        lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 50000
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 50001
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 50002
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 50003
      )EOF";

      envoy::config::endpoint::v3::LocalityLbEndpoints new_lb_endpints;
      TestUtility::loadFromYaml(EndpointsYaml, new_lb_endpints);
      *endpoint = new_lb_endpints;
    });
  }

  // Initialize route filter and per route config.
  void initializeFilterAndRoute(const std::string& filter_yaml,
                                const std::string& per_route_config_yaml) {
    config_helper_.prependFilter(filter_yaml);

    // Create virtual host with domain `stateful.session.com` and default route to `cluster_0`
    auto virtual_host = config_helper_.createVirtualHost("stateful.session.com");

    // Update per route config of default route.
    if (!per_route_config_yaml.empty()) {
      auto* route = virtual_host.mutable_routes(0);
      ProtobufWkt::Any per_route_config;
      TestUtility::loadFromYaml(per_route_config_yaml, per_route_config);

      route->mutable_typed_per_filter_config()->insert(
          {"envoy.filters.http.stateful_session", per_route_config});
    }
    config_helper_.addVirtualHost(virtual_host);

    initialize();
  }
};

static const std::string STATEFUL_SESSION_FILTER =
    R"EOF(
name: envoy.filters.http.stateful_session
typed_config:
  "@type": type.googleapis.com/envoy.extensions.filters.http.stateful_session.v3.StatefulSession
  session_state:
    name: envoy.http.stateful_session.cookie
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.http.stateful_session.cookie.v3.CookieBasedSessionState
      cookie:
        name: global-session-cookie
        path: /path
        ttl: 120s
)EOF";

static const std::string DISABLE_STATEFUL_SESSION =
    R"EOF(
"@type": type.googleapis.com/envoy.extensions.filters.http.stateful_session.v3.StatefulSessionPerRoute
disabled: true
)EOF";

static const std::string OVERRIDE_STATEFUL_SESSION =
    R"EOF(
"@type": type.googleapis.com/envoy.extensions.filters.http.stateful_session.v3.StatefulSessionPerRoute
stateful_session:
  session_state:
    name: envoy.http.stateful_session.cookie
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.http.stateful_session.cookie.v3.CookieBasedSessionState
      cookie:
        name: route-session-cookie
        path: /path
        ttl: 120s
)EOF";

TEST_F(StatefulSessionIntegrationTest, NormalStatefulSession) {
  initializeFilterAndRoute(STATEFUL_SESSION_FILTER, "");

  codec_client_ = makeHttpConnection(lookupPort("http"));

  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/test"},
                                                 {":scheme", "http"},
                                                 {":authority", "stateful.session.com"}};

  auto response = codec_client_->makeRequestWithBody(request_headers, 0);

  auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
  ASSERT(upstream_index.has_value());
  const std::string address_string = fmt::format("127.0.0.1:{}", upstream_index.value() + 50000);
  const std::string encoded_address = Envoy::Base64::encode(address_string.data(), 15);

  upstream_request_->encodeHeaders(default_response_headers_, true);

  ASSERT_TRUE(response->waitForEndStream());

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_TRUE(response->complete());

  // The selected upstream server address would be selected to the response headers.
  EXPECT_EQ(
      Envoy::Http::Utility::makeSetCookieValue("global-session-cookie", encoded_address, "/path",
                                               std::chrono::seconds(120), true),
      response->headers().get(Http::LowerCaseString("set-cookie"))[0]->value().getStringView());

  cleanupUpstreamAndDownstream();
}

TEST_F(StatefulSessionIntegrationTest, DownstreamRequestWithStatefulSessionCookie) {
  initializeFilterAndRoute(STATEFUL_SESSION_FILTER, "");

  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie", fmt::format("global-session-cookie=\"{}\"",
                               Envoy::Base64::encode("127.0.0.1:50001", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    // `127.0.0.1:50001` should be selected and it's upstream index is 1.
    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    EXPECT_EQ(upstream_index.value(), 1);

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    // No response header to be added.
    EXPECT_TRUE(response->headers().get(Http::LowerCaseString("set-cookie")).empty());

    cleanupUpstreamAndDownstream();
  }

  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie", fmt::format("global-session-cookie=\"{}\"",
                               Envoy::Base64::encode("127.0.0.1:50002", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    // `127.0.0.1:50002` should be selected and it's upstream index is 2.
    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    EXPECT_EQ(upstream_index.value(), 2);

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    // No response header to be added.
    EXPECT_TRUE(response->headers().get(Http::LowerCaseString("set-cookie")).empty());

    cleanupUpstreamAndDownstream();
  }

  // Test the case that stateful session cookie with unknown server address.
  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie", fmt::format("global-session-cookie=\"{}\"",
                               Envoy::Base64::encode("127.0.0.1:50005", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    ASSERT(upstream_index.has_value());
    const std::string address_string = fmt::format("127.0.0.1:{}", upstream_index.value() + 50000);
    const std::string encoded_address = Envoy::Base64::encode(address_string.data(), 15);

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    // The selected upstream server address would be selected to the response headers.
    EXPECT_EQ(
        Envoy::Http::Utility::makeSetCookieValue("global-session-cookie", encoded_address, "/path",
                                                 std::chrono::seconds(120), true),
        response->headers().get(Http::LowerCaseString("set-cookie"))[0]->value().getStringView());

    cleanupUpstreamAndDownstream();
  }
}

TEST_F(StatefulSessionIntegrationTest, StatefulSessionDisabledByRoute) {
  initializeFilterAndRoute(STATEFUL_SESSION_FILTER, DISABLE_STATEFUL_SESSION);

  uint64_t first_index = 0;
  uint64_t second_index = 0;

  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie", fmt::format("global-session-cookie=\"{}\"",
                               Envoy::Base64::encode("127.0.0.1:50001", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    ASSERT(upstream_index.has_value());
    first_index = upstream_index.value();

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    // No response header to be added.
    EXPECT_TRUE(response->headers().get(Http::LowerCaseString("set-cookie")).empty());

    cleanupUpstreamAndDownstream();
  }

  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie", fmt::format("global-session-cookie=\"{}\"",
                               Envoy::Base64::encode("127.0.0.1:50001", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    ASSERT(upstream_index.has_value());
    second_index = upstream_index.value();

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    // No response header to be added.
    EXPECT_TRUE(response->headers().get(Http::LowerCaseString("set-cookie")).empty());

    cleanupUpstreamAndDownstream();
  }

  // Choose different upstream servers by default.
  EXPECT_NE(first_index, second_index);
}

TEST_F(StatefulSessionIntegrationTest, StatefulSessionOverriddenByRoute) {
  initializeFilterAndRoute(STATEFUL_SESSION_FILTER, OVERRIDE_STATEFUL_SESSION);

  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie", fmt::format("global-session-cookie=\"{}\"",
                               Envoy::Base64::encode("127.0.0.1:50001", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    ASSERT(upstream_index.has_value());
    const std::string address_string = fmt::format("127.0.0.1:{}", upstream_index.value() + 50000);
    const std::string encoded_address = Envoy::Base64::encode(address_string.data(), 15);

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    EXPECT_EQ(
        Envoy::Http::Utility::makeSetCookieValue("route-session-cookie", encoded_address, "/path",
                                                 std::chrono::seconds(120), true),
        response->headers().get(Http::LowerCaseString("set-cookie"))[0]->value().getStringView());

    cleanupUpstreamAndDownstream();
  }

  {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/test"},
        {":scheme", "http"},
        {":authority", "stateful.session.com"},
        {"cookie",
         fmt::format("route-session-cookie=\"{}\"", Envoy::Base64::encode("127.0.0.1:50002", 15))}};

    auto response = codec_client_->makeRequestWithBody(request_headers, 0);

    // Stateful session is overridden and `127.0.0.1:50002` should be selected.
    auto upstream_index = waitForNextUpstreamRequest({0, 1, 2, 3});
    EXPECT_EQ(upstream_index.value(), 2);

    upstream_request_->encodeHeaders(default_response_headers_, true);

    ASSERT_TRUE(response->waitForEndStream());

    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_TRUE(response->complete());

    // No response header to be added.
    EXPECT_TRUE(response->headers().get(Http::LowerCaseString("set-cookie")).empty());

    cleanupUpstreamAndDownstream();
  }
}

} // namespace
} // namespace StatefulSession
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
