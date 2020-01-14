# KallistiOS ##version##
#
# arch/dreamcast Makefile
# (c)2001 Dan Potter
#
	
SUBDIRS=fs hardware kernel math sound util

ifeq ($(KOS_SUBARCH), navi)
	SUBDIRS += navi
endif

include $(KOS_BASE)/Makefile.rules

all: subdirs
clean: clean_subdirs
