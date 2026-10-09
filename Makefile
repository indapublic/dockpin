# `make` builds DockPin.app, `make test` runs the edge-logic tests.
CFLAGS ?= -O2 -Wall
FRAMEWORKS = -framework CoreGraphics -framework CoreFoundation

DockPin.app: dockpin.c Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp Info.plist $@/Contents/Info.plist
	$(CC) $(CFLAGS) -o $@/Contents/MacOS/DockPin dockpin.c $(FRAMEWORKS)
	codesign --force --sign - $@

tests/edge_test: tests/edge_test.c dockpin.c
	$(CC) $(CFLAGS) -o $@ tests/edge_test.c $(FRAMEWORKS)

test: tests/edge_test
	./tests/edge_test

clean:
	rm -rf DockPin.app tests/edge_test

.PHONY: test clean
