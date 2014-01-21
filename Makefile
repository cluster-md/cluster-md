md-mod-y  += md.o bitmap.o
obj-m += md-mod.o
obj-m += raid1.o

all:
	make -C /lib/modules/3.11.6-4-desktop/build M=$(PWD) modules

clean:
	make -C /lib/modules/3.11.6-4-desktop/build M=$(PWD) clean
