include $(TOPDIR)/rules.mk

PKG_NAME:=qdma-autorate
PKG_VERSION:=5.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_BUILD_DEPENDS:=libbpf elfutils zlib

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/bpf.mk

define Package/qdma-autorate
  SECTION:=net
  CATEGORY:=Network
  TITLE:=ePPing-driven QDMA hardware download shaper (static)
  DEPENDS:=$(BPF_DEPENDS) +kmod-sched-cake
endef

define Package/qdma-autorate/description
  Closed-loop download shaper for the GL-MT6000: ePPing per-flow RTT (tcx on the WAN) drives
  the QDMA hardware scheduler cap of kmod-qdma-rl (install that package too). Turns SQM's
  download shaping off while running and restores it on stop.
endef

define Package/qdma-autorate/conffiles
/etc/config/qdma-autorate
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

# Same BPF build as mindwolf80/epping-owrt: no CO-RE, no __bpf_trap typed ksym (no kernel BTF on stock OpenWrt)
define Build/Compile
	$(CLANG) -target $(BPF_TARGET) -O2 -g -Wall -DPPING_NO_CORE \
		-Wno-compare-distinct-pointer-types \
		-I$(PKG_BUILD_DIR)/include -I$(PKG_BUILD_DIR) \
		-I$(STAGING_DIR)/usr/include -I$(TOOLCHAIN_DIR)/include \
		-mllvm -bpf-disable-trap-unreachable \
		-c $(PKG_BUILD_DIR)/pping_kern.c -o $(PKG_BUILD_DIR)/pping_kern.o
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall -I$(PKG_BUILD_DIR) \
		-static -o $(PKG_BUILD_DIR)/qdma-autorate \
		$(PKG_BUILD_DIR)/autorate.c $(PKG_BUILD_DIR)/ctl.c \
		$(TARGET_LDFLAGS) \
		$(STAGING_DIR)/usr/lib/libbpf.a $(STAGING_DIR)/usr/lib/libelf.a $(STAGING_DIR)/usr/lib/libz.a -lm
	$(TARGET_CROSS)strip $(PKG_BUILD_DIR)/qdma-autorate
	$(INSTALL_DIR) $(TOPDIR)/bin/qdma-autorate
	$(CP) $(PKG_BUILD_DIR)/qdma-autorate $(PKG_BUILD_DIR)/pping_kern.o $(TOPDIR)/bin/qdma-autorate/
endef

define Package/qdma-autorate/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/lib/qdma-autorate $(1)/etc/init.d $(1)/etc/config \
		$(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/qdma-autorate $(1)/usr/sbin/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/pping_kern.o $(1)/usr/lib/qdma-autorate/
	$(INSTALL_BIN) ./files/qdma-autorate.init $(1)/etc/init.d/qdma-autorate
	$(INSTALL_CONF) ./files/qdma-autorate.config $(1)/etc/config/qdma-autorate
	$(INSTALL_DATA) ./files/qdma-autorate.hotplug $(1)/etc/hotplug.d/iface/99-qdma-autorate
endef

$(eval $(call BuildPackage,qdma-autorate))
