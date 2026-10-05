# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=foedecode
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=khristophe972-glitch

include $(INCLUDE_DIR)/package.mk

define Package/foedecode
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Decode MTK PPE FoE entries (ib1/ib2) from debugfs
  PKGARCH:=all
endef

define Package/foedecode/description
  Reads the MediaTek PPE flow-table debugfs (mtk_ppe/entries, mtk_ppe/bind or
  ppeN/…) and decodes the ib2 field into QID / DSCP / ECN / PSE_QOS / DEST_PORT
  for the entry's NETSYS version (v1 or v2). Read-only diagnostic tool.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/foedecode/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/usr/bin/foedecode $(1)/usr/bin/foedecode
endef

$(eval $(call BuildPackage,foedecode))
