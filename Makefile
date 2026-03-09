CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -march=native -D_GNU_SOURCE
LDFLAGS  = -lpthread
SRCDIR   = src
SOURCES  = $(SRCDIR)/main.cpp $(SRCDIR)/dns_encode.cpp $(SRCDIR)/sender.cpp $(SRCDIR)/receiver.cpp
TARGET   = dnsblast

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(wildcard $(SRCDIR)/*.h)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(TARGET)
