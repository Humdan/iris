CC ?= gcc
CFLAGS ?= -O3 -ffast-math -march=native -mtune=native
iris_fb: iris_fb.c iris_widgets.h
	$(CC) $(CFLAGS) -o $@ $< -lm -lpthread
clean:
	rm -f iris_fb
.PHONY: clean
