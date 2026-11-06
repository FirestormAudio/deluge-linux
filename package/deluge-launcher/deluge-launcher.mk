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
# way pkg-cargo does, minus --offline: cargo fetches the pinned revisions
# (into $(BR_CARGO_HOME), under the download dir), and --locked holds it to
# Cargo.lock. So this package needs network access at build time, `make
# source` doesn't prefetch its crates, and legal-info doesn't include the
# deluge-sdk sources (public at the revision pinned in launcher/Cargo.toml).
#
# --ignore-rust-version: deluge-sdk's grid toolkit pulls in its bare-metal
# rza1-* crates, which declare rust-version 1.97; Buildroot ships 1.96.1,
# which builds the launcher fine.
define DELUGE_LAUNCHER_BUILD_CMDS
	cd $(@D) && \
	$(TARGET_MAKE_ENV) \
		$(TARGET_CONFIGURE_OPTS) \
		$(PKG_CARGO_ENV) \
		$(DELUGE_LAUNCHER_CARGO_ENV) \
		cargo build \
			$(if $(BR2_ENABLE_DEBUG),,--release) \
			--manifest-path Cargo.toml \
			--locked \
			--ignore-rust-version \
			$(DELUGE_LAUNCHER_CARGO_BUILD_OPTS)
endef

define DELUGE_LAUNCHER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/target/$(RUSTC_TARGET_NAME)/$(if $(BR2_ENABLE_DEBUG),debug,release)/launcher \
		$(TARGET_DIR)/usr/bin/deluge-app
endef

$(eval $(cargo-package))
