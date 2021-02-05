XEN_ROOT=$(CURDIR)/../..
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS += -Werror
CFLAGS += $(CFLAGS_libxenstore) $(CFLAGS_libxenvchan) $(CFLAGS_libxengnttab) $(CFLAGS_libxenevtchn)

LDLIBS += $(LDLIBS_libxenstore) $(LDLIBS_libxenvchan)

.PHONY: all
all: xenpcid

.PHONY: install
install: all
	$(INSTALL_DIR) $(DESTDIR)$(sbindir)
	$(INSTALL_PROG) xenpcid $(DESTDIR)$(sbindir)

.PHONY: clean
clean:
	$(RM) -f xenpcid xenpcid.o $(DEPS_RM)

.PHONY: distclean
distclean: clean

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(sbindir)/xenpcid

xenpcid: xenpcid.o Makefile
	$(CC) $(LDFLAGS) $< -o $@ $(LDLIBS) $(APPEND_LDFLAGS)

-include $(DEPS_INCLUDE)
