/**
 * @file syscalls.c
 * @brief Newlib 所需的最小裸机系统调用。
 *
 * printf 最终通过 _write() 调用板级 __io_putchar()。动态内存从链接符号
 * _end 向上增长，并始终给主栈保留 _Min_Stack_Size 指定的空间。
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

extern char _end;
extern char _sstack;

/**
 * @brief 板级单字符输出的弱默认实现。
 * @param ch 待输出字符。
 * @return 原字符值。
 */
__attribute__((weak)) int __io_putchar(int ch)
{
    return ch;
}

int _write(int file, char *buffer, int length)
{
    int i;

    (void)file;
    for (i = 0; i < length; ++i)
        __io_putchar((unsigned char)buffer[i]);
    return length;
}

void *_sbrk(ptrdiff_t increment)
{
    static char *heap_end;
    char *previous;

    if (heap_end == NULL)
        heap_end = &_end;

    previous = heap_end;
    if (heap_end + increment > &_sstack)
    {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_end += increment;
    return previous;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _fstat(int file, struct stat *status)
{
    (void)file;
    status->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _lseek(int file, int offset, int origin)
{
    (void)file;
    (void)offset;
    (void)origin;
    return 0;
}

int _read(int file, char *buffer, int length)
{
    (void)file;
    (void)buffer;
    (void)length;
    return 0;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int signal)
{
    (void)pid;
    (void)signal;
    errno = EINVAL;
    return -1;
}

void _exit(int status)
{
    (void)status;
    while (1)
    {
    }
}

