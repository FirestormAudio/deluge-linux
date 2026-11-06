# deluge-linux — Buildroot BR2_EXTERNAL tree for the Synthstrom Deluge.
#
# Buildroot is pinned and fetched into .buildroot/ (gitignored). It owns the
# kernel, U-Boot, the toolchain and the rootfs; this tree supplies the board.
#
# No host cross-toolchain is required: Buildroot builds its own.

BUILDROOT_URL     := https://gitlab.com/buildroot.org/buildroot.git
BUILDROOT_VERSION := afc0ca1723a7e0e862ccccd9a8a5c7d419b6fd44

TOP := $(CURDIR)
BR  := $(TOP)/.buildroot
OUT := $(TOP)/out
MAKEFLAGS += --no-print-directory

# In-tree build: Buildroot ignores an O= that points inside its own source tree,
# so don't pass one. .config lives at $(BR)/.config and images at $(BR)/output/.
BRMAKE = $(MAKE) -C $(BR) BR2_EXTERNAL=$(TOP)

.PHONY: help base platform appelf load clean distclean buildroot \
        linux-menuconfig linux-update-defconfig sdk bundle

help:
	@echo 'deluge-linux — targets:'
	@echo '  make base                      build the appliance profile (what apps ship on)'
	@echo '  make platform                  build the hackable profile (getty, ssh, ncm)'
	@echo '  make appelf                    pack out/ into LINUX.ELF for the app-loader'
	@echo '  make load                      load U-Boot into SDRAM over JTAG (J-Link)'
	@echo '  make linux-menuconfig          configure the kernel'
	@echo '  make linux-update-defconfig    snapshot the kernel config back into board/'
	@echo '  make sdk                       build the relocatable toolchain tarball'
	@echo '  make bundle                    assemble the base bundle the SDK consumes'
	@echo '  make clean | distclean'

# --- pinned Buildroot ------------------------------------------------------
$(BR)/Makefile:
	@echo "=== Fetching Buildroot $(BUILDROOT_VERSION) ==="
	git clone $(BUILDROOT_URL) $(BR)
	git -C $(BR) checkout --detach $(BUILDROOT_VERSION)

buildroot: $(BR)/Makefile
	@git -C $(BR) rev-parse HEAD | grep -q '^$(BUILDROOT_VERSION)' || { \
	  echo "=== Re-pinning Buildroot to $(BUILDROOT_VERSION) ==="; \
	  git -C $(BR) fetch origin && git -C $(BR) checkout --detach $(BUILDROOT_VERSION); }

# --- profiles --------------------------------------------------------------
# Load the base defconfig, merge the profile fragment, resolve, build.
define profile_build
	$(BRMAKE) deluge_defconfig
	$(BR)/support/kconfig/merge_config.sh -m -O $(BR) \
	    $(BR)/.config $(TOP)/configs/$(1).fragment
	$(BRMAKE) olddefconfig
	# The two profiles carry different kernel configs (platform adds the
	# networking fragment). Buildroot does not invalidate the kernel stamp on
	# a config/fragment change, so force a reconfigure+rebuild to guarantee the
	# kernel matches THIS profile before the image is assembled.
	$(BRMAKE) linux-reconfigure
	$(BRMAKE)
	mkdir -p $(OUT)
	cp $(BR)/output/images/zImage $(BR)/output/images/r7s72102-deluge.dtb $(OUT)/
	cp $(BR)/output/images/u-boot.bin $(OUT)/
	cp $(BR)/output/images/u-boot $(OUT)/u-boot
	cp $(BR)/output/images/rootfs.cpio $(OUT)/
endef

base: buildroot
	$(call profile_build,base)

platform: buildroot
	$(call profile_build,platform)

# --- kernel config workflow -----------------------------------------------
# NEVER edit the kernel tree under .buildroot/ — it is throwaway. Configure
# here, then snapshot back into board/synthstrom/deluge/kernel/deluge.config.
linux-menuconfig: buildroot
	$(BRMAKE) linux-menuconfig

linux-update-defconfig: buildroot
	$(BRMAKE) linux-update-defconfig
	@echo "Snapshotted -> board/synthstrom/deluge/kernel/deluge.config"

sdk: buildroot
	$(BRMAKE) sdk

# --- app-loader ELF --------------------------------------------------------
# Pack u-boot.bin + DTB + zImage + the rootfs into the single ELF the Deluge
# app-loader launches from /APPS. Load addresses come from
# board/synthstrom/deluge/memory-map.json — the packer reads it, so they are
# not repeated here. U-Boot's compiled-in bootcmd must agree with that map.
MAP := $(TOP)/board/synthstrom/deluge/memory-map.json

appelf: $(OUT)/LINUX.ELF

$(OUT)/LINUX.ELF: $(OUT)/u-boot.bin $(OUT)/r7s72102-deluge.dtb $(OUT)/zImage \
                  $(OUT)/rootfs.cpio $(MAP)
	python3 $(TOP)/tools/mk-app-elf.py --map $(MAP) --dir $(OUT) \
	    --rootfs $(OUT)/rootfs.cpio $@
	python3 $(TOP)/tools/check-app-elf.py --map $(MAP) $@
	@echo "  Output: $@  (copy to the card as /APPS/LINUX.ELF)"

# --- release bundle --------------------------------------------------------
# The artifact the SDK consumes. Builds both profiles + the toolchain, so it is
# slow — this runs once per platform release, not per app build. Version
# defaults to `git describe`; override with `make bundle VERSION=0.1.0`.
VERSION ?= $(shell git -C $(TOP) describe --tags --always --dirty)

bundle: buildroot
	$(TOP)/scripts/mk-bundle.sh $(VERSION)

# --- JTAG ------------------------------------------------------------------
load: $(OUT)/u-boot.bin
	cp $(OUT)/u-boot.bin $(TOP)/debug/jlink/uboot.bin
	cd $(TOP)/debug/jlink && JLinkExe -device R7S721020 -if JTAG -speed 4000 \
	    -CommandFile load_uboot_warm.jlink

clean:
	rm -rf $(BR)/output $(OUT)

distclean: clean
	rm -rf $(BR)
