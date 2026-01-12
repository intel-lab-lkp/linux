LIBS := -lcheck

test_utils: test_utils.c ../src/utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

check: test_utils
	./test_utils

clean:
	rm -f test_utils

.PHONY: check clean
