ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

SRCDIR = src
BUSYBOX_URL ?= https://busybox.net/downloads/busybox-1.32.0.tar.bz2
BUSYBOX_SHA256 ?= c35d87f1d04b2b153d33c275c2632e40d388a88f19a9e71727e0bbbff51fe689

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif


.PHONY: all
all: busybox busybox.manifest
ifeq ($(SGX),1)
all: busybox.manifest.sgx busybox.sig busybox.token
endif

$(SRCDIR)/Makefile:
	../common_tools/download --output busybox.tar.bz2 \
		--sha256 $(BUSYBOX_SHA256) --url $(BUSYBOX_URL)
	mkdir $(SRCDIR)
	tar -C $(SRCDIR) --strip-components=1 -xf busybox.tar.bz2

$(SRCDIR)/.config: $(SRCDIR)/Makefile
	$(MAKE) -C $(SRCDIR) clean
	$(MAKE) -C $(SRCDIR) defconfig
	# Enable usage of Busybox's built-in applets
	sed -e 's/.*CONFIG_FEATURE_SH_STANDALONE.*/CONFIG_FEATURE_SH_STANDALONE=y/' \
		-i $(SRCDIR)/.config
	# Currently '/proc/self/exe' is bugged in gramine, so manually set path
	# to the Busybox binary
	sed -e 's/.*CONFIG_BUSYBOX_EXEC_PATH.*/CONFIG_BUSYBOX_EXEC_PATH="\/busybox"/' \
		-i $(SRCDIR)/.config

$(SRCDIR)/busybox: $(SRCDIR)/.config
	$(MAKE) -C $(SRCDIR)

busybox.manifest: busybox.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< > $@

busybox.manifest.sgx: busybox.manifest busybox
	gramine-sgx-sign \
		--manifest $< \
		--output $@
	gramine-sgx-get-token \
		--output busybox.token --sig busybox.sig

# Copy Busybox binary to our root directory for simplicity.
busybox: $(SRCDIR)/busybox
	cp $(SRCDIR)/busybox busybox

.PHONY: clean
clean:
	$(RM) *.token *.sig *.manifest.sgx *.manifest busybox

.PHONY: distclean
distclean: clean
	$(RM) -r $(SRCDIR) busybox.tar.bz2
