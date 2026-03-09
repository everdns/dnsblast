CXX      := g++
CXXFLAGS := -std=c++17 -O3 -march=native -Wall -Wextra -pthread
LDFLAGS  := -pthread

TARGET   := dnsblast
SRCS     := main.cpp config.cpp dns.cpp worker.cpp
OBJS     := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Header dependencies
main.o:   main.cpp config.h dns.h stats.h worker.h clock.h
config.o: config.cpp config.h
dns.o:    dns.cpp dns.h
worker.o: worker.cpp worker.h config.h dns.h stats.h ratelimit.h clock.h

clean:
	rm -f $(OBJS) $(TARGET)
