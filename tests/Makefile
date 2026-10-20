# PC tests: the PS1 editor + the Pico's bus logic and file server, with a
# folder as the SD card.   make && ./run_tests
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined
CPPFLAGS = -I../common -I../pico -I../ps1

SRC = test_main.c sio_host.c vfs_posix.c aiport_fake.c \
      ../pico/shared.c ../pico/bus_logic.c ../pico/fileserver.c ../pico/keymap.c ../pico/ai.c \
      ../ps1/link.c ../ps1/doc.c ../ps1/editor.c

LINK_SRC = ai_link_test.c vfs_posix.c ../pico/shared.c ../pico/fileserver.c ../pico/ai.c

all: run_tests ai_link_test

run_tests: $(SRC) $(wildcard ../common/*.h ../pico/*.h ../ps1/*.h)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC)

ai_link_test: $(LINK_SRC) $(wildcard ../common/*.h ../pico/*.h)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(LINK_SRC)

# the editor tests, then pc/ai_bridge.py against the Pico's AI link code
test: run_tests ai_link_test
	./run_tests
	python3 test_bridge.py

clean:
	rm -rf run_tests ai_link_test sd sd_bridge screen_*.bin out*.txt __pycache__

.PHONY: all test clean
