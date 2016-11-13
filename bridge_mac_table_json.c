#include <arpa/inet.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/if_bridge.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json-c/json.h>

// stolen from linux
#define BR_PORT_BITS    10
#define BR_MAX_PORTS    (1<<BR_PORT_BITS)

typedef struct {
	char n[BR_MAX_PORTS][IFNAMSIZ + 1];
} ifnames_by_port_t;

static void add_fdbs_to_json(struct __fdb_entry *fdb, size_t cnt, ifnames_by_port_t *ifnames_by_port, struct json_object *jlist)
{
	char macbuf[18];
	struct json_object *jobj;

	while (cnt-- > 0) {
		jobj = json_object_new_object();

		json_object_object_add(jobj, "ifname",
			json_object_new_string(ifnames_by_port->n[fdb->port_no | (fdb->port_hi << 8)]));

		json_object_object_add(jobj, "age",
			json_object_new_int(fdb->ageing_timer_value));

		json_object_object_add(jobj, "local",
			json_object_new_boolean(fdb->is_local));

		snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
				fdb->mac_addr[0], fdb->mac_addr[1], fdb->mac_addr[2], fdb->mac_addr[3], fdb->mac_addr[4], fdb->mac_addr[5]);
		macbuf[sizeof(macbuf) - 1] = '\0';

		json_object_object_add(jlist, macbuf, jobj);
	}
}

static bool fetch_ifnames(int br_sock, struct ifreq *ifr, ifnames_by_port_t *to)
{
	int port_ifindices[BR_MAX_PORTS];

	unsigned long args[4] = {
		BRCTL_GET_PORT_LIST,
		(uintptr_t) port_ifindices,
		sizeof(port_ifindices) / sizeof(port_ifindices[0]),
		0
	};
	ifr->ifr_data = &args;

	int port_count;

	port_count = ioctl(br_sock, SIOCDEVPRIVATE, ifr);
	if (port_count == -1) {
		printf("GET_PORT_LIST ouch %d %s\n", errno, strerror(errno));
		return false;
	}

	struct ifreq tmp;
	while (port_count-- > 0) {
		if (port_ifindices[port_count] == 0)
			continue;
		tmp.ifr_ifindex = port_ifindices[port_count];
		if (ioctl(br_sock, SIOCGIFNAME, &tmp) == -1) {
			fprintf(stderr, "SIOCGIFNAME ouch %s\n", strerror(errno));
			continue;
		}
		strncpy(to->n[port_count], tmp.ifr_name, IFNAMSIZ);
		to->n[port_count][IFNAMSIZ] = '\0';
	}

	return true;
}

static bool fetch_bridge_mac_table(const char *ifname, struct json_object *jlist)
{
	bool res = false;

	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd == -1) {
		fprintf(stderr, "open ouch: %s\n", strerror(errno));
		goto cleanup;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));

	strncpy((char *) ifr.ifr_name, ifname, strnlen(ifname, IFNAMSIZ));

	ifnames_by_port_t ifnames_by_port;
	if (!fetch_ifnames(fd, &ifr, &ifnames_by_port)) {
		goto cleanup_fd;
	}

	struct __fdb_entry entries[10];
	unsigned long args[4] = {
		BRCTL_GET_FDB_ENTRIES,
		(uintptr_t) &entries,
		sizeof(entries)/sizeof(entries[0]),
		0
	};

	ifr.ifr_data = &args;

	int fdb_count;

	do {
		fdb_count = ioctl(fd, SIOCDEVPRIVATE, &ifr);
		if (fdb_count == -1) {
			fprintf(stderr, "GET_FDB_ENTRIES ouch: %s\n", strerror(errno));
			goto cleanup_fd;
		}

		add_fdbs_to_json(entries, fdb_count, &ifnames_by_port, jlist);

		args[3] += args[2];
	} while (fdb_count > args[2]);

	res = true;

cleanup_fd:
	close(fd);

cleanup:
	return res;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "%s devname\n", argv[0]);
		return -1;
	}

	struct json_object *jlist = json_object_new_object();

	fetch_bridge_mac_table(argv[1], jlist);

	const char *str = json_object_to_json_string(jlist);
	puts(str);
	json_object_put(jlist);

	return 0;
}