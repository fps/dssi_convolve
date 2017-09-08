PREFIX = /usr/local

# add channel counts to your hearts desire, but make sure the resulting
# LADSPA id is free (see Makefile.plugin)

.PHONY: all
all:
	for n in 1 2 4 6; do echo; CHANNELS=$$n make -f Makefile.plugin || exit 42; done
	echo; make PREFIX=$(PREFIX) -f Makefile.ui

.PHONY: install
install:
	for n in 1 2 4 6; do echo; PREFIX=$(PREFIX) CHANNELS=$$n make -f Makefile.plugin install; done
	echo; make PREFIX=$(PREFIX) -f Makefile.ui install
	echo; make PREFIX=$(PREFIX) -f Makefile.ui install_links

.PHONY: clean 
clean:
	for n in 1 2 4 6; do echo; PREFIX=$(PREFIX) CHANNELS=$$n make -f Makefile.plugin clean; done
	echo; make -f Makefile.ui clean; rm test_lo *.bak|| true

