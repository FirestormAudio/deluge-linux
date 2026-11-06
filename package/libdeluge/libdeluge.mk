################################################################################
#
# libdeluge
#
# The Deluge userspace C library (the libdeluge repo): a static archive
# (libdeluge.a) + public headers + deluge.pc, installed into the STAGING sysroot
# ONLY. Apps statically link it and run from the SD card, so nothing lands in
# the target rootfs. It ships in the SDK sysroot the base bundle publishes
# (BR2_TARGET_SDK), which is how C/C++ and Rust apps build against it.
#
# Fetched by tag from GitHub. To build against a local libdeluge checkout
# instead, pass Buildroot's standard source override:
#   make base LIBDELUGE_OVERRIDE_SRCDIR=/path/to/libdeluge
#
################################################################################

LIBDELUGE_VERSION = v0.1.0
LIBDELUGE_SITE = https://github.com/FirestormAudio/libdeluge.git
LIBDELUGE_SITE_METHOD = git
LIBDELUGE_LICENSE = MIT OR Apache-2.0
LIBDELUGE_LICENSE_FILES = LICENSE-MIT LICENSE-APACHE

# Compiles against ALSA headers (pkg_check_modules(ALSA REQUIRED alsa)).
LIBDELUGE_DEPENDENCIES = alsa-lib

# Build only the library: no in-tree examples, no ctest suite.
LIBDELUGE_CONF_OPTS = -DLIBDELUGE_EXAMPLES=OFF -DLIBDELUGE_TESTS=OFF

# Staging only — app builds link libdeluge.a into apps; the rootfs never needs it.
LIBDELUGE_INSTALL_STAGING = YES
LIBDELUGE_INSTALL_TARGET = NO

$(eval $(cmake-package))
