/**
 * @file test_firmware_security_policy.c
 * @brief 在宿主机上验证Bootloader与应用共用的防降级规则。
 */

#include <assert.h>
#include <stdio.h>
#include "firmware_security_policy.h"

int main(void)
{
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

    puts("Anti-rollback policy tests passed.");
    return 0;
}
