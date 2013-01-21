Memory page replication for Linux on X86 processors
===================================================

This is a patched version of Linux, that supports automatic replication of memory pages.
This mechanism has been used in "Traffic Management: A Holistic Approach to Memory Placement on NUMA Systems", a paper published at ASPLOS in 2013 (http://asplos13.rice.edu/programme/).

The configuration file used for our experiments is 'config-bench'.

There are a few options that can be tuned in file "include/linux/replicate-options.h". Of course, you need to reinstall the kernel after changing an option. Default options are those we used for the ASPLOS paper.

EXAMPLES
--------

If you want to see how replication can be used with madvise, take a look at our stresstests in folder tools/replication. Note that we slightly changed the behavior of madvise when replicating pages. It is performed asynchronously.

If you want to use it with Carrefour, you will have to take a look at two others projects:
carrefour-module: https://github.com/Carrefour/carrefour-module
carrefour-runtime: https://github.com/Carrefour/carrefour-runtime


IMPORTANT NOTES
---------------

This patch has only been tested on 2/4 nodes AMD NUMA architectures. Nevertheless, we are confident that it should work on others X86 architectures.


KNOWN BUGS
----------

Vmalloc does not work properly as soon as a pgd has been replicated.