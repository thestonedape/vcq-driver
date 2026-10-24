KDIR ?= /lib/modules/$(shell uname -r)/build
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -pthread
CPPFLAGS += -Iinclude/uapi -Iuser
ifeq ($(STATIC),1)
LDFLAGS += -static
endif
BUILD := build

all: module user

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver modules

user: $(BUILD)/vcq_test $(BUILD)/vcq_bench

$(BUILD)/%: tests/%.cpp user/vcq.cpp user/vcq.hpp include/uapi/vcq.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< user/vcq.cpp -o $@ $(LDFLAGS)

$(BUILD):
	mkdir -p $@

# Loads the module on this machine and runs the tests (needs root).
check: all
	insmod driver/vcq.ko
	$(BUILD)/vcq_test; rc=$$?; rmmod vcq; exit $$rc

# Boots a stock Ubuntu kernel under QEMU inside Docker, for non-Linux hosts.
qemu-test:
	docker build -t vcq-dev tools
	docker run --rm -v "$(CURDIR):/src" vcq-dev sh tools/qemu-test.sh

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver clean
	rm -rf $(BUILD)

.PHONY: all module user check qemu-test clean
