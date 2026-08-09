APP_ABI := all

# Disable PIE for SDK <16 support. Enable manually for >=5.0
# where necessary.
APP_PIE := false

# Align LOAD segments to 16 KB so that the binaries also load on Android
# 15+ devices with 16 KB page sizes. Harmless on 4 KB devices.
APP_LDFLAGS += -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
