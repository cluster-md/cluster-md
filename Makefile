cluster-md-y  += md.o bitmap.o
cluster-raid1-y  += raid1.o
obj-m += cluster-md.o
obj-m += cluster-raid1.o

KBUILD?=/lib/modules/`uname -r`/build

all:
	make -C $(KBUILD) M=$(PWD) modules
package:
	git archive --prefix=cluster-md/ --format=tar HEAD | gzip > cluster-md.tar.gz

clean:
	make -C $(KBUILD) M=$(PWD) clean
