.PHONY: all procview testforge autotest-assist-tui clean

all: procview testforge autotest-assist-tui

procview:
	$(MAKE) -C procview

testforge:
	$(MAKE) -C testforge

autotest-assist-tui:
	$(MAKE) -C autotest-assist-tui

clean:
	$(MAKE) -C procview clean
	$(MAKE) -C testforge clean
	$(MAKE) -C autotest-assist-tui clean
