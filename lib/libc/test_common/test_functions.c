/*
 * Copyright (c) 2022 LK Trusty Authors. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdarg.h>
#include "printf_test.h"

struct _output_args {
    char *outstr;
    size_t len;
    size_t pos;
};

static int _vsnprintf_output(const char *str, size_t len, void *state)
{
    struct _output_args *args = state;

    size_t count = 0;
    while (count < len) {
        if (args->pos < args->len) {
            args->outstr[args->pos++] = *str;
        }

        str++;
        count++;
    }

    return count;
}

/*
* vsnprintf_filtered and snprintf_filtered were added to test that klog, printf and fprintf
* filtering is working. Normal sprintf variants are currently not being filtered.
*/

int vsnprintf_filtered(char *str, size_t len, const char *fmt, va_list ap)
{
    struct _output_args args;
    int wlen;

    args.outstr = str;
    args.len = len;
    args.pos = 0;

    wlen = _printf_engine(&_vsnprintf_output, (void *)&args, fmt, ap);
    if (args.pos >= len)
        str[len-1] = '\0';
    else
        str[wlen] = '\0';
    return wlen;
}

int snprintf_filtered(char *str, size_t len, const char *fmt, ...)
{
    int err;

    va_list ap;
    va_start(ap, fmt);
    err = vsnprintf_filtered(str, len, fmt, ap);
    va_end(ap);

    return err;
}
