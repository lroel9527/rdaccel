# The same two programs with pkg-config and make, no CMake:
#
#   PKG_CONFIG_PATH=/some/prefix/lib/pkgconfig make            # shared library
#   PKG_CONFIG_PATH=/some/prefix/lib/pkgconfig make STATIC=1   # static library
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
ALL_CFLAGS = $(CFLAGS) -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread
ifdef STATIC
RDA_LIBS = $(shell $(PKG_CONFIG) --static --libs rdaccel)
else
RDA_LIBS = $(shell $(PKG_CONFIG) --libs rdaccel)
endif
RDA_CFLAGS = $(shell $(PKG_CONFIG) --cflags rdaccel)
# Lets a shared library outside the default search path be found at run time.
RDA_RPATH = $(if $(STATIC),,-Wl,-rpath,$(shell $(PKG_CONFIG) --variable=libdir rdaccel))

all: support_host support_viewer

support_%: support_%.c common.h
	$(CC) $(ALL_CFLAGS) $(RDA_CFLAGS) -o $@ $< $(RDA_LIBS) $(RDA_RPATH) $(LDFLAGS) -pthread

clean:
	rm -f support_host support_viewer

.PHONY: all clean
