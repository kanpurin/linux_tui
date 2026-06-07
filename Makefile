.PHONY: all procview testforge clean

all: procview testforge

procview:
	$(MAKE) -C procview

testforge:
	$(MAKE) -C testforge

clean:
	$(MAKE) -C procview clean
	$(MAKE) -C testforge clean
