################################################################################
#
# deluge-launcher
#
# The platform profile's app slot: the on-device launcher for /LINUX/APPS/.
# In-tree Rust source (launcher/); its deluge-sdk dependencies are git
# dependencies pinned in launcher/Cargo.toml and locked by Cargo.lock.
#
################################################################################

DELUGE_LAUNCHER_SITE = $(BR2_EXTERNAL_DELUGE_PATH)/package/deluge-launcher/launcher
DELUGE_LAUNCHER_SITE_METHOD = local
DELUGE_LAUNCHER_LICENSE = GPL-3.0-or-later
DELUGE_LAUNCHER_LICENSE_FILES = LICENSE
DELUGE_LAUNCHER_DEPENDENCIES = libdeluge alsa-lib

# libdeluge-sys links the static libdeluge.a from the staging sysroot.
DELUGE_LAUNCHER_CARGO_ENV = DELUGE_SDK_ROOT=$(STAGING_DIR)/usr

# Buildroot vendors crates only for downloaded sources, so a local package
# would build --offline with its git dependencies missing. Build the same
# way pkg-cargo does, minus --offline: cargo fetches the pinned revisions,
# and --locked holds it to Cargo.lock.
define DELUGE_LAUNCHER_BUILD_CMDS
	cd $(@D) && \
	$(TARGET_MAKE_ENV) \
		$(TARGET_CONFIGURE_OPTS) \
		$(PKG_CARGO_ENV) \
		$(DELUGE_LAUNCHER_CARGO_ENV) \
		cargo build --release --manifest-path Cargo.toml --locked --ignore-rust-version
endef

define DELUGE_LAUNCHER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/target/$(RUSTC_TARGET_NAME)/release/launcher \
		$(TARGET_DIR)/usr/bin/deluge-app
endef

$(eval $(cargo-package))
