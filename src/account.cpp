/*
 * 账号登录、注册、设备列表与同账号授权。
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include <algorithm>
#include <cctype>

#include "controller_identity.h"
#include "log.h"
#include "signal_negotiation.h"

namespace {
constexpr size_t kMaxAccountDevices = 100;

bool ValidUsername(const std::string& name) {
  return name.size() >= 3 && name.size() <= 32 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@';
         });
}

bool ValidAccountPassword(const std::string& password) {
  return password.size() >= 8 && password.size() <= 128 &&
         password.find('\0') == std::string::npos;
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// 设备名称只用于展示：去除控制字符，并在 UTF-8 字符边界截断到 64 字节。
std::string DeviceName(const json& request) {
  const auto field = request.find("device_name");
  if (field == request.end() || !field->is_string()) return "";
  std::string name;
  for (unsigned char c : field->get_ref<const std::string&>())
    if (c >= 0x20 && c != 0x7f) name.push_back(static_cast<char>(c));
  if (name.size() > 64) {
    size_t end = 64;
    while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
    name.resize(end);
  }
  return name;
}

bool PersistentDeviceId(const std::string& id) {
  return !id.empty() && id.size() <= 128 && id.rfind("C-", 0) != 0 &&
         id.find('@') == std::string::npos && !IsTemporaryControllerId(id) &&
         !IsTemporaryControllerRegistration(id);
}

json AccountJson(const AccountRecord& account) {
  return {{"username", account.username}};
}
}  // namespace

bool SignalNegotiation::IsAccountMessage(const std::string& type) {
  return type == "account_register" || type == "account_login" ||
         type == "account_logout" || type == "account_status" ||
         type == "account_devices" || type == "account_remove_device";
}

bool SignalNegotiation::SameAccount(websocketpp::connection_hdl hdl,
                                    const std::string& target_device) const {
  // 只信任设备密钥验证得到的设备 ID；声明的 user_id 不能作为账号身份。
  const auto controller = transmission_manager_->GetVerifiedDeviceId(hdl);
  if (!PersistentDeviceId(controller) || !PersistentDeviceId(target_device) ||
      controller == target_device)
    return false;
  const auto mine = device_db_manager_->DeviceAccountId(controller);
  if (!mine) return false;
  const auto theirs = device_db_manager_->DeviceAccountId(target_device);
  return theirs && *theirs == *mine;
}

void SignalNegotiation::NotifyAccountChanged(int64_t account_id,
                                             const std::string& except) {
  for (const auto& device : device_db_manager_->ListAccountDevices(account_id)) {
    if (device.device_id == except) continue;
    const auto hdl = transmission_manager_->GetWsHandle(device.device_id);
    if (!hdl.expired()) send_msg_(hdl, {{"type", "account_changed"}});
  }
}

void SignalNegotiation::BindAccount(websocketpp::connection_hdl hdl,
                                    const std::string& device_id,
                                    const AccountRecord& account,
                                    const std::string& device_name, json reply) {
  const auto previous = device_db_manager_->DeviceAccountId(device_id);
  if (!previous || *previous != account.id) {
    if (device_db_manager_->ListAccountDevices(account.id).size() >=
        kMaxAccountDevices) {
      reply["error_code"] = "ACCOUNT_DEVICE_LIMIT";
      reply["reason"] = "Too many devices on this account";
      send_msg_(hdl, reply);
      return;
    }
  }
  if (!device_db_manager_->BindDeviceToAccount(device_id, account.id, device_name)) {
    reply["error_code"] = "SERVER_ERROR";
    reply["reason"] = "Unable to bind device";
    send_msg_(hdl, reply);
    return;
  }
  LOG_INFO("Device [{}] signed in to account [{}]", device_id, account.id);
  reply["status"] = "success";
  reply["account"] = AccountJson(account);
  send_msg_(hdl, reply);
  if (previous && *previous != account.id) NotifyAccountChanged(*previous);
  NotifyAccountChanged(account.id, device_id);
}

void SignalNegotiation::account_request(websocketpp::connection_hdl hdl,
                                        const json& j,
                                        const std::string& source_address,
                                        const CredentialDispatch& dispatch) {
  const auto type = j.at("type").get<std::string>();
  json reply = {{"type", "account"}, {"action", type.substr(8)}, {"status", "fail"}};
  const auto request_id = j.find("request_id");
  if (request_id != j.end() && request_id->is_string() &&
      request_id->get_ref<const std::string&>().size() <= 64)
    reply["request_id"] = *request_id;
  auto fail = [this, hdl, &reply](const char* code, const char* reason) {
    reply["error_code"] = code;
    reply["reason"] = reason;
    send_msg_(hdl, reply);
  };

  // 账号操作只允许设备主连接，控制端连接（C-）或旧版密码身份不能登录账号。
  const auto device = transmission_manager_->GetVerifiedDeviceId(hdl);
  if (hdl.expired() || !PersistentDeviceId(device) ||
      transmission_manager_->GetUserId(hdl) != device)
    return fail("NOT_AUTHENTICATED", "Device not authenticated");

  if (type == "account_status") {
    const auto account_id = device_db_manager_->DeviceAccountId(device);
    const auto account = account_id ? device_db_manager_->ReadAccountById(*account_id)
                                     : std::nullopt;
    reply["status"] = "success";
    reply["account"] = account ? AccountJson(*account) : json(nullptr);
    send_msg_(hdl, reply);
    return;
  }

  if (type == "account_logout") {
    const auto account_id = device_db_manager_->DeviceAccountId(device);
    if (!device_db_manager_->UnbindDeviceFromAccount(device))
      return fail("SERVER_ERROR", "Unable to sign out");
    if (account_id) {
      LOG_INFO("Device [{}] signed out of account [{}]", device, *account_id);
      NotifyAccountChanged(*account_id, device);
    }
    reply["status"] = "success";
    reply["account"] = nullptr;
    send_msg_(hdl, reply);
    return;
  }

  if (type == "account_devices" || type == "account_remove_device") {
    const auto account_id = device_db_manager_->DeviceAccountId(device);
    if (!account_id) return fail("NOT_SIGNED_IN", "Not signed in");
    if (type == "account_remove_device") {
      const auto target = j.find("device_id");
      if (target == j.end() || !target->is_string() ||
          !PersistentDeviceId(target->get<std::string>()))
        return fail("INVALID_DEVICE", "Invalid device");
      const auto target_id = target->get<std::string>();
      const auto target_account = device_db_manager_->DeviceAccountId(target_id);
      if (!target_account || *target_account != *account_id)
        return fail("INVALID_DEVICE", "Device is not on this account");
      if (!device_db_manager_->UnbindDeviceFromAccount(target_id, *account_id))
        return fail("SERVER_ERROR", "Unable to remove device");
      LOG_INFO("Device [{}] removed [{}] from account [{}]", device, target_id,
               *account_id);
      // 被移除的设备也需要刷新登录状态。
      const auto removed = transmission_manager_->GetWsHandle(target_id);
      if (!removed.expired()) send_msg_(removed, {{"type", "account_changed"}});
      NotifyAccountChanged(*account_id, device);
    }
    json devices = json::array();
    for (const auto& item : device_db_manager_->ListAccountDevices(*account_id)) {
      // 列表中的在线状态以当前信令连接为准，数据库状态可能尚未刷新。
      const bool online = !transmission_manager_->GetWsHandle(item.device_id).expired();
      devices.push_back({{"id", item.device_id},
                         {"name", item.name},
                         {"platform", item.platform},
                         {"online", online},
                         {"self", item.device_id == device}});
    }
    reply["status"] = "success";
    reply["devices"] = std::move(devices);
    send_msg_(hdl, reply);
    return;
  }

  // account_register / account_login
  const bool registration = type == "account_register";
  const auto username_field = j.find("username");
  const auto password_field = j.find("password");
  if (username_field == j.end() || !username_field->is_string() ||
      !ValidUsername(username_field->get<std::string>()))
    return fail("INVALID_USERNAME", "Invalid username");
  if (password_field == j.end() || !password_field->is_string() ||
      !ValidAccountPassword(password_field->get<std::string>()))
    return fail("INVALID_PASSWORD", "Invalid password");
  if (registration && !account_registration_enabled_)
    return fail("REGISTRATION_DISABLED", "Registration is disabled");
  const auto username = username_field->get<std::string>();
  const auto password = password_field->get<std::string>();
  const auto device_name = DeviceName(j);

  if (registration) {
    RunCredentialWork(hdl, source_address, reply,
        [this, hdl, device, username, password, device_name,
         reply]() -> std::function<void()> {
          auto record = DevicePassword::Hash(password);
          return [this, hdl, device, username, device_name, reply = json(reply),
                  record = std::move(record)]() mutable {
            if (hdl.expired() || transmission_manager_->GetUserId(hdl) != device) return;
            if (!record) {
              reply["error_code"] = "SERVER_ERROR";
              reply["reason"] = "Unable to create account";
              send_msg_(hdl, reply);
              return;
            }
            const auto [result, id] = device_db_manager_->CreateAccount(username, *record);
            if (result != AccountCreateResult::Created) {
              const bool exists = result == AccountCreateResult::Exists;
              reply["error_code"] = exists ? "USERNAME_TAKEN" : "SERVER_ERROR";
              reply["reason"] = exists ? "Username already exists" : "Unable to create account";
              send_msg_(hdl, reply);
              return;
            }
            LOG_INFO("Account [{}] registered from device [{}]", id, device);
            BindAccount(hdl, device, {id, username, *record}, device_name, reply);
          };
        },
        dispatch);
    return;
  }

  // 失败计数按“设备 + 用户名”与“设备”两个维度累计，并在后台风控页展示。
  const auto source_key = json::array({"controller", device}).dump();
  const auto pair_key = json::array({"controller", device, "account:" + Lower(username)}).dump();
  if (const int retry = device_auth_limiter_->CooldownRemaining(source_key, pair_key);
      retry > 0) {
    reply["retry_after"] = retry;
    return fail("AUTHENTICATION_THROTTLED", "Too many authentication attempts");
  }
  const auto account = device_db_manager_->ReadAccount(username);
  RunCredentialWork(hdl, source_address, reply,
      [this, hdl, device, password, device_name, account, source_key, pair_key,
       reply]() -> std::function<void()> {
        // 账号不存在时仍执行一次哈希，避免通过耗时区分用户名是否存在。
        const auto stored = account ? std::optional<DevicePasswordRecord>(account->password)
                                    : std::nullopt;
        const auto result = device_auth_limiter_->Verify(source_key, pair_key, [&] {
          return DevicePassword::Verify(stored, password).has_value() && account.has_value();
        }, DeviceAuthLimiter::Operation::AccountLogin);
        return [this, hdl, device, device_name, account, result,
                reply = json(reply)]() mutable {
          if (hdl.expired() || transmission_manager_->GetUserId(hdl) != device) return;
          if (!result.authenticated || !account) {
            if (result.retry_after > 0) {
              reply["error_code"] = "AUTHENTICATION_THROTTLED";
              reply["reason"] = "Too many authentication attempts";
              reply["retry_after"] = result.retry_after;
            } else {
              reply["error_code"] = "AUTHENTICATION_FAILED";
              reply["reason"] = "Incorrect username or password";
            }
            send_msg_(hdl, reply);
            return;
          }
          BindAccount(hdl, device, *account, device_name, reply);
        };
      },
      dispatch);
}
