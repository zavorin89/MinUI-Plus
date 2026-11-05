# workspace (all)

###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

###########################################################

.PHONY: all

# MinUI+ tools, A30 only for now (Wi-Fi and the device's OpenSSL)
ifeq ($(PLATFORM),my282)
EXTRA_TOOLS = rasync wifi boxart controls ndslayer
endif

all:
	cd ./$(PLATFORM)/libmsettings && make
	cd ./$(PLATFORM) && make early # eg. other libs
	cd ./$(PLATFORM)/keymon && make
	cd ./all/minui/ && make
	cd ./all/minarch/ && make
	cd ./all/clock/ && make
	cd ./all/minput/ && make
	cd ./all/syncsettings/ && make
	cd ./all/say/ && make
	for TOOL in $(EXTRA_TOOLS); do (cd ./all/$$TOOL && make) || exit 1; done
	cd ./$(PLATFORM)/cores && make
	cd ./$(PLATFORM) && make

clean:
	cd ./$(PLATFORM)/libmsettings && make clean
	cd ./$(PLATFORM)/keymon && make clean
	cd ./all/minui/ && make clean
	cd ./all/minarch/ && make clean
	cd ./all/clock/ && make clean
	cd ./all/minput/ && make clean
	cd ./all/syncsettings/ && make clean
	cd ./all/say/ && make clean
	for TOOL in $(EXTRA_TOOLS); do (cd ./all/$$TOOL && make clean); done
	cd ./$(PLATFORM) && make clean