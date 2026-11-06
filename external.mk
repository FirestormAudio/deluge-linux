# Deluge BR2_EXTERNAL glue.
#
# The board's kernel and U-Boot customisation lives in board/synthstrom/deluge/
# as files mirroring the upstream tree paths, and is stamped into Buildroot's
# throwaway source tree by the install scripts below. They run as pre-patch
# hooks, so BR2_GLOBAL_PATCH_DIR patches (if any) apply on top of the new files.
#
# Hook lists are expanded lazily inside the package recipes
# (package/pkg-generic.mk), so appending to them here — after linux/linux.mk and
# boot/uboot/uboot.mk have been included — still takes effect.

DELUGE_SCRIPTS := $(BR2_EXTERNAL_DELUGE_PATH)/scripts

define DELUGE_INSTALL_KERNEL_FILES
	$(DELUGE_SCRIPTS)/install-deluge-drivers.sh $(@D)
endef
LINUX_PRE_PATCH_HOOKS += DELUGE_INSTALL_KERNEL_FILES

define DELUGE_INSTALL_UBOOT_BOARD
	$(DELUGE_SCRIPTS)/install-uboot-board.sh $(@D)
endef
UBOOT_PRE_PATCH_HOOKS += DELUGE_INSTALL_UBOOT_BOARD

include $(sort $(wildcard $(BR2_EXTERNAL_DELUGE_PATH)/package/*/*.mk))
