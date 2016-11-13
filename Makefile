NAME := bridge_mac_table_json

CFLAGS += -Werror -Wall -Wpedantic

LDFLAGS += -ljson-c


default: $(NAME)

$(NAME): main.c
	$(CC) $(LDFLAGS) -o $@ $< $(CFLAGS)

clean:
	rm -f $(NAME)