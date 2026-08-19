/**
 * @file ota_request.h
 * @brief 处理 WeatherClock 留在 EEPROM 中的 OTA 安装请求。
 */

#ifndef __OTA_REQUEST_H__
#define __OTA_REQUEST_H__

/**
 * @brief 检查并处理一次 OTA 安装请求。
 *
 * 请求只是应用到 Bootloader 的交接凭据。函数会重新验证外部槽中的完整
 * XBIN、签名和防降级策略，成功后才把 BCR 切换到 InstallPending。
 * 无请求或请求无效时，当前已确认固件和 BCR 均保持不变。
 */
void ota_request_process(void);

#endif /* __OTA_REQUEST_H__ */
