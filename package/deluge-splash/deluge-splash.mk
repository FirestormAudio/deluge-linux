################################################################################
#
# deluge-splash
#
################################################################################

# In-tree source: the splash lives in this package and is built from local
# source (no fetch). Change nothing here to bump it — edit splash/ directly.
DELUGE_SPLASH_SITE = $(BR2_EXTERNAL_DELUGE_PATH)/package/deluge-splash/splash
DELUGE_SPLASH_SITE_METHOD = local
DELUGE_SPLASH_LICENSE = GPL-2.0

# Displayed name, chosen at build time (BR2_PACKAGE_DELUGE_SPLASH_NAME). qstrip
# drops the surrounding quotes from the Buildroot string value. NB: do not call
# this DELUGE_SPLASH_NAME — that variable is reserved by Buildroot's package
# infra (it holds the raw package name "deluge-splash") and would clobber this.
DELUGE_SPLASH_WORDMARK = $(call qstrip,$(BR2_PACKAGE_DELUGE_SPLASH_NAME))

# Static: /usr/bin/splash must not depend on the rootfs it lands in.
define DELUGE_SPLASH_BUILD_CMDS
	$(MAKE) -C $(@D) arm CROSS="$(TARGET_CROSS)" NAME='$(DELUGE_SPLASH_WORDMARK)'
endef

define DELUGE_SPLASH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/overlay/usr/bin/splash \
		$(TARGET_DIR)/usr/bin/splash
endef

$(eval $(generic-package))
