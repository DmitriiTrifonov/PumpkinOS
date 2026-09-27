# Test helper, built inside the docker/pmos container (needs wlr-protocols).
PROTOCOL=/usr/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml

vclick: vclick.c wlr-virtual-pointer-unstable-v1-protocol.c wlr-virtual-pointer-unstable-v1-client-protocol.h
	$(CC) -Wall -o vclick vclick.c wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client

wlr-virtual-pointer-unstable-v1-client-protocol.h:
	wayland-scanner client-header $(PROTOCOL) $@

wlr-virtual-pointer-unstable-v1-protocol.c:
	wayland-scanner private-code $(PROTOCOL) $@

clean:
	rm -f vclick wlr-virtual-pointer-unstable-v1-*
