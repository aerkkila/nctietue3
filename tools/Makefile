CFLAGS = -Wall -g
LDLIBS = -lnctietue3

bindir = /usr/local/bin

all: nctinfo

install: nctinfo
	cp $^ $(bindir)

clean:
	rm nctinfo

uninstall:
	rm $(bindir)/nctinfo
