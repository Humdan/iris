CC ?= gcc
CFLAGS ?= -O2 -ffast-math
iris_fb: iris_fb.c
	$(CC) $(CFLAGS) -o $@ $< -lm -lpthread
clean:
	rm -f iris_fb
.PHONY: clean
