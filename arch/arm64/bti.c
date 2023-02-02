/*
 * Copyright (c) 2022, Google, Inc. All rights reserved
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

#include <arch/arm64.h>
#include <arch/ops.h>
#include <inttypes.h>

#define ID_AA64PFR1_EL1_BTI_SHIFT 0u
#define ID_AA64PFR1_EL1_BIT_MASK 0xfu
#define ID_AA64PFR1_EL1_BTI_NOT_SUPPORTED 0x0u
#define ID_AA64PFR1_EL1_BTI_SUPPORTED 0x1u

static uint8_t arm64_bti_support_level(void) {
    uint64_t v = ARM64_READ_SYSREG(id_aa64pfr1_el1);
    return ((v >> ID_AA64PFR1_EL1_BTI_SHIFT) & ID_AA64PFR1_EL1_BIT_MASK);
}

bool arch_bti_supported(void) {
    return arm64_bti_support_level() != ID_AA64PFR1_EL1_BTI_NOT_SUPPORTED;
}
