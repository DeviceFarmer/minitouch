.PHONY: default clean prebuilt test

HOST_CC ?= cc

NDKBUILT := \
  libs/arm64-v8a/minitouch \
  libs/arm64-v8a/minitouch-nopie \
  libs/armeabi-v7a/minitouch \
  libs/armeabi-v7a/minitouch-nopie \
  libs/x86/minitouch \
  libs/x86/minitouch-nopie \
  libs/x86_64/minitouch \
  libs/x86_64/minitouch-nopie \

default: prebuilt

clean:
	ndk-build clean
	rm -rf prebuilt
	rm -f test/touch_test

$(NDKBUILT):
	ndk-build

# It may feel a bit redundant to list everything here. However it also
# acts as a safeguard to make sure that we really are including everything
# that is supposed to be there.
prebuilt: \
  prebuilt/arm64-v8a/bin/minitouch \
  prebuilt/arm64-v8a/bin/minitouch-nopie \
  prebuilt/armeabi-v7a/bin/minitouch \
  prebuilt/armeabi-v7a/bin/minitouch-nopie \
  prebuilt/x86/bin/minitouch \
  prebuilt/x86/bin/minitouch-nopie \
  prebuilt/x86_64/bin/minitouch \
  prebuilt/x86_64/bin/minitouch-nopie \

prebuilt/%/bin/minitouch: libs/%/minitouch
	mkdir -p $(@D)
	cp $^ $@

prebuilt/%/bin/minitouch-nopie: libs/%/minitouch-nopie
	mkdir -p $(@D)
	cp $^ $@

# Checks the contact state machine on the host, no device or NDK needed.
test: test/touch_test
	./test/touch_test

test/touch_test: test/touch_test.c test/include/linux/types.h jni/minitouch/touch.c jni/minitouch/touch.h
	$(HOST_CC) -Wall -Wextra -Werror -Itest/include -Ijni/vendor/libevdev/source/include -Ijni/minitouch -o $@ test/touch_test.c jni/minitouch/touch.c
