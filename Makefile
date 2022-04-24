CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra

.PHONY: clean all

myshell := msh

all: $(myshell)

helpers.o: helpers.cc sh61.hh
	$(CXX) $(CXXFLAGS) -g -c $<

msh.o: msh.cc sh61.hh
	$(CXX) $(CXXFLAGS) -g -c $<

msh: msh.o helpers.o
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(myshell) *.o
