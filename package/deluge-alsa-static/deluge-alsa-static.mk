################################################################################
#
# deluge-alsa-static
#
# A static-only build of alsa-lib, installed into the STAGING sysroot ONLY, so
# the SDK can statically link libasound into apps (they run from the SD card and
# must be fully static). Buildroot's alsa-lib builds shared OR static, not both
# (configure.ac: "do not try to compile static and shared libraries together"),
# and every rootfs needs the shared libasound.so — so this second, static build
# supplies libasound.a alongside it. It reuses alsa-lib's exact source; the
# headers, alsa.pc and /usr/share/alsa all come from the shared alsa-lib package.
#
################################################################################

# Track the shared alsa-lib package exactly — same tarball, no version drift.
DELUGE_ALSA_STATIC_VERSION = $(ALSA_LIB_VERSION)
DELUGE_ALSA_STATIC_SITE = $(ALSA_LIB_SITE)
DELUGE_ALSA_STATIC_SOURCE = $(ALSA_LIB_SOURCE)
DELUGE_ALSA_STATIC_LICENSE = LGPL-2.1+
DELUGE_ALSA_STATIC_LICENSE_FILES = COPYING
DELUGE_ALSA_STATIC_AUTORECONF = YES
DELUGE_ALSA_STATIC_DEPENDENCIES = alsa-lib

# Static-only, but otherwise matching the shared alsa-lib's interface: mixer,
# hwdep, seq, ucm and topology are default-y in Buildroot's alsa-lib and stay
# enabled here too, so libasound.a is a full match for the libasound.so the SDK
# headers describe (a future app linking snd_mixer_*/snd_seq_* resolves against
# either). Only aload and python are dropped, matching the shared build.
# --without-libdl is required for a static build; --without-versioned matches
# the musl (non-glibc) alsa-lib build.
DELUGE_ALSA_STATIC_CONF_OPTS = \
	--enable-static=yes --enable-shared=no --without-libdl \
	--without-versioned \
	--with-pcm-plugins="all" --with-ctl-plugins="all" \
	--disable-aload --disable-python

# Staging only: install just the archive. Nothing lands in the target rootfs.
# (autotools-package defaults INSTALL_STAGING to NO, so enable it explicitly.)
DELUGE_ALSA_STATIC_INSTALL_STAGING = YES
DELUGE_ALSA_STATIC_INSTALL_TARGET = NO
define DELUGE_ALSA_STATIC_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/src/.libs/libasound.a \
		$(STAGING_DIR)/usr/lib/libasound.a
endef

$(eval $(autotools-package))
