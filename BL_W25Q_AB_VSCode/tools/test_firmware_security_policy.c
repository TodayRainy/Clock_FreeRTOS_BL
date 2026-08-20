/**
 * @file test_firmware_security_policy.c
 * @brief 在宿主机上验证安全防降级和自动 OTA 发布顺序规则。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware_release_policy.h"
#include "firmware_security_policy.h"

int main(void)
{
    uint8_t current_sha[32] = {0};
    uint8_t other_sha[32];

    /* 正常升级允许相同或更高的安全版本，拒绝更低版本。 */
    assert(firmware_security_version_allowed(7u, 7u, false));
    assert(firmware_security_version_allowed(7u, 8u, false));
    assert(!firmware_security_version_allowed(7u, 6u, false));

    /* KEY2只放行本次安装，不会让持久化安全下限倒退。 */
    assert(firmware_security_version_allowed(7u, 6u, true));
    assert(firmware_security_floor_after_confirm(7u, 6u) == 7u);
    assert(firmware_security_floor_after_confirm(7u, 8u) == 8u);

    /* Trial失败没有确认动作，因此旧下限自然保持不变。 */
    assert(firmware_security_floor_after_confirm(7u, 7u) == 7u);

    memcpy(other_sha, current_sha, sizeof(other_sha));
    other_sha[31] = 1u;
    assert(firmware_release_decide(10u, current_sha, 11u, other_sha) ==
           FIRMWARE_RELEASE_UPDATE);
    assert(firmware_release_decide(10u, current_sha, 9u, other_sha) ==
           FIRMWARE_RELEASE_OLDER);
    assert(firmware_release_decide(10u, current_sha, 10u, current_sha) ==
           FIRMWARE_RELEASE_CURRENT);
    assert(firmware_release_decide(10u, current_sha, 10u, other_sha) ==
           FIRMWARE_RELEASE_CONFLICT);

    puts("Firmware security/release policy tests passed.");
    return 0;
}
