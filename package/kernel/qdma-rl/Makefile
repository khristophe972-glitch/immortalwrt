include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qdma-rl
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qdma-rl
  SUBMENU:=Network Devices
  TITLE:=MT7986 QDMA per-queue max-rate test knob
  DEPENDS:=@TARGET_mediatek_filogic
  FILES:=$(PKG_BUILD_DIR)/qdma_rl.ko
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
	# raw module for the CI artifact
	$(INSTALL_DIR) $(TOPDIR)/bin/qdma-rl
	$(CP) $(PKG_BUILD_DIR)/qdma_rl.ko $(TOPDIR)/bin/qdma-rl/
endef

$(eval $(call KernelPackage,qdma-rl))
