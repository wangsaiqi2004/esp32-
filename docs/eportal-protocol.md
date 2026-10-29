# ePortal（城市热点 / Dr.COM）认证协议记录

本文件记录从学校门户**自己的前端脚本**里逆向出的接口，用于实现上行门户自动登录。
不是猜测，除标注「未验证」的部分外均为实际抓取结果。

## 环境

| 项目 | 值 |
|---|---|
| 门户地址 | `http://172.16.29.11` |
| API 端口 | `8080` |
| 认证系统 | ePortal（Dr.COM / 城市热点） |
| 登录成功后落地页 | `/eportal/success.jsp?userIndex=<hex token>` |
| 前端脚本 | `/eportal/interface/index_files/js/AuthInterFace.js` |
| 另一种首页脚本 | `/eportal/interface/index_files/pc/login_bch.js` |

`userIndex` 形如一段十六进制串 + `_<客户端IP>_<学号>`，可作为会话标识。

## 接口

### 登录

```http
POST http://172.16.29.11:8080/eportal/InterFace.do?method=login
Content-Type: application/x-www-form-urlencoded; charset=UTF-8

userId=<学号>&password=<密码>&service=&queryString=<url编码的门户参数>
&operatorPwd=&operatorUserId=&validcode=&passwordEncrypt=false
```

前端脚本里的原始拼接（`AuthInterFace.js`）：

```js
var content = "userId=" + userId + "&password=" + password +
              "&service=" + service + "&queryString=" + queryString +
              "&operatorPwd=" + operatorPwd + "&operatorUserId=" + operatorUserId +
              "&validcode=" + validcode + "&passwordEncrypt=" + passwordEncrypt;
post(ePortalUrl + "login", content, callback);
```

其中 `ePortalUrl = url + "InterFace.do?method="`。

`queryString` 的取值来自被重定向时的 URL 参数，典型内容是
`wlanuserip=<客户端IP>&wlanacname=&nasip=&ssid=`，作为表单值必须整体 URL 编码：

```
wlanuserip%3D10.40.16.218%26wlanacname%3D%26nasip%3D%26ssid%3D
```

> **未验证**：本项目按上述惯例构造 `queryString`，没有拿真实凭据跑通过一次完整登录。
> 如果门户拒绝，返回值里会带原因，需据此调整。

### 保活

```http
POST http://172.16.29.11:8080/eportal/InterFace.do?method=keepalive
Content-Type: application/x-www-form-urlencoded; charset=UTF-8

userIndex=<登录响应中的 userIndex>
```

### 其他已知 method

| method | 用途 |
|---|---|
| `login` | 账号密码登录 |
| `keepalive` | 会话保活 |
| `getOnlineUserInfo` | 查询在线用户信息 |
| `freshOnlineUserInfo` | 刷新在线用户信息 |
| `loginWithQrCode` | 扫码登录 |
| `loginWithSmsVerifyCode` | 短信验证码登录 |

## 响应

返回 JSON，成功时含 `"result":"success"` 与 `"userIndex"` 字段（不同版本字段名略有差异）。
本项目的解析策略：先找 `"result":"success"`，再从 `"userIndex":"` 之后截取到下一个引号。

## 密码加密

脚本里保留了 RSA 分支：若页面存在 `publicKeyExponent` / `publicKeyModulus` 且
`passwordEncrypt == "true"`，密码需先做 RSA 加密（且是**字符串反转后**再加密）。
本项目所在学校**未启用**该分支，提交的是明文 + `passwordEncrypt=false`。
若换到启用了加密的学校，需要在 ESP32 上实现 RSA（mbedtls 可用），工作量明显增加。

## 断网探测

判断「是否真的能上外网」不能用 ICMP（校园网常封），也不建议只看 link-up。
本项目用两个独立的 204 端点，**必须返回 204 才算在线**：

```
http://connect.rom.miui.com/generate_204
http://wifi.vivo.com.cn/generate_204
```

注意：门户未认证时，HTTP 请求常被劫持并返回 200（登录页）或 302（跳转），
所以「状态码 200」不能当作在线判据，只有 204 才算。

## 反向借鉴

同类需求可参考未完成的二开项目 `mubeenhassanmeer/chameleonnat`（MAC 池 + 门户看门狗），
但注意它**同样没有关闭 WiFi 省电**，直接整包刷入会把吞吐打回 2.6 Mbps 一档。
