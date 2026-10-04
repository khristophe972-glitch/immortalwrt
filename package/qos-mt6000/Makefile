# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=qos-mt6000
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=khristophe972-glitch

include $(INCLUDE_DIR)/package.mk

define Package/qos-mt6000
  SECTION:=net
  CATEGORY:=Network
  DEPENDS:=+tc +kmod-sched-cake
  TITLE:=Root cake_mq qdisc manager for the GL-MT6000 WAN port
  PKGARCH:=all
endef

define Package/qos-mt6000/description
  Applies the root cake_mq qdisc of the WAN port from /etc/config/qos-mt6000
  (validated values, tc qdisc replace, cake_mq debugfs tuning) and reapplies
  it when the WAN interface comes up.
endef

define Package/qos-mt6000/conffiles
/etc/config/qos-mt6000
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/qos-mt6000/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/qos-mt6000 $(1)/etc/config/qos-mt6000
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/qos-mt6000 $(1)/etc/init.d/qos-mt6000
	$(INSTALL_DIR) $(1)/usr/lib/qos-mt6000
	$(INSTALL_BIN) ./files/usr/lib/qos-mt6000/qos-mt6000.sh $(1)/usr/lib/qos-mt6000/qos-mt6000.sh
endef

$(eval $(call BuildPackage,qos-mt6000))
