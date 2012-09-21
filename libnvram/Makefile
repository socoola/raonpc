libnvram.so: nvram.c nvram.h
	gcc -c nvram.c
	gcc -shared nvram.o -o libnvram.so

install: libnvram.so
	cp ./libnvram.so /usr/lib/
	mkdir -p /etc/nvram/
