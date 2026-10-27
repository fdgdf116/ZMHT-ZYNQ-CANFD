# PetaLinux/Yocto cross build.
# CC, CXX, CFLAGS, CXXFLAGS and LDFLAGS are supplied by the recipe.

TARGET = canfd_server

CXX_SOURCES = \
	src/main.cpp \
	src/network/network_main.cpp \
	src/network/tcp_socket.cpp \
	src/network/sync_send.cpp \
	src/network/thread.cpp \
	src/canfd/protocol.cpp \
	src/canfd/canfd_hw.cpp \
	src/canfd/canfd_codec.cpp \
	src/canfd/canfd_service.cpp \
	src/hardware/fifo_engine.cpp

C_SOURCES = src/hardware/ringbuffer.c

OBJDIR = build
CXX_OBJECTS = $(patsubst %.cpp,$(OBJDIR)/%.o,$(CXX_SOURCES))
C_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(C_SOURCES))

INCLUDES = -Isrc/network -Isrc/canfd -Isrc/hardware
CXXFLAGS += -std=c++14 -D_GNU_SOURCE -DLINUX $(INCLUDES) -pthread
CFLAGS += -D_GNU_SOURCE -DLINUX $(INCLUDES) -pthread
LDFLAGS += -static-libstdc++ -static-libgcc
LDLIBS += -pthread

all: $(TARGET)

$(TARGET): $(CXX_OBJECTS) $(C_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
