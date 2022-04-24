# Default optimization level
O ?= 2

all: msh

-include build/rules.mk

%.o: %.cc sh61.hh $(BUILDSTAMP)
	$(call run,$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPCFLAGS) $(O) -DSH61_TESTS -o $@ -c,COMPILE,$<)

msh: msh.o helpers.o
	$(call run,$(CXX) $(CXXFLAGS) $(O) -o $@ $^ $(LDFLAGS) $(LIBS),LINK $@)

sleep61: sleep61.cc
	$(call run,$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPCFLAGS) $(O) -o $@ $^ $(LDFLAGS) $(LIBS),BUILD $@)

ifneq ($(filter -fsanitize=leak,$(CXXFLAGS)),)
LEAKCHECK = --leak
endif

check: msh
	perl check.pl $(LEAKCHECK)

check-%: msh
	perl check.pl $(LEAKCHECK) $(subst check-,,$@)

clean: clean-main
clean-main:
	$(call run,rm -f msh *.o *~ *.bak core *.core,CLEAN)
	$(call run,rm -rf out *.dSYM $(DEPSDIR))

.PRECIOUS: %.o
.PHONY: all clean clean-main distclean check check-%
