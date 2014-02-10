cluster-md-y  += md.o bitmap.o
cluster-raid1-y  += raid1.o
obj-m += cluster-md.o
obj-m += cluster-raid1.o

KERNSRC=/mnt/devel/linux-3.12-SLE12.mod

all:
	make -C $(KERNSRC) M=$(PWD) modules
package:
	git archive --prefix=cluster-md/ --format=tar HEAD | gzip > cluster-md.tar.gz

clean:
	make -C $(KERNSRC) M=$(PWD) clean
