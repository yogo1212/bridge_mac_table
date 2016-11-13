NAME := bridge_mac_table

CFLAGS += -Werror -Wall -Wpedantic

LDFLAGS += -ljson-c


default: $(NAME)

$(NAME): main.c
	$(CC) $(LDFLAGS) -o $@ $< $(CFLAGS)

clean:
	rm -f $(NAME)