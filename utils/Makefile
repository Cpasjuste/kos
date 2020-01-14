# KallistiOS ##version##
#
# utils/Makefile
# (c)2001 Dan Potter
#

DIRS = genromfs wav2adpcm vqenc scramble dcbumpgen

# Ok for these to fail atm...

all:
	for i in $(DIRS); do $(KOS_MAKE) -C $$i; done

clean:
	for i in $(DIRS); do $(KOS_MAKE) -C $$i clean; done
		

