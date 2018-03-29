# Copyright 2018 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

LOCAL_DIR := $(GET_LOCAL_DIR)

PLATFORM := generic-arm

MODULE := $(LOCAL_DIR)

BOOT_SHIM_SRC := $(LOCAL_DIR)/boot-shim.S
BOOT_SHIM_OBJ := $(BUILDDIR)/boot-shim.o
BOOT_SHIMC_SRC := $(LOCAL_DIR)/boot-shim.c
BOOT_SHIMC_OBJ := $(BUILDDIR)/boot-shim.c.o
BOOT_SHIM_ELF := $(BUILDDIR)/boot-shim.elf
BOOT_SHIM_BIN := $(BUILDDIR)/boot-shim.bin

BOOT_SHIM_ALIGN := 65536
SHIM_ASM_DEFINES := -DBOOT_SHIM_SIZE=$(BOOT_SHIM_ALIGN)

$(BOOT_SHIM_OBJ): $(BOOT_SHIM_SRC)
	@$(MKDIR)
	$(call BUILDECHO, compiling $<)
	$(NOECHO)$(CC) -Ikernel/include -Ikernel/arch/arm64/include -Isystem/public $(SHIM_ASM_DEFINES) -c $< -MMD -MP -MT $@ -MF $(@:%o=%d) -o $@

$(BOOT_SHIMC_OBJ): $(BOOT_SHIMC_SRC)
	@$(MKDIR)
	$(call BUILDECHO, compiling $<)
	$(NOECHO)$(CC) -c $< -o $@

$(BOOT_SHIM_ELF): $(BOOT_SHIM_OBJ) $(BOOT_SHIMC_OBJ)
	$(call BUILDECHO,linking $@)
	$(NOECHO)$(LD) $(BOOT_SHIM_OBJ) $(BOOT_SHIMC_OBJ) -o $@

$(BOOT_SHIM_BIN): $(BOOT_SHIM_ELF)
	$(call BUILDECHO,generating $@)
	$(NOECHO)$(OBJCOPY) -O binary $< $@

GENERATED += $(BOOT_SHIM_BIN)

# include rules for our various arm64 boards
include $(LOCAL_DIR)/*/rules.mk
