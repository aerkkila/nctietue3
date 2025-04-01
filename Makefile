include config.mk

libraries = -lnetcdf

ifdef have_proj
	macros += -DHAVE_PROJ -D_GNU_SOURCE
	libraries += -lproj -lm
	extra_headers += extra/nctproj.h
	extra_dependencies += extra/nctproj.[ch]
endif

ifdef have_lz4
	macros += -DHAVE_LZ4
	libraries += -llz4
	extra_dependencies += extra/lz4.[ch]
endif

all: libnctietue3.so

nctietue3.o: nctietue3.c nctietue3.h internals.h load_data.h transpose.c config.mk $(extra_dependencies) Makefile
	$(CC) $(CFLAGS) $(macros) -o $@ -c $<

functions.o: functions.c config.mk nctietue3.h
	$(CC) $(CFLAGS) -O3 -o $@ -c $<

libnctietue3.so: nctietue3.o functions.o
	$(CC) -o $@ -shared $^ $(libraries)

functions.c: make_functions.pl functions.in.c
	./$<

clean:
	rm -rf *.o *.so functions.c

install: libnctietue3.so
	mkdir -p $(includedir) $(libdir)
	cp nctietue3.h $(extra_headers) $(includedir)
	cp libnctietue3.so $(libdir)

uninstall:
	rm -f $(includedir)/nctietue3.h $(includedir)/nctproj.h $(libdir)/libnctietue3.so
