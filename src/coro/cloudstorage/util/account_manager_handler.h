#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/auth_token_manager.h>
#include <coro/cloudstorage/util/proxy_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/stdx/any_invocable.h>
#include <coro/util/type_list.h>

#include <regex>

namespace coro::cloudstorage::util {

template <typename CloudProviderTypeList, typename CloudFactory,
          typename AccountListener,
          typename AuthTokenManagerT = AuthTokenManager>
class AccountManagerHandler;

template <typename... CloudProviders, typename CloudFactory,
          typename AccountListener, typename AuthTokenManagerT>
class AccountManagerHandler<coro::util::TypeList<CloudProviders...>,
                            CloudFactory, AccountListener, AuthTokenManagerT> {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;
  using HandlerType = coro::stdx::any_invocable<Task<Response>(
      Request, coro::stdx::stop_token)>;

  struct Data;

  template <typename CloudProvider>
  struct OnAuthTokenChanged {
    void operator()(typename CloudProvider::Auth::AuthToken auth_token) {
      if (*account_id) {
        d->auth_token_manager.template SaveToken<CloudProvider>(
            std::move(auth_token), **account_id);
      }
    }
    Data* d;
    std::shared_ptr<std::optional<std::string>> account_id;
  };

  struct CloudProviderAccount {
    template <typename T>
    using CloudProviderT =
        decltype(std::declval<CloudFactory>().template Create<T>(
            std::declval<typename T::Auth::AuthToken>(),
            std::declval<OnAuthTokenChanged<T>>()));

    std::string id;
    std::variant<CloudProviderT<CloudProviders>...> provider;
  };

  struct Handler {
    std::string id;
    std::regex regex;
    HandlerType handler;
  };

  struct Data {
    Data(const CloudFactory& factory, AccountListener account_listener,
         AuthTokenManagerT auth_token_manager)
        : factory(factory),
          account_listener(std::move(account_listener)),
          auth_token_manager(std::move(auth_token_manager)) {}

    ~Data() {
      for (auto& account : accounts) {
        account_listener.OnDestroy(&account);
      }
    }

    template <typename CloudProvider>
    void RemoveCloudProvider(std::string id) {
      for (auto it = std::begin(accounts); it != std::end(accounts);) {
        if (it->id == id) {
          account_listener.OnDestroy(&*it);
          it = accounts.erase(it);
        } else {
          it++;
        }
      }
      for (auto it = std::begin(handlers); it != std::end(handlers);) {
        if (it->id == id) {
          it = handlers.erase(it);
        } else {
          it++;
        }
      }
      auth_token_manager.template RemoveToken<CloudProvider>(id);
    }

    template <typename CloudProvider>
    struct AuthHandler {
      using AuthToken = typename CloudProvider::Auth::AuthToken;

      Task<Response> operator()(Request request,
                                stdx::stop_token stop_token) const {
        auto result =
            co_await d->factory.template CreateAuthHandler<CloudProvider>()(
                std::move(request), stop_token);
        AuthToken auth_token;
        if constexpr (std::is_same_v<decltype(result), AuthToken>) {
          auth_token = std::move(result);
        } else {
          if (std::holds_alternative<Response>(result)) {
            co_return std::move(std::get<Response>(result));
          } else {
            auth_token = std::move(std::get<AuthToken>(result));
          }
        }
        auto account_id =
            std::make_shared<std::optional<std::string>>(std::nullopt);
        auto provider = d->factory.template Create<CloudProvider>(
            auth_token, OnAuthTokenChanged<CloudProvider>{d, account_id});
        auto general_data =
            co_await provider.GetGeneralData(std::move(stop_token));
        *account_id = std::string(GetCloudProviderId<CloudProvider>()) + "/" +
                      std::move(general_data.username);
        d->auth_token_manager.template SaveToken<CloudProvider>(
            std::move(auth_token), **account_id);
        d->OnCloudProviderCreated<CloudProvider>(std::move(provider),
                                                 **account_id);
        co_return Response{.status = 302,
                           .headers = {{"Location", "/" + **account_id}}};
      }

      Data* d;
    };

    template <typename CloudProvider>
    void AddAuthHandler() {
      handlers.emplace_back(Handler{
          .regex = std::regex("/auth(/" +
                              std::string(GetCloudProviderId<CloudProvider>()) +
                              ".*$)"),
          .handler = AuthHandler<CloudProvider>{this}});
    }

    template <typename CloudProvider, typename CloudProviderT>
    void OnCloudProviderCreated(CloudProviderT provider_impl,
                                std::string account_id) {
      for (const auto& entry : accounts) {
        if (entry.id == account_id) {
          return;
        }
      }
      handlers.emplace_back(Handler{
          .id = account_id,
          .regex = std::regex("/remove(/" + account_id + ".*$)"),
          .handler = [d = this, id = account_id](
                         Request request,
                         coro::stdx::stop_token) -> Task<Response> {
            d->template RemoveCloudProvider<CloudProvider>(id);
            co_return Response{.status = 302, .headers = {{"Location", "/"}}};
          }});

      accounts.emplace_back(CloudProviderAccount{
          .id = account_id, .provider = std::move(provider_impl)});

      auto* provider =
          &std::get<typename CloudProviderAccount::template CloudProviderT<
              CloudProvider>>(accounts.back().provider);
      handlers.emplace_back(Handler{
          .id = account_id,
          .regex = std::regex("/" + account_id + "(.*$)"),
          .handler = HandlerType(ProxyHandler(provider, "/" + account_id))});
      account_listener.OnCreate(&accounts.back());
    }

    const CloudFactory& factory;
    std::vector<Handler> handlers;
    AccountListener account_listener;
    AuthTokenManagerT auth_token_manager;
    std::list<CloudProviderAccount> accounts;
  };

  AccountManagerHandler(const CloudFactory& factory,
                        AccountListener account_listener,
                        AuthTokenManagerT auth_token_manager)
      : d_(std::make_unique<Data>(factory, std::move(account_listener),
                                  std::move(auth_token_manager))) {
    (d_->template AddAuthHandler<CloudProviders>(), ...);
    for (const auto& any_token : d_->auth_token_manager.template LoadTokenData<
                                 coro::util::TypeList<CloudProviders...>>()) {
      std::visit(
          [d = d_.get()](auto token) {
            using CloudProvider = typename decltype(token)::CloudProvider;
            auto id = std::move(token.id);
            d->template OnCloudProviderCreated<CloudProvider>(
                d->factory.template Create<CloudProvider>(
                    std::move(token),
                    OnAuthTokenChanged<CloudProvider>{
                        d, std::make_shared<std::optional<std::string>>(id)}),
                id);
          },
          any_token);
    }
  }

  bool CanHandleUrl(std::string_view url) const {
    if (url.empty() || url == "/") {
      return true;
    }
    for (const auto& handler : d_->handlers) {
      if (std::regex_match(std::string(url), handler.regex)) {
        return true;
      }
    }
    return false;
  }

  Task<Response> operator()(Request request,
                            coro::stdx::stop_token stop_token) {
    if (request.method == coro::http::Method::kOptions) {
      co_return Response{
          .status = 204,
          .headers = {{"Allow", "OPTIONS, GET, HEAD, POST, PROPFIND"},
                      {"DAV", "1"}}};
    }
    for (auto& handler : d_->handlers) {
      if (std::regex_match(request.url, handler.regex)) {
        co_return co_await handler.handler(std::move(request), stop_token);
      }
    }
    if (request.url.empty() || request.url == "/") {
      if (request.method == coro::http::Method::kPropfind) {
        std::vector<std::string> responses = {GetElement(
            ElementData{.path = "/", .name = "root", .is_directory = true})};
        if (coro::http::GetHeader(request.headers, "Depth") == "1") {
          for (const auto& account : d_->accounts) {
            responses.push_back(
                GetElement(ElementData{.path = "/" + account.id + "/",
                                       .name = account.id,
                                       .is_directory = true}));
          }
        }
        co_return coro::http::Response<>{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = CreateBody(GetMultiStatusResponse(responses))};
      } else {
        co_return coro::http::Response<>{.status = 200, .body = GetHomePage()};
      }
    } else {
      co_return coro::http::Response<>{.status = 302,
                                       .headers = {{"Location", "/"}}};
    }
  }

 private:
  template <typename CloudProvider>
  static void AppendAuthUrl(const CloudFactory& factory,
                            std::stringstream& stream) {
    std::string id(GetCloudProviderId<CloudProvider>());
    std::string url =
        factory.template GetAuthorizationUrl<CloudProvider>().value_or(
            "/auth/" + id);
    stream << "<tr><td><a href='" << url << "'>"
           << GetCloudProviderId<CloudProvider>() << "</a></td>";
    stream << "</tr>";
  }

  Generator<std::string> GetHomePage() {
    std::stringstream result;
    result << "<html><body><table>";
    (AppendAuthUrl<CloudProviders>(d_->factory, result), ...);
    result << "</table><table>";

    for (const auto& account : d_->accounts) {
      result << "<tr><td><a href='/" << account.id << "/'>" << account.id
             << "</a></td>";
      result << "<td><form action='/remove/" << account.id
             << "' method='POST' style='margin: auto;'><input "
                "type='submit' value='remove'/></form></td>";
      result << "</tr>";
    }

    result << "</table></body></html>";
    co_yield result.str();
  }

  static Generator<std::string> CreateBody(std::string body) {
    co_yield std::move(body);
  }

  std::unique_ptr<Data> d_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
