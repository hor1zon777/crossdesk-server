/*
 * 账号与设备绑定的持久化。
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include <chrono>
#include <memory>
#include <stdexcept>

#include "device_db_manager.h"
#include "log.h"

namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* raw = nullptr;
  if (!db || sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
    raw = nullptr;
  return Statement(raw, sqlite3_finalize);
}

std::string Text(sqlite3_stmt* stmt, int column) {
  const auto* text = sqlite3_column_text(stmt, column);
  return text ? reinterpret_cast<const char*>(text) : "";
}

void Bind(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<AccountRecord> ReadAccountRow(sqlite3_stmt* stmt) {
  if (!stmt || sqlite3_step(stmt) != SQLITE_ROW) return std::nullopt;
  AccountRecord record;
  record.id = sqlite3_column_int64(stmt, 0);
  record.username = Text(stmt, 1);
  record.password = {Text(stmt, 2), Text(stmt, 3)};
  return record;
}
}  // namespace

void DeviceDBManager::InitAccounts() {
  // 触发器保证设备被保留策略、管理后台或临时控制端清理时，同时移除账号绑定。
  const char* sql =
      "CREATE TABLE IF NOT EXISTS accounts ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "username TEXT NOT NULL UNIQUE COLLATE NOCASE,"
      "password_salt TEXT NOT NULL,"
      "password_hash TEXT NOT NULL,"
      "created_at INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS account_devices ("
      "device_id TEXT PRIMARY KEY,"
      "account_id INTEGER NOT NULL,"
      "device_name TEXT NOT NULL DEFAULT '',"
      "bound_at INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_account_devices_account "
      "ON account_devices(account_id);"
      "CREATE TRIGGER IF NOT EXISTS account_device_delete "
      "AFTER DELETE ON devices BEGIN "
      "DELETE FROM account_devices WHERE device_id=OLD.device_id; END;"
      "CREATE TRIGGER IF NOT EXISTS account_delete "
      "AFTER DELETE ON accounts BEGIN "
      "DELETE FROM account_devices WHERE account_id=OLD.id; END;"
      "DELETE FROM account_devices WHERE NOT EXISTS "
      "(SELECT 1 FROM devices WHERE devices.device_id=account_devices.device_id) "
      "OR NOT EXISTS (SELECT 1 FROM accounts WHERE accounts.id=account_devices.account_id);";
  char* error = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    LOG_ERROR("Failed to initialize accounts: {}", message);
    throw std::runtime_error("Failed to initialize accounts: " + message);
  }
}

std::pair<AccountCreateResult, int64_t> DeviceDBManager::CreateAccount(
    const std::string& username, const DevicePasswordRecord& record) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_,
      "INSERT INTO accounts(username,password_salt,password_hash,created_at) "
      "VALUES(?,?,?,?);");
  if (!stmt) return {AccountCreateResult::Failed, 0};
  Bind(stmt.get(), 1, username);
  Bind(stmt.get(), 2, record.salt);
  Bind(stmt.get(), 3, record.hash);
  sqlite3_bind_int64(stmt.get(), 4, Now());
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE)
    return {AccountCreateResult::Created, sqlite3_last_insert_rowid(db_)};
  if ((rc & 0xff) == SQLITE_CONSTRAINT) return {AccountCreateResult::Exists, 0};
  LOG_ERROR("Failed to create account: {}", sqlite3_errmsg(db_));
  return {AccountCreateResult::Failed, 0};
}

std::optional<AccountRecord> DeviceDBManager::ReadAccount(
    const std::string& username) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_,
      "SELECT id,username,password_salt,password_hash FROM accounts "
      "WHERE username=?;");
  if (!stmt) return std::nullopt;
  Bind(stmt.get(), 1, username);
  return ReadAccountRow(stmt.get());
}

std::optional<AccountRecord> DeviceDBManager::ReadAccountById(
    int64_t account_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_,
      "SELECT id,username,password_salt,password_hash FROM accounts "
      "WHERE id=?;");
  if (!stmt) return std::nullopt;
  sqlite3_bind_int64(stmt.get(), 1, account_id);
  return ReadAccountRow(stmt.get());
}

bool DeviceDBManager::BindDeviceToAccount(const std::string& device_id,
                                          int64_t account_id,
                                          const std::string& device_name) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_,
      "INSERT INTO account_devices(device_id,account_id,device_name,bound_at) "
      "SELECT ?1,?2,?3,?4 FROM devices WHERE device_id=?1 "
      "AND EXISTS (SELECT 1 FROM accounts WHERE id=?2) "
      "ON CONFLICT(device_id) DO UPDATE SET account_id=excluded.account_id,"
      "device_name=excluded.device_name,bound_at=CASE WHEN "
      "account_devices.account_id=excluded.account_id THEN account_devices.bound_at "
      "ELSE excluded.bound_at END;");
  if (!stmt) return false;
  Bind(stmt.get(), 1, device_id);
  sqlite3_bind_int64(stmt.get(), 2, account_id);
  Bind(stmt.get(), 3, device_name);
  sqlite3_bind_int64(stmt.get(), 4, Now());
  return sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool DeviceDBManager::UnbindDeviceFromAccount(const std::string& device_id,
                                              int64_t account_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_,
      "DELETE FROM account_devices WHERE device_id=?1 "
      "AND (?2=0 OR account_id=?2);");
  if (!stmt) return false;
  Bind(stmt.get(), 1, device_id);
  sqlite3_bind_int64(stmt.get(), 2, account_id);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

std::optional<int64_t> DeviceDBManager::DeviceAccountId(
    const std::string& device_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_,
      "SELECT account_id FROM account_devices WHERE device_id=?;");
  if (!stmt) return std::nullopt;
  Bind(stmt.get(), 1, device_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  return sqlite3_column_int64(stmt.get(), 0);
}

std::vector<AccountDevice> DeviceDBManager::ListAccountDevices(
    int64_t account_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  std::vector<AccountDevice> devices;
  auto stmt = Prepare(db_,
      "SELECT a.device_id,a.device_name,COALESCE(p.client_platform,''),"
      "COALESCE(p.online,0),a.bound_at FROM account_devices a "
      "LEFT JOIN device_presence p ON p.device_id=a.device_id "
      "WHERE a.account_id=? ORDER BY a.bound_at,a.device_id LIMIT 500;");
  if (!stmt) return devices;
  sqlite3_bind_int64(stmt.get(), 1, account_id);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    AccountDevice device;
    device.device_id = Text(stmt.get(), 0);
    device.name = Text(stmt.get(), 1);
    device.platform = Text(stmt.get(), 2);
    device.online = sqlite3_column_int(stmt.get(), 3) != 0;
    device.bound_at = sqlite3_column_int64(stmt.get(), 4);
    devices.push_back(std::move(device));
  }
  return devices;
}

int64_t DeviceDBManager::CountAccounts() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(db_, "SELECT COUNT(*) FROM accounts;");
  if (!stmt || sqlite3_step(stmt.get()) != SQLITE_ROW) return 0;
  return sqlite3_column_int64(stmt.get(), 0);
}
