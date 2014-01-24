cluster-md-y  += md.o bitmap.o
cluster-raid1-y  += raid1.o
obj-m += cluster-md.o
obj-m += cluster-raid1.o

all:
	make -C /lib/modules/3.11.6-4-desktop/build M=$(PWD) modules
package:
	git archive --prefix=cluster-md/ --format=tar HEAD | gzip > cluster-md.tar.gz

clean:
	make -C /lib/modules/3.11.6-4-desktop/build M=$(PWD) clean
