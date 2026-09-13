################################################################################
#
# Keystone driver
#
################################################################################

ifeq ($(KEYSTONE_DRIVER),)
$(error KEYSTONE_DRIVER directory not defined)
else
include $(KEYSTONE)/mkutils/pkg-keystone.mk
endif

KEYSTONE_DRIVER_DEPENDENCIES += host-keystone-sdk

$(eval $(keystone-package))
$(eval $(kernel-module))

# Build/install the interactive single-stepper alongside the kernel
# module -- a plain userspace tool, not a kernel object, so it rides on
# kernel-module's documented "post-build/post-install hooks are free for
# a userspace component" mechanism rather than the modules_install path.
define KEYSTONE_DRIVER_BUILD_STEP_TOOL
	$(TARGET_CC) -o $(@D)/tools/step $(@D)/tools/step.c
endef
KEYSTONE_DRIVER_POST_BUILD_HOOKS += KEYSTONE_DRIVER_BUILD_STEP_TOOL

define KEYSTONE_DRIVER_INSTALL_STEP_TOOL
	$(INSTALL) -D -m 0755 $(@D)/tools/step $(TARGET_DIR)/usr/bin/keystone-step
endef
KEYSTONE_DRIVER_POST_INSTALL_TARGET_HOOKS += KEYSTONE_DRIVER_INSTALL_STEP_TOOL

$(eval $(generic-package))
