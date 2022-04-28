LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_DEPS := \
	lib/io

ifndef WITH_CUSTOM_MALLOC
MODULE_DEPS += lib/heap
endif

# Generate a random 32-bit seed for the RNG
KERNEL_LIBC_RANDSEED_HEX := $(shell xxd -l4 -g0 -p /dev/urandom)
KERNEL_LIBC_RANDSEED := 0x$(KERNEL_LIBC_RANDSEED_HEX)U

MODULE_DEFINES += \
	KERNEL_LIBC_RANDSEED=$(KERNEL_LIBC_RANDSEED) \

$(info KERNEL_LIBC_RANDSEED = $(KERNEL_LIBC_RANDSEED))

MODULE_SRCS += \
	$(LOCAL_DIR)/atoi.c \
	$(LOCAL_DIR)/bsearch.c \
	$(LOCAL_DIR)/ctype.c \
	$(LOCAL_DIR)/errno.c \
	$(LOCAL_DIR)/printf.c \
	$(LOCAL_DIR)/rand.c \
	$(LOCAL_DIR)/strtol.c \
	$(LOCAL_DIR)/strtoll.c \
	$(LOCAL_DIR)/stdio.c \
	$(LOCAL_DIR)/qsort.c \
	$(LOCAL_DIR)/eabi.c \
	$(LOCAL_DIR)/eabi_unwind_stubs.c

ifeq ($(WITH_CPP_SUPPORT),true)
MODULE_SRCS += \
	$(LOCAL_DIR)/atexit.c \
	$(LOCAL_DIR)/pure_virtual.cpp
endif

include $(LOCAL_DIR)/string/rules.mk

include make/module.mk
