#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "device_db_manager.h"
#include "signal_negotiation.h"
#include "transmission_manager.h"

// 账号注册、登录、设备列表、移除、退出以及同账号免密连接。
int main() {
  int failures = 0;
  auto expect = [&failures](bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << std::endl;
      ++failures;
    }
  };

  const auto db_path =
      std::filesystem::temp_directory_path() /
      ("crossdesk_account_test_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".db");
  {
    DeviceDBManager db(db_path.string());
    const auto a = db.AddDevice("", "").device_id;
    const auto b = db.AddDevice("", "").device_id;
    const auto d = db.AddDevice("", "").device_id;
    expect(!a.empty() && !b.empty() && !d.empty(), "test devices are registered");

    auto transmission = std::make_shared<TransmissionManager>();
    SignalNegotiation negotiation(transmission, &db);

    std::vector<std::pair<void*, json>> sent;
    negotiation.SetSendMsgCallback([&](websocketpp::connection_hdl hdl, json message) {
      sent.emplace_back(hdl.lock().get(), std::move(message));
    });
    auto connect = [&](const std::string& user, const std::string& verified) {
      auto connection = std::make_shared<int>(0);
      transmission->BindUserToWsHandle(user, websocketpp::connection_hdl(connection),
                                       verified);
      return connection;
    };
    auto a_main = connect(a, a);
    auto a_controller = connect("C-" + a, a);
    auto b_main = connect(b, b);
    auto d_controller = connect("C-" + d, d);
    auto legacy = connect(d, "");  // 旧版密码登录，没有设备密钥验证
    transmission->BindHostToTransmission(b, b);

    auto request = [&](const std::shared_ptr<int>& connection, json message) {
      sent.clear();
      if (message.at("type").get<std::string>().rfind("account_", 0) == 0)
        negotiation.account_request(websocketpp::connection_hdl(connection), message);
      else
        negotiation.join_transmission(websocketpp::connection_hdl(connection), message);
    };
    auto reply_to = [&](const std::shared_ptr<int>& connection) -> json {
      for (const auto& [target, message] : sent)
        if (target == connection.get()) return message;
      return {};
    };
    auto code = [&](const std::shared_ptr<int>& connection) {
      const auto reply = reply_to(connection);
      return reply.is_object() ? reply.value("error_code", std::string()) : std::string("<none>");
    };
    auto succeeded = [&](const std::shared_ptr<int>& connection) {
      const auto reply = reply_to(connection);
      return reply.is_object() && reply.value("status", "") == "success";
    };

    request(a_main, {{"type", "account_status"}, {"request_id", "s1"}});
    expect(succeeded(a_main) && reply_to(a_main)["account"].is_null() &&
               reply_to(a_main).value("request_id", "") == "s1",
           "status reports signed out with request id");

    request(a_controller, {{"type", "account_register"}, {"username", "alice"},
                           {"password", "password123"}});
    expect(code(a_controller) == "NOT_AUTHENTICATED", "controller connection cannot register");
    request(legacy, {{"type", "account_status"}});
    expect(code(legacy) == "NOT_AUTHENTICATED", "unverified legacy connection is rejected");

    request(a_main, {{"type", "account_register"}, {"username", "ab"}, {"password", "password123"}});
    expect(code(a_main) == "INVALID_USERNAME", "short username is rejected");
    request(a_main, {{"type", "account_register"}, {"username", "alice"}, {"password", "short"}});
    expect(code(a_main) == "INVALID_PASSWORD", "short password is rejected");

    request(a_main, {{"type", "account_register"}, {"username", "alice"},
                     {"password", "password123"}, {"device_name", "Office PC"}});
    expect(succeeded(a_main) && reply_to(a_main)["account"].value("username", "") == "alice",
           "registration signs in the device");

    request(b_main, {{"type", "account_register"}, {"username", "ALICE"},
                     {"password", "password123"}});
    expect(code(b_main) == "USERNAME_TAKEN", "usernames are case-insensitive");

    request(b_main, {{"type", "account_login"}, {"username", "alice"},
                     {"password", "wrong-password"}});
    expect(code(b_main) == "AUTHENTICATION_FAILED", "wrong password is rejected");
    request(b_main, {{"type", "account_login"}, {"username", "nobody"},
                     {"password", "password123"}});
    expect(code(b_main) == "AUTHENTICATION_FAILED", "unknown account looks like a wrong password");

    request(b_main, {{"type", "account_login"}, {"username", "Alice"},
                     {"password", "password123"}, {"device_name", "Laptop"}});
    expect(succeeded(b_main) && reply_to(b_main)["account"].value("username", "") == "alice",
           "login binds the second device");
    expect(reply_to(a_main).value("type", "") == "account_changed",
           "other online devices are notified of a new device");

    request(a_main, {{"type", "account_devices"}});
    const auto devices = reply_to(a_main)["devices"];
    expect(devices.is_array() && devices.size() == 2, "account lists both devices");
    for (const auto& device : devices) {
      if (device["id"] == a)
        expect(device["self"] == true && device["name"] == "Office PC", "current device is marked");
      if (device["id"] == b)
        expect(device["online"] == true && device["name"] == "Laptop", "other device is online");
    }

    // 同账号控制端无需密码即可加入。
    request(a_controller, {{"type", "join_transmission"}, {"user_id", "C-" + a},
                           {"transmission_id", b + "@"}});
    expect(succeeded(b_main) && reply_to(b_main).value("type", "") == "user_join_transmission",
           "same-account controller joins without a password");
    expect(reply_to(a_controller).is_null(), "same-account join sends no failure to controller");

    // 不同账号仍需密码。
    request(d_controller, {{"type", "join_transmission"}, {"user_id", "C-" + d},
                           {"transmission_id", b + "@"}});
    expect(code(d_controller) == "PASSWORD_REQUIRED", "other devices still need the password");

    request(a_main, {{"type", "account_remove_device"}, {"device_id", d}});
    expect(code(a_main) == "INVALID_DEVICE", "cannot remove a device from another account");
    request(a_main, {{"type", "account_remove_device"}, {"device_id", b}});
    expect(succeeded(a_main) && reply_to(a_main)["devices"].size() == 1,
           "removing a device returns the updated list");
    expect(reply_to(b_main).value("type", "") == "account_changed",
           "removed device is notified");
    request(a_controller, {{"type", "join_transmission"}, {"user_id", "C-" + a},
                           {"transmission_id", b + "@"}});
    expect(code(a_controller) == "PASSWORD_REQUIRED", "removed device requires the password again");

    request(a_main, {{"type", "account_logout"}});
    expect(succeeded(a_main) && reply_to(a_main)["account"].is_null(), "logout succeeds");
    request(a_main, {{"type", "account_devices"}});
    expect(code(a_main) == "NOT_SIGNED_IN", "device list requires sign-in");

    // 失败过多后限流。
    bool throttled = false;
    for (int i = 0; i < 8 && !throttled; ++i) {
      request(a_main, {{"type", "account_login"}, {"username", "alice"},
                       {"password", "bad-password-" + std::to_string(i)}});
      throttled = code(a_main) == "AUTHENTICATION_THROTTLED";
    }
    expect(throttled, "repeated failures are throttled");

    negotiation.SetAccountRegistrationEnabled(false);
    request(b_main, {{"type", "account_register"}, {"username", "bob_1"},
                     {"password", "password123"}});
    expect(code(b_main) == "REGISTRATION_DISABLED", "registration can be disabled");

    // 删除设备时一并移除账号绑定。
    request(b_main, {{"type", "account_login"}, {"username", "alice"},
                     {"password", "password123"}});
    expect(succeeded(b_main), "existing accounts can still sign in");
    expect(db.DeviceAccountId(b).has_value(), "device is bound before deletion");
    expect(db.RemoveDevice(b), "device can be removed");
    expect(!db.DeviceAccountId(b).has_value(), "deleting a device removes its account binding");
    expect(db.CountAccounts() == 1, "one account exists");
  }
  std::error_code error;
  std::filesystem::remove(db_path, error);
  std::filesystem::remove(db_path.string() + "-wal", error);
  std::filesystem::remove(db_path.string() + "-shm", error);
  if (failures == 0) std::cout << "account_test passed" << std::endl;
  return failures == 0 ? 0 : 1;
}
