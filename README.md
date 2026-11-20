# CrossDesk Server

为 [CrossDesk](https://github.com/kunkundi/crossdesk) 提供设备注册、在线状态、远程会话信令与临时 TURN 凭据，内置 Web 管理后台。信令使用 WSS，设备资料、在线状态与活动会话使用 SQLite 持久化。

[English](README_EN.md) · [下载发布配置](https://github.com/kunkundi/crossdesk-server/releases) · [Docker 镜像](https://hub.docker.com/r/crossdesk/crossdesk-server/tags) · [客户端](https://github.com/kunkundi/crossdesk) · [Web 客户端](https://github.com/kunkundi/crossdesk-web-client)

[![Build](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml/badge.svg)](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/kunkundi/crossdesk-server)](https://github.com/kunkundi/crossdesk-server/releases)
[![Docker Pulls](https://img.shields.io/docker/pulls/crossdesk/crossdesk-server)](https://hub.docker.com/r/crossdesk/crossdesk-server)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)

[部署服务](#运行服务) · [连接客户端](#clients) · [账号](#accounts) · [管理后台](#admin) · [状态接口](#stats) · [维护与排查](#operations) · [源码构建](#build)

## 组件与能力

| 组件 | 职责 |
| --- | --- |
| CrossDesk Server | WSS 信令、设备身份与密码验证、在线状态、会话协调、临时 TURN 凭据 |
| Coturn | 在客户端无法直连时中继媒体与数据；由独立容器运行 |
| SQLite | 保存设备记录、在线状态及活动会话信息 |
| Web 管理后台 | 在线与日活指标、客户端与会话管理，设备数据查询 / 导出 / 清理 |

客户端优先尝试 P2P 直连，必要时通过 TURN 中继。当前 [Compose](compose.yaml) 和 [CI](.github/workflows/build.yml) 面向 **Linux amd64 / arm64**；发布镜像以 Ubuntu 22.04 为运行环境。

<a id="运行服务"></a>

## 快速部署

准备安装了 Docker Engine 与 Compose 插件的 **Linux amd64 / arm64** 服务器，部署使用 host 网络。从**同一 [Release](https://github.com/kunkundi/crossdesk-server/releases)** 下载 `compose.yaml` 和 `env.example` 到独立部署目录，执行：

```sh
cp env.example .env
openssl rand -hex 32
openssl rand -hex 32
```

启动前编辑 `.env`：保留对应发布版本的固定镜像 tag，`EXTERNAL_IP` 填公网 IP，`INTERNAL_IP` 填 Coturn 能绑定的本机地址。将上面两次生成的不同密钥分别填入 `COTURN_AUTH_SECRET` 和 `COTURN_STATELESS_NONCE_SECRET`。源码仓库的文件名是 [`.env.example`](.env.example)，默认镜像为 `latest`，使用时需自行选择固定 tag。

按配置同时放通防火墙与云安全组：

| 默认端口 | 协议 | 服务 |
| --- | --- | --- |
| `9099` | TCP | WSS 信令、HTTPS 状态接口及后台 |
| `3478` | TCP / UDP | STUN / TURN |
| `50000`–`60000` | UDP | TURN 中继 |

在配置目录执行：

```sh
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
sudo docker compose ps
```

首次启动生成证书与数据库，随后按下文完成**证书信任与客户端配置**。Compose 健康检查仅检查证书文件，仍需验证 HTTPS 与真实远控连接。Compose 默认端口为 `9099`，独立二进制默认端口为 `9090`。

[完整部署指南](docs/DEPLOYMENT.md) · [自定义 ICE / TURN 节点](docs/DEPLOYMENT.md#ice)

<a id="clients"></a>
<a id="证书与客户端连接"></a>

## 证书与客户端

自动生成的证书只覆盖 `EXTERNAL_IP`。将本次部署的 `${CROSSDESK_DATA_DIR}/certs/api.crossdesk.cn_root.crt` 导入各客户端信任库；使用域名时需提供匹配的证书链和私钥。修改 `.env` 的 IP 不会重新生成已有证书。

桌面端不再内置公共服务器。打开 **设置 → 常规 → 服务器**，填写主机和信令端口并**保存**；两端必须使用同一服务器，TURN 地址由服务端下发。Web 的 WSS 地址与浏览器证书信任见 [Web 部署指南](https://github.com/kunkundi/crossdesk-web-client/blob/HEAD/docs/DEPLOYMENT.md#configuration)。

[证书文件、各系统信任命令与客户端配置](docs/DEPLOYMENT.md#clients)

<a id="accounts"></a>

## 账号与免密连接

桌面端可在本服务上注册、登录账号。账号绑定到设备身份（设备密钥），客户端不保存账号密码或令牌：

- 同一账号下的设备在“我的设备”中显示在线状态，互相连接时由服务端授权，无需输入设备连接密码；其他设备仍按原流程验证密码。
- 账号密码使用与设备密码相同的 Argon2id 摘要，登录失败按“设备 + 用户名”累计退避，记录显示在后台风控页（操作类型“账号登录”）。
- 只有通过设备密钥认证的设备主连接可以登录账号；控制端连接、Web / CLI 临时身份和旧版密码身份会被拒绝。每个账号最多绑定 100 台设备，设备被清理时账号绑定随之删除。
- 默认允许客户端自助注册。在 `.env` 中设置 `CROSSDESK_ACCOUNT_REGISTRATION=0` 并重新创建容器后，只能登录已有账号。

账号数据保存在同一 SQLite 数据库的 `accounts` / `account_devices` 表中，随数据库一同备份。升级后旧客户端不受影响，但只有新客户端提供账号入口。

<a id="admin"></a>

## 后台管理

在 `.env` 中**同时设置** `ADMIN_USERNAME` 和 `ADMIN_PASSWORD`，执行 `sudo docker compose up -d --no-build` 重新创建容器，然后打开 `https://你的服务器:9099/admin`。

| 页面 | 用途 |
| --- | --- |
| 运行概览 | 电脑 / 移动端 / Web 在线数、控制端日活、活动会话、筛选与断开会话 |
| 数据管理 | 精确查询设备、导出 JSON 摘要、复核管理员密码后清理数据 |
| 关于 | 服务端版本、许可证和项目链接 |

统计口径、恢复逻辑、登录限制和数据操作范围见 [后台管理指南](docs/ADMIN.md)。断开远控会话不会让设备本身离线。

<a id="stats"></a>

## 检查服务状态

替换示例 IP，并使用本次部署的根证书；自定义数据目录时同步调整路径：

```sh
curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://203.0.113.10:9099/stats
```

`/stats` 与 `/api/stats` 无需管理登录，提供汇总数据，不返回设备明细；[字段说明](docs/DEPLOYMENT.md#stats)。还需实际连接客户端，验证媒体与中继链路。

<a id="operations"></a>

## 日常维护

- 持久化数据与日志目录，默认 `/var/lib/crossdesk` 和 `/var/log/crossdesk`。应用日志写入文件，Compose 容器日志已关闭。
- 升级前备份 `.env`、`compose.yaml`、证书与数据库。密码验证成功后会升级旧摘要，旧服务端无法验证升级后的记录，回滚需使用升级前备份。
- 按部署规模核对留存与容量。默认日志保留 185 天、离线设备 180 天后清理、信令连接上限 8,192；这些是配置默认值，不是吞吐保证。

[日志与升级](docs/OPERATIONS.md) · [认证、恢复、容量、留存与备份](docs/OPERATIONS.md#operations)

<a id="build"></a>

## 从源码构建

使用 Git、C++17 编译器与 Xmake；发布基线为 Ubuntu 22.04、amd64 / arm64。[构建指南](docs/DEPLOYMENT.md#build) 包含依赖安装、现有回归测试及镜像打包步骤；Dockerfile 用于打包已编译的二进制。

## 反馈与许可

[提交问题](https://github.com/kunkundi/crossdesk-server/issues)时请附上服务端 tag、两端客户端版本、部署方式及脱敏日志。CrossDesk Server 使用 [LGPL-3.0](LICENSE) 许可。
