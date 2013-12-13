obj-m += pinthreads.o
carrefour-y := pinthreads.o

KV = $(shell uname -r)
KDIR := /lib/modules/$(KV)/build
PWD := $(shell pwd)

.PHONY: tags 

default: tags
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	rm -f modules.order

clean_tags:
	rm cscope.*
	rm tags

mrproper: clean clean_tags

tags:
	ctags --totals `find . -name '*.[ch]'`
	cscope -b -q -k -R -s.
