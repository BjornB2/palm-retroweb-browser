MODEM_APP := RBrowser
WEB_APP := RetroWEBBrowser
MODEM_CREATOR := RwBr
WEB_CREATOR := RtWB
MODEM_DB_NAME := RBrowser 0.1.35
WEB_DB_NAME := RetroWEB 0.1.35
MODEM_DB_VERSION := 45
WEB_DB_VERSION := 45
BUILD := build
MODEM_BUILD := $(BUILD)/modem
WEB_BUILD := $(BUILD)/retroweb
DIST := dist
SDK := /opt/palmdev/sdk-4

CC := m68k-palmos-gcc
# PilRC 3.2 stores BMP dimensions in C long. Its stock 64-bit Linux build
# consequently misreads every icon bitmap header. This project-local build
# uses fixed 32-bit BMP fields and is run inside the amd64 build container.
PILRC := $(CURDIR)/tools/pilrc-3.2-fixed
OBJRES := m68k-palmos-obj-res
BUILDPRC := build-prc

COMMON_CFLAGS := -Os -Wall -Wno-unused -palmos4
COMMON_LDFLAGS := -lNetSocket -palmos4 -L$(SDK)/lib/m68k-palmos-coff
SOURCES := src/main.c src/gif.c src/document.c
HEADERS := src/gif.h src/document.h src/resources.h
RESOURCE := resources/RetrowebBrowser.rcp
BROWSER_ASSETS := assets/icon-1-32.bmp assets/icon-8.bmp

.PHONY: all modem retroweb clean docker-build

all: modem retroweb

modem: $(DIST)/$(MODEM_APP).prc

retroweb: $(DIST)/$(WEB_APP).prc

$(MODEM_BUILD) $(WEB_BUILD) $(DIST):
	mkdir -p $@

$(MODEM_BUILD)/palmos.specs: | $(MODEM_BUILD)
	palmdev-prep -q --dump-specs unused > $@

$(WEB_BUILD)/palmos.specs: | $(WEB_BUILD)
	palmdev-prep -q --dump-specs unused > $@

$(MODEM_BUILD)/$(MODEM_APP): $(SOURCES) $(HEADERS) $(MODEM_BUILD)/palmos.specs
	$(CC) $(COMMON_CFLAGS) -specs=$(CURDIR)/$(MODEM_BUILD)/palmos.specs \
		$(SOURCES) -o $@ $(COMMON_LDFLAGS) \
		-specs=$(CURDIR)/$(MODEM_BUILD)/palmos.specs

$(WEB_BUILD)/$(WEB_APP): $(SOURCES) $(HEADERS) $(WEB_BUILD)/palmos.specs
	$(CC) $(COMMON_CFLAGS) -DRETROWEB_BUILD \
		-specs=$(CURDIR)/$(WEB_BUILD)/palmos.specs \
		$(SOURCES) -o $@ $(COMMON_LDFLAGS) \
		-specs=$(CURDIR)/$(WEB_BUILD)/palmos.specs

$(DIST)/$(MODEM_APP).prc: $(MODEM_BUILD)/$(MODEM_APP) $(RESOURCE) $(BROWSER_ASSETS) | $(DIST)
	cd $(MODEM_BUILD) && $(OBJRES) $(MODEM_APP)
	cd $(MODEM_BUILD) && $(PILRC) -I ../../src ../../$(RESOURCE)
	cd $(MODEM_BUILD) && $(BUILDPRC) --ok-to-install-newer -v $(MODEM_DB_VERSION) \
		../../$@ "$(MODEM_DB_NAME)" $(MODEM_CREATOR) *.grc *.bin

$(DIST)/$(WEB_APP).prc: $(WEB_BUILD)/$(WEB_APP) $(RESOURCE) $(BROWSER_ASSETS) | $(DIST)
	cd $(WEB_BUILD) && $(OBJRES) $(WEB_APP)
	cd $(WEB_BUILD) && $(PILRC) -I ../../src -D RETROWEB_BUILD ../../$(RESOURCE)
	cd $(WEB_BUILD) && $(BUILDPRC) --ok-to-install-newer -v $(WEB_DB_VERSION) \
		../../$@ "$(WEB_DB_NAME)" $(WEB_CREATOR) *.grc *.bin

docker-build:
	docker run --rm --platform linux/amd64 \
		-v "$(CURDIR):/work" -w /work \
		tomari/prc-tools-remix:latest make clean all

clean:
	rm -rf $(BUILD) $(DIST)
