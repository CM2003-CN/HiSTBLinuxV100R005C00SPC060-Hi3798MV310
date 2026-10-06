#===============================================================================
# hi_sdio_detect module Makefile
#===============================================================================

MODULE_NAME := hi_sdio_detect

ccflags-y += -I$(src)

obj-m += $(MODULE_NAME).o
#$(MODULE_NAME)-objs := hi_sdio_detect.o

strip:
	$(CROSS_COMPILE)strip $(MODULE_NAME).ko --strip-unneeded

.PHONY: strip
