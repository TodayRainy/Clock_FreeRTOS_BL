#include <stdio.h>
#include "elog.h"
#include "console.h"
#include "tim_delay.h"

ElogErrCode elog_port_init(void)
{
    return ELOG_NO_ERR;
}

void elog_port_deinit(void)
{

}

void elog_port_output(const char *log, size_t size)
{
    console_write(log, size);
}

void elog_port_output_lock(void)
{

}

void elog_port_output_unlock(void)
{

}

const char *elog_port_get_time(void)
{
    static char time_str[16] = {0};
    uint32_t ms = tim_get_ms() % (3600 * 1000);
    uint32_t fmt_mm = ms / (60 * 1000);
    uint32_t fmt_ss = (ms % (60 * 1000)) / 1000;
    uint32_t fmt_ms = ms % 1000;
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%03d", fmt_mm, fmt_ss, fmt_ms);
    return time_str;
}

const char *elog_port_get_p_info(void)
{
    return "";
}

const char *elog_port_get_t_info(void)
{
    return "";
}
