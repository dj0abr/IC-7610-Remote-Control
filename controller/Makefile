CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic

ic7610-status: ic7610-status.cpp icom-lan.cpp icom-lan.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 $(LDFLAGS) -o $@ $(filter %.cpp,$^) $(LDLIBS)
