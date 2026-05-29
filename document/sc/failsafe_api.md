[English](../failsafe_api.md) | [简体中文](#)

# Failsafe Web UI API 文档

本文档根据当前代码实现整理（`failsafe/*.c`），用于说明 U-Boot failsafe Web UI 的 HTTP 接口、参数与返回格式。

---

## 1. 服务入口与运行方式

- 命令入口：`httpd`
- 实现位置：`failsafe/failsafe.c` 的 `do_httpd()` / `start_web_failsafe()`
- 默认监听端口：`80`
- 运行阶段：U-Boot 阶段（非 Linux 用户态）

启动后会打印：

- `Web failsafe UI started`
- `URL: http://<ip>/`

---

## 2. 功能开关（Kconfig）

主开关：

- `CONFIG_WEBUI_FAILSAFE`

UI 资源风格（3 选 1）：

- `CONFIG_WEBUI_FAILSAFE_UI_BOOTSTRAP`（`fsdata/`）
- `CONFIG_WEBUI_FAILSAFE_UI_MTK`（`fsdata_mtk/`）
- `CONFIG_WEBUI_FAILSAFE_UI_GL`（`fsdata_gl/`）

可选能力：

- `CONFIG_WEBUI_FAILSAFE_ADVANCED`：高级功能总开关（backup/flash/env/console 依赖此项）
- `CONFIG_WEBUI_FAILSAFE_I18N`：`/i18n.js`
- `CONFIG_WEBUI_FAILSAFE_BACKUP`：备份下载接口
- `CONFIG_WEBUI_FAILSAFE_FLASH`：Flash 编辑接口
- `CONFIG_WEBUI_FAILSAFE_ENV`：环境变量管理接口
- `CONFIG_WEBUI_FAILSAFE_ENV_CUSTOM_ENV`：首次访问时加载自定义环境变量
- `CONFIG_WEBUI_FAILSAFE_ENV_CUSTOM_ENV_FILE`：自定义环境变量文件路径（默认 `defenvs/custom_env`）
- `CONFIG_WEBUI_FAILSAFE_ENV_DEFAULT_GLBTN_KEY`：自动创建默认 `glbtn_key`
- `CONFIG_WEBUI_FAILSAFE_CONSOLE`：Web 控制台接口
- `CONFIG_WEBUI_FAILSAFE_UBI`：UBI 卷管理接口
- `CONFIG_WEBUI_FAILSAFE_FACTORY`：factory 升级入口
- `CONFIG_WEBUI_FAILSAFE_SIMG`：simg 升级入口
- `CONFIG_WEBUI_FAILSAFE_BUILD_VARIANT`：构建变体标签字符串

---

## 3. 通用约定

### 3.1 请求与编码

- 前端主要使用 `multipart/form-data`（`FormData`）提交 POST。
- GET 接口较少，主要用于静态内容和查询类 API。

### 3.2 返回类型

- 页面/静态资源：`text/html` / `text/css` / `text/javascript` / 图片 MIME
- 文本状态：`text/plain`
- 结构化数据：`application/json`
- 备份下载：`application/octet-stream`（自定义头 + 流式 body）

### 3.3 错误处理风格

- 一部分接口返回纯文本错误（如 `bad request`、`save failed`）
- 一部分接口返回 JSON 错误（如 `{"ok":false,"error":"bad_range"}`）

---

## 4. 主流程 API（固件上传/写入/重启）

### 4.1 `POST /upload`

上传固件到内存并校验，不立即写入 Flash。

支持的文件字段（多选一）：

- `firmware`
- `fip`
- `bl2`
- `initramfs`
- `gpt`（需 `CONFIG_MTK_BOOTMENU_MMC`）
- `factory`（需 `CONFIG_WEBUI_FAILSAFE_FACTORY`）
- `simg`（需 `CONFIG_WEBUI_FAILSAFE_SIMG`）

可选字段：

- `mtd_layout`（`CONFIG_MEDIATEK_MULTI_MTD_LAYOUT` 时生效）

成功返回（纯文本）：

- `"<size> <md5>"`
- 或 `"<size> <md5> <mtd_layout>"`

失败返回：

- `fail`

说明：

- `firmware/fip/bl2/factory/simg` 走 `failsafe_validate_image()`
- `initramfs` 走 `fdt_check_header()`

### 4.2 `POST /result`（或页面触发请求）

执行写入动作并返回结果。

返回：

- `success`：写入成功
- `failed`：写入失败

行为要点：

- 仅当 `upload_data_id == upload_id` 时才执行写入。
- `initramfs` 不落盘，视作成功（后续可直接内存启动）。
- 成功后可能触发自动动作（重启或 `boot_from_mem`），受 `failsafe_auto_reboot` 与镜像类型影响。

### 4.3 `POST /reboot`

- 返回：`rebooting`
- 在连接关闭后执行 `do_reset()`。

### 4.4 `POST /reboot-failsafe`

- 先执行：`env_set("failsafe", "1")` + `env_save()`
- 成功返回：`rebooting to failsafe`
- 失败返回：`failsafe env set failed`（HTTP 500）
- 连接关闭后重启。

---

## 5. 设备与系统信息 API

### 5.1 `GET /version`

返回 U-Boot 版本字符串（可拼接 `CONFIG_WEBUI_FAILSAFE_BUILD_VARIANT`）。

类型：`text/plain`

### 5.2 `GET /sysinfo`

返回系统信息 JSON，包含：

- `board.model`
- `board.compatible`
- `ram.size`
- `build_variant`
- `storage.mtd_layout`（若支持多布局）
- `storage.mmc`（是否存在、厂商、容量、分区等）

类型：`application/json`

### 5.3 `GET /getmtdlayout`

- 多布局启用时：返回 `"当前布局;布局1;布局2;...;"`
- 否则返回：`error`

类型：`text/plain`

---

## 6. 备份与 Flash 编辑 API

> 需 `CONFIG_WEBUI_FAILSAFE_BACKUP`，其中 Flash 编辑还需 `CONFIG_WEBUI_FAILSAFE_FLASH`。

### 6.1 `GET /backup/info`

返回备份/编辑所需的存储拓扑信息（JSON）：

- `mmc.present/vendor/product/blksz/size/parts[]`
- `mtd.present/model/type/parts[]`

类型：`application/json`

### 6.2 `POST /backup/main`

按分区或范围导出二进制备份流。

请求参数：

- `mode`: `part` | `range`
- `storage`: `auto` | `mtd` | `mmc`（可选）
- `target`: 目标（支持 `mtd:<name>` / `mmc:<name>` 前缀）
- `start`、`end`：仅 `mode=range` 必填，支持十进制、十六进制、`k/kb/kib`

成功响应：

- `HTTP 200`，`application/octet-stream`
- 带 `Content-Length`
- 带 `Content-Disposition: attachment; filename="..."`

典型错误：

- `400 bad request`
- `400 invalid range`
- `404 target not found`
- `500 no mem`

### 6.3 `POST /flash/read`

请求参数：

- `op=read`（可选，URI 也可推断）
- `storage`、`target`、`start`、`end`

成功返回：

- `{"ok":true,"start":"0x...","end":"0x...","size":N,"data":"aa bb ..."}`

限制：

- 单次读取最大 `4096` 字节（`FLASH_EDIT_MAX_READ`）

### 6.4 `POST /flash/write`

请求参数：

- `op=write`
- `storage`、`target`、`start`
- `data`：hex 文本（允许空格/换行/`0x` 前缀）

成功返回：

- `{"ok":true,"written":N}`

### 6.5 `POST /flash/erase`

请求参数：

- `op=erase`
- `storage`、`target`
- `start/end` 可选；都不传时擦除整个目标

成功返回：

- `{"ok":true,"erased":N,"start":"0x...","end":"0x..."}`

### 6.6 `POST /flash/restore`

请求参数（两种模式）：

1) 自动解析文件名模式（推荐）：

- `backup`（或 `file`）上传备份文件
- 文件名若匹配 `backup_<stype>_<model>_<target>_0x<start>-0x<end>.bin`，可自动推导目标与范围

1) 手工指定模式：

- `backup`（或 `file`）
- `storage`、`target`、`start`、`end`

成功返回：

- `{"ok":true,"restored":N}`

### 6.7 Flash 接口统一错误 JSON

- `{"ok":false,"error":"bad_request"}`
- `{"ok":false,"error":"target_not_found"}`
- `{"ok":false,"error":"bad_range"}`
- `{"ok":false,"error":"bad_hex"}`
- `{"ok":false,"error":"too_large"}`
- `{"ok":false,"error":"io"}`
- `{"ok":false,"error":"oom"}`
- `{"ok":false,"error":"method"}`（非 POST）
- `{"ok":false,"error":"no_op"}` / `unknown_op`

---

## 7. 环境变量管理 API

> 需 `CONFIG_WEBUI_FAILSAFE_ENV`

### 7.1 `GET /env/list`

- 返回环境变量文本（每行 `key=value`）
- 错误：`405 method` / `500 export failed`

### 7.2 `POST /env/set`

参数：

- `name`（必填）
- `value`（可空）

成功：`ok`

常见错误：

- `400 bad name`
- `400 bad value`
- `500 save failed`

### 7.3 `POST /env/unset`

参数：

- `name`（必填）

成功：`ok`

错误：`400 bad name` / `500 save failed`

### 7.4 `POST /env/reset`

- 恢复默认环境并保存
- 成功：`ok`
- 错误：`500 save failed`

### 7.5 `POST /env/restore`

参数：

- `envfile`（二进制环境文件，大小至少 `sizeof(env_t)`）

成功：`ok`

错误：

- `400 bad file`
- `500 restore failed`

### 7.6 `GET /env/size`

返回环境变量存储区的已用/总容量。

成功返回（纯文本）：

- `"<used>/<total> bytes"`

类型：`text/plain`

错误：`405 method` / `500 export failed`

---

## 8. Web 控制台 API

> 需 `CONFIG_WEBUI_FAILSAFE_CONSOLE`

安全机制：

- 若设置环境变量 `failsafe_console_token`，则接口要求 POST 且表单携带 `token`，不匹配返回 `403 forbidden`。

### 8.1 `POST /console/poll`

轮询控制台输出缓冲。

成功返回：

- `{"data":"...","avail":<remaining>}`

错误：

- `405 {"error":"method"}`
- `403 forbidden`
- `503 {"error":"no_console"}`
- `500 {"error":"oom"}`

### 8.2 `POST /console/exec`

参数：

- `cmd`（必填，最大 256 字节）
- `token`（若启用令牌）

成功返回：

- `{"ok":true,"ret":<run_command返回值>,"cmd":"..."}`

### 8.3 `POST /console/clear`

清空控制台录制缓冲。

成功返回：

- `{"ok":true}`

---

## 9. UBI 卷管理 API

> 需 `CONFIG_WEBUI_FAILSAFE_UBI`

### 9.1 `GET /ubi/info`

返回当前 UBI 设备信息（JSON）。

成功返回：

- `{"attached":true,"mtd_name":"...","ubi_num":0,"flash_size":N,"peb_size":N,"leb_size":N,"good_peb_count":N,"bad_peb_count":N,"min_io_size":N,"max_vol_count":N,"vol_count":N,"avail_pebs":N,"rsvd_pebs":N,"beb_rsvd_pebs":N,"max_ec":N,"mean_ec":N}`
- 若未附加：`{"error":"no ubi device","attached":false}`

类型：`application/json`

### 9.2 `GET /ubi/volumes`

返回 UBI 卷列表（JSON）。

成功返回：

- `{"volumes":[{"id":0,"name":"...","size":N,"used_bytes":N,"type":"dynamic|static","corrupted":0,"upd_marker":0,"skip_check":0,"reserved_peb":N,"alignment":N,"data_pad":N,"usable_leb_size":N},...]}`
- 若未附加：`{"error":"no ubi device","volumes":[]}`

类型：`application/json`

### 9.3 `GET /ubi/mtd_list`

返回可用 MTD 分区列表（JSON）。

成功返回：

- `{"partitions":[{"name":"...","size":N,"erasesize":N},...]}`

类型：`application/json`

### 9.4 `POST /ubi/attach`

将 UBI 附加到指定 MTD 分区。

参数：

- `mtd_name`（必填）：MTD 分区名

成功：`{"ok":true}`

错误：

- `400 {"error":"missing mtd_name"}`
- `400 {"error":"no ubi device attached"}`
- `500 {"error":"attach failed: N"}`

### 9.5 `POST /ubi/detach`

分离当前 UBI 设备。

参数：无

成功：`{"ok":true}`

错误：

- `500 {"error":"detach failed: N"}`

### 9.6 `POST /ubi/create`

创建新的 UBI 卷。

参数：

- `name`（必填）：卷名称
- `size`（可选）：卷大小（字节），0 或不传表示使用最大可用空间
- `type`（可选）：`dynamic`（默认）或 `static`
- `skipcheck`（可选）：`1` 跳过 CRC 检查，`0`（默认）

成功：`{"ok":true}`

错误：

- `400 {"error":"missing volume name"}`
- `400 {"error":"no ubi device attached"}`
- `500 {"error":"create failed: N"}`

### 9.7 `POST /ubi/remove`

删除指定 UBI 卷。

参数：

- `name`（必填）：卷名称

成功：`{"ok":true}`

错误：

- `400 {"error":"missing volume name"}`
- `400 {"error":"no ubi device attached"}`
- `500 {"error":"remove failed: N"}`

### 9.8 `POST /ubi/rename`

重命名 UBI 卷。

参数：

- `old_name`（必填）：当前卷名称
- `new_name`（必填）：新卷名称

成功：`{"ok":true}`

错误：

- `400 {"error":"missing old_name"}`
- `400 {"error":"missing new_name"}`
- `400 {"error":"no ubi device attached"}`
- `404 {"error":"volume not found"}`
- `500 {"error":"rename failed: N"}`

### 9.9 `POST /ubi/backup`

下载 UBI 卷内容为二进制文件。

参数：

- `name`（必填）：卷名称

成功响应：

- `HTTP 200`，`application/octet-stream`
- 带 `Content-Length`
- 带 `Content-Disposition: attachment; filename="ubi_<name>.bin"`

错误：

- `400 missing name`
- `400 no ubi device`
- `404 volume not found`
- `500 oom`

---

## 10. 主题与图标 API（BOOTSTRAP UI）

> `theme_*` 接口注册条件：`CONFIG_WEBUI_FAILSAFE_UI_BOOTSTRAP`

### 10.1 `GET /theme/get`

返回：

- `{"ok":true,"color":"#rrggbb","theme":"auto|light|dark","dark_variant":"standard|amoled"}`

### 10.2 `POST /theme/set`

参数（都可选，传了才更新）：

- `color`：`#RGB` / `#RRGGBB`（内部规范化为 `#rrggbb`）
- `theme`：`auto|light|dark`
- `dark_variant`：`standard` | `amoled`

成功：

- `{"ok":true}`

错误：

- `400 {"ok":false,"error":"bad_color"}`
- `400 {"ok":false,"error":"bad_theme"}`
- `400 {"ok":false,"error":"bad_dark_variant"}`
- `500 {"ok":false,"error":"save"}`

### 10.3 `GET /favicon.svg`（及图片资源）

- 由 `picture_handler()` 提供
- `favicon.ico` 找不到时会回退到 `favicon.svg`

---

## 11. 静态页面与资源路由

已注册页面（按配置条件可能增减）：

- `/`、`/uboot.html`、`/bl2.html`、`/initramfs.html`
- `/booting.html`、`/flashing.html`、`/fail.html`、`/reboot.html`
- `/gpt.html`（MMC 存在时）
- `/backup.html`、`/flash.html`、`/env.html`、`/console.html`
- `/ubi.html`（需 `CONFIG_WEBUI_FAILSAFE_UBI`）
- `/settings.html`（新 UI）
- `/factory.html`、`/simg.html`

OpenWrt 兼容路由：

- `/cgi-bin/luci`、`/cgi-bin/luci/` → 重定向到 `/`

静态资源：

- `/style.css`
- `/main.js`
- `/backup_js.js`（启用 backup）
- `/flash_js.js`（启用 flash）
- `/env_js.js`（启用 env）
- `/console_js.js`（启用 console）
- `/ubi_js.js`（启用 UBI）
- `/settings_js.js`（新 UI）
- `/theme.js`（新 UI）
- `/i18n.js`（启用 I18N）
- `/favicon.svg`（新 UI）

未命中路由：

- 返回 `/404.html`，HTTP 状态码 `404`

---

## 12. 升级后自动动作说明

在 `/result` 完成后：

- 若写入成功，`upgrade_success = true`
- `auto_action_pending` 触发条件：
  - 固件类型为 `initramfs`，或
  - `failsafe_auto_reboot` 判定为启用

`failsafe_auto_reboot` 判定：

- 环境变量值为 `1/true/yes/on` 时启用
- 若未设置：`UI_GL` 或 `UI_MTK` 默认启用；`UI_BOOTSTRAP` 默认关闭

后续动作：

- `initramfs`：`boot_from_mem(upload_data)`
- 其他：`do_reset()`

---

## 13. 安全提示

- `flash`、`env`、`console`、`ubi` 都属于高风险能力，可能导致设备不可启动。
- `console` 本质是网络命令执行入口；建议至少设置 `failsafe_console_token`，并仅在可信网络使用。
- `ubi` 操作（attach/detach/create/remove/rename）直接影响 NAND 存储结构，误操作可能导致数据丢失。

---

## 14. 源码模块结构

failsafe Web UI 源码位于 `failsafe/` 目录，重构后的模块划分：

| 模块 | Kconfig 守卫 | 说明 |
|---|---|---|
| `failsafe.c` | （核心） | HTTP 路由注册、主流程（upload/result/reboot/sysinfo） |
| `failsafe_helpers.c/h` | （核心） | 公共 HTTP 回复、表单解析、会话管理、存储目标操作等共享工具函数 |
| `failsafe_internal.h` | （核心） | 模块间内部接口声明 |
| `failsafe_backup.c` | `CONFIG_WEBUI_FAILSAFE_BACKUP` | 备份下载（`/backup/main`） |
| `failsafe_flash.c` | `CONFIG_WEBUI_FAILSAFE_FLASH` | Flash 读写擦恢复（`/flash/*`） |
| `failsafe_env.c` | `CONFIG_WEBUI_FAILSAFE_ENV` | 环境变量管理（`/env/*`） |
| `failsafe_console.c` | `CONFIG_WEBUI_FAILSAFE_CONSOLE` | Web 控制台（`/console/*`） |
| `failsafe_ubi.c` | `CONFIG_WEBUI_FAILSAFE_UBI` | UBI 卷管理（`/ubi/*`） |
| `failsafe_theme.c` | `CONFIG_WEBUI_FAILSAFE_UI_BOOTSTRAP` | 主题设置（`/theme/*`） |

（文档完）
